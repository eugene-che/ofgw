#include "gateway.hpp"

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <exception>


ip_port::ip_port(const std::string& str) {
	in_addr addr = {0};
	auto pos = str.find(':');
	if (pos == str.size()) throw std::invalid_argument("no ':' in supplied ip_port");
	if (!inet_pton(AF_INET, str.substr(0, pos).c_str(), &addr))
		throw std::invalid_argument("wrong format of ip address");
	port = std::stoi(str.substr(pos+1));
	ip = ntohl(addr.s_addr);
}

ip_port::ip_port(const sockaddr_in& addr) {
	if (addr.sin_family != AF_INET)
		throw std::invalid_argument("unknown sockaddr family");
	ip = ntohl(addr.sin_addr.s_addr);
	port = addr.sin_port;
}

ip_port::operator std::string() const {
	in_addr addr {htonl(ip)};
	char buffer[] = "xxx.xxx.xxx.xxx";
	inet_ntop(AF_INET, &addr, buffer, sizeof(buffer));
	return std::string(buffer) + ":" + std::to_string(port);
}

ip_port::operator sockaddr_in() const {
	sockaddr_in addr;
	addr.sin_addr.s_addr = htonl(ip);
	addr.sin_port = htons(port);
	addr.sin_family = AF_INET;
	return addr;
}


bool operator == (const ip_port& lhs, const ip_port& rhs) {
	return lhs.ip == rhs.ip && lhs.port == rhs.port;
}

std::ostream& operator <<(std::ostream &stream, const ip_port& ipp) {
	return stream << std::string(ipp);
}


void throw_sys_error(std::string&& prefix_message) {
	throw std::system_error(errno, std::system_category(), prefix_message);
}

int open_bind_listen(const ip_port& ipp) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) throw_sys_error("failed to open sock");

	const int value = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)))
		throw_sys_error("failed to set SO_REUSEADDR");

	sockaddr_in serv_addr = ipp;
	if (bind(fd, (sockaddr*) &serv_addr, sizeof(serv_addr)) < 0)
		throw_sys_error("failed to bind sock");

	const int BACKLOG_SIZE = 5;
	if (listen(fd, BACKLOG_SIZE) < 0)
		throw_sys_error("failed to listen sock");

	std::cout << "Start listening at " << ipp << std::endl;
	return fd;
}

std::pair<ip_port, int> accept_listener(listener_data* data) {
	sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int fd = accept4(data->fd, (sockaddr*)&addr, &addr_len, SOCK_CLOEXEC);
	if (fd < 0) throw_sys_error("failed to accept socket");
	ip_port ipp(addr);

	std::cout << "Trying to establish new connection from "
						<< ipp << " to " << data->ipp << std::endl;

	return {ipp, fd};
}

void add_to_epoll(int epoll_fd, listener_data* data) {
	epoll_event ev = {EPOLLIN | EPOLLONESHOT, data};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data->fd, &ev) < 0)
		throw_sys_error("failed to add fd to epoll");
}

void send_to_sock(int fd, uint8_t *buffer, size_t bytes_to_send) {
	while (bytes_to_send > 0) {
		// request not to send SIG_PIPE signal in case of connection close
		// this case will be handled by the negative return value
		int sent = send(fd, buffer, bytes_to_send, MSG_NOSIGNAL);
		if (sent < 0) throw_sys_error("failed to send message to socket");
		bytes_to_send -= sent;
		buffer += sent;
	}
}


void GateWay::request_termination() {
	std::lock_guard<std::mutex> lock(mut);
	if (state == RUNNING) {
		uint64_t value = 1;
		write(event_fd, &value, 8);
		state = STOPPING;
	}
}


GateWay::~GateWay() {
	std::lock_guard<std::mutex> lock(mut);
	if (state == RUNNING) state = STOPPING;
	if (worker.joinable()) worker.join();
}


void GateWay::clean_running_config() {
	if (epoll_fd >= 0) close(epoll_fd);
	if (event_fd >= 0) close(event_fd);

	for(auto& entry : dev_listeners) {
		listener_data* data = entry.second;
		if (data->fd >= 0) close(data->fd);
		data->fd = -1;
	}
	for(auto& entry : cli_listeners) {
		listener_data* data = entry.second;
		if (data->fd >= 0) close(data->fd);
		data->fd = -1;
	}

	for(auto& entry: clients) {
		close(entry->fd);
		delete entry;
	}

	transactinos.clear();
	clients.clear();

	if (device) {
		close(device->fd);
		delete device;
		device = nullptr;
	}
}


void GateWay::add_listener(std::unordered_map<ip_port, listener_data*>& listeners, listener_data* data) {
	{ // protect listener and state
		std::lock_guard<std::mutex> lock(mut);
		auto res = listeners.emplace(data->ipp, data);
		if (!res.second) return delete data;
		if (state != RUNNING) return;
	}

	data->fd = open_bind_listen(data->ipp);
	add_to_epoll(epoll_fd, data);
}

void GateWay::add_device_listener(const std::string& ipp) {
	add_listener(dev_listeners, new listener_data{DEV_LISTENER, ipp, -1});
}

void GateWay::add_client_listener(const std::string& ipp) {
	add_listener(cli_listeners, new listener_data{CLI_LISTENER, ipp, -1});
}


void GateWay::del_listener(std::unordered_map<ip_port, listener_data*>& listeners, ip_port ipp) {
	int listener_fd = -1;
	{ // protect listener and state
		std::lock_guard<std::mutex> lock(mut);
		auto iter = listeners.find(ipp);
		if (iter == listeners.cend()) return;
		listener_fd = (*iter).second->fd;
		listeners.erase(iter);
		if (state != RUNNING) return;
	}

	if (listener_fd >= 0) close(listener_fd);
	// epoll removes descriptor on its closing automatically
	// if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listener_fd, nullptr) < 0)
	//	 throw_sys_error("failed to add fd to epoll");
}

void GateWay::del_device_listener(const std::string& ipp) {
	del_listener(dev_listeners, {ipp});
}

void GateWay::del_client_listener(const std::string& ipp) {
	del_listener(cli_listeners, {ipp});
}


void GateWay::start(bool capture_thread) {
	std::lock_guard<std::mutex> lock(mut);
	if (state != OFF) return;

	try {
		epoll_fd = epoll_create1(EPOLL_CLOEXEC);
		if (epoll_fd < 0) throw_sys_error("failed to create epoll fd");
		event_fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE | EFD_CLOEXEC);
		if (event_fd < 0) throw_sys_error("failed to create event fd");

		epoll_event ev = {EPOLLIN, nullptr};
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev) < 0)
			throw_sys_error("failed to add fd to epoll");

		for(auto& entry : dev_listeners) {
			listener_data* data = entry.second;
			data->fd = open_bind_listen(data->ipp);
			add_to_epoll(epoll_fd, data);
		}

		for(auto& entry : cli_listeners) {
			listener_data* data = entry.second;
			data->fd = open_bind_listen(data->ipp);
			add_to_epoll(epoll_fd, data);
		}

		if (!capture_thread) {
			if (worker.joinable()) worker.join();
			worker = std::thread(&GateWay::main_loop_safe, this);
		} else {
			mut.unlock();
			main_loop_safe();
		}
	}

	catch (...) {
		clean_running_config();
		throw;
	}

	state = RUNNING;
}


void GateWay::main_loop_safe() {
	try { main_loop(); }
	catch(std::exception& e) {
		std::cout << e.what() << std::endl;
	}

	std::lock_guard<std::mutex> lock(mut);
	clean_running_config();
	state = OFF;
}


void GateWay::main_loop() {
	epoll_event events[MAX_EVENTS_PER_EPOLL];
	int rnum = 0;

	while (true) {
		// provide -1 timeout to block the thread indefinitely
		rnum = epoll_wait(epoll_fd, events, MAX_EVENTS_PER_EPOLL, -1);
		if (rnum < 0) throw_sys_error("worker thread failed to epoll");

		// check evalue to exclude possibility of its starvation
		uint64_t unused_value = 0; // any non-zero 64 bit integer
		if (read(event_fd, &unused_value, 8) == sizeof(uint64_t)) {
			break; // thread termination requested through event_fd
		} else if (errno != EAGAIN && errno != EWOULDBLOCK)
			throw_sys_error("worker thread failed to read event");

		// main loop of OF sock handling
		for(int i = 0; i < rnum; ++i) {
			auto data = (listener_data*)(events[i].data.ptr);
			if (data == nullptr) continue; // reserved for event_fd

			switch (data->type) {
				case DEVICE: if (!handle_negotiator_safe((negotiator_data*)data)) continue; break;
				case CLIENT: if (!handle_negotiator_safe((negotiator_data*)data)) continue; break;
				case CLI_LISTENER: handle_client_listener_fd((listener_data*)data); break;
				case DEV_LISTENER: handle_device_listener_fd((listener_data*)data); break;
			}

			// re-add handled socket to epoll
			events[i].events = EPOLLIN | EPOLLONESHOT;
			if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, data->fd, &events[i]) != 0) {
				if (errno == ENOENT) continue; // fd has been removed
				throw_sys_error("failed to modify sock at epoll");
			}
		}
	}
}


bool GateWay::handle_negotiator_safe(negotiator_data* entry) {
	try { handle_negotiator(entry);	}
	catch (std::exception& e) {
		std::lock_guard<std::mutex> lock(mut);
		switch (entry->type) {
			case DEVICE: remove_device(entry, &e); break;
			case CLIENT: remove_client(entry, &e); break;
			default: break;
		}
		return false;
	}
	return true;
}


void GateWay::remove_device(negotiator_data *unused, std::exception* e) {
	if (e) std::cout << "Disconnecting device -- " << e->what() << std::endl;
	std::cout << " disconnection all clients" << std::endl;
	while (clients.size()) remove_client(*clients.begin(), nullptr);

	close(device->fd);
	delete device;
	device = nullptr;
}

void GateWay::remove_client(negotiator_data* client, std::exception* e) {
	if (e) std::cout << "Disconnecting client -- " << e->what() << std::endl;
	auto iter = transactinos.begin(), iter_end = transactinos.end();
	while (iter != iter_end) {
		if (iter->second == client) {
			iter = transactinos.erase(iter);
		} else ++iter;
	}

	clients.erase(client);
	close(client->fd);
	delete client;
}


namespace of {
	struct ofp_header {
		uint8_t version;
		uint8_t type;
		uint16_t length;
		uint32_t xid;
	};

	enum ofp_type {
		OFPT_HELLO = 0,
		OFPT_ECHO_REQUEST = 2,
		OFPT_ECHO_REPLY = 3
	};
}


void GateWay::handle_negotiator(negotiator_data *entry) {
	// reserve a part of the buffer at the end to extract the length of
	// openflow messages when switching to front side of the ring buffer
	const size_t buf_size = entry->buffer.size() - sizeof(of::ofp_header);
	for(size_t i = 0; i < MAX_READS_PER_SESSION; ++i) {

		size_t tail = entry->head + entry->size;
		if (tail >= buf_size) tail -= buf_size;
		size_t space = buf_size - std::max(entry->size, tail);
		if (!space) throw std::runtime_error("buffer is full");

		int received = recv(entry->fd, &entry->buffer[tail], space, MSG_DONTWAIT);
		if (!received) throw std::runtime_error("connection reset by peer");
		if (received < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				throw_sys_error("failed to receive data");
			break;
		}

		entry->size += received;
		while (entry->size >= sizeof(of::ofp_header)) {
			const size_t left_to_end = buf_size - entry->head;
			if (left_to_end < sizeof(of::ofp_header)) {
				// copy the rest of ofp_header to a space after the end to get a proper length
				// it is rather cheap: actual implementation will copy only a single 8 byte word
				std::memcpy(&entry->buffer[buf_size], &entry->buffer[0], sizeof(of::ofp_header));
			}

			uint8_t *msg_buf = &entry->buffer[entry->head];
			size_t msg_size = ntohs( ((of::ofp_header*)msg_buf)->length );
			if (entry->size < msg_size) break; // read full messages only

			if (msg_size > left_to_end) {
				// allocate temporary buffer to assemble fragmented messages
				// into a single piece of continuos memory
				msg_buf = new uint8_t[msg_size];
				memcpy(msg_buf, &entry->buffer[entry->head], left_to_end);
				memcpy(msg_buf + left_to_end, &entry->buffer[0], msg_size - left_to_end);
			}

			handle_message(entry, msg_buf, msg_size);

			entry->size -= msg_size;
			entry->head += msg_size;
			if (entry->head > buf_size) delete msg_buf;
			if (entry->head >= buf_size) entry->head -= buf_size;
		}
	}
}


void GateWay::handle_message(negotiator_data* entry, uint8_t *buffer, size_t size) {
	size_t msg_type = ((of::ofp_header*)buffer)->type;
	switch (msg_type) {

		case of::OFPT_HELLO:
			((of::ofp_header*)buffer)->version = OF_VERSION;
			send_to_sock(entry->fd, buffer, size);
			break;

		case of::OFPT_ECHO_REQUEST:
			((of::ofp_header*)buffer)->type = of::OFPT_ECHO_REPLY;
			send_to_sock(entry->fd, buffer, size);
			break;

		default:
			std::lock_guard<std::mutex> lock(mut);
			switch (entry->type) {
				case DEVICE: handle_device_msg(entry, buffer, size); break;
				case CLIENT: handle_client_msg(entry, buffer, size); break;
				default: break;
			}
	}
}


void GateWay::handle_device_msg(negotiator_data* device, uint8_t *buffer, size_t size) {
	uint32_t xid = ((of::ofp_header*)buffer)->xid;
	auto iter = transactinos.find(xid);
	if (iter == transactinos.end()) return;
	auto client = iter->second;
	try {send_to_sock(client->fd, buffer, size);}
	catch(...) {}
}


void GateWay::handle_client_msg(negotiator_data* client, uint8_t *buffer, size_t size) {
	if (!device) return;
	uint32_t xid = ((of::ofp_header*)buffer)->xid;
	transactinos.emplace(xid, client);
	try {send_to_sock(device->fd, buffer, size);}
	catch(...) {}
}


void GateWay::handle_device_listener_fd(listener_data* dev_listener) {
	auto result = accept_listener(dev_listener);
	std::lock_guard<std::mutex> lock(mut);
	if (device) {
		std::cout << " rejecting device -- one more device is already connected." << std::endl;
		close(result.second);
	} else {
		std::cout << " accepting new device." << std::endl;
		device = new negotiator_data(DEVICE, result.first, result.second);
		device->buffer.resize(BUFFER_SIZE, 0);
		add_to_epoll(epoll_fd, device);
	}
}

void GateWay::handle_client_listener_fd(listener_data* cli_listener) {
	auto result = accept_listener(cli_listener);
	std::lock_guard<std::mutex> lock(mut);
	if (!device) {
		std::cout << " rejecting client -- no device connected." << std::endl;
		close(result.second);
	} else {
		std::cout << " accepting new client." << std::endl;
		auto client = new negotiator_data(CLIENT, result.first, result.second);
		client->buffer.resize(BUFFER_SIZE, 0);
		add_to_epoll(epoll_fd, client);
		clients.insert(client);
	}
}
