#pragma once

#include <netinet/in.h>

#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>


struct ip_port {
	uint32_t ip;
	uint16_t port;

	ip_port() = default;
	ip_port(uint32_t ip, uint16_t port) : ip(ip), port(port) {}

	ip_port(const sockaddr_in& addr);
	ip_port(const std::string& str);

	operator std::string() const;
	operator sockaddr_in() const;
};

bool operator == (const ip_port& lhs, const ip_port& rhs);
std::ostream& operator <<(std::ostream &stream, const ip_port& ipp);

namespace std {
	template<> struct hash<ip_port> {
			size_t operator()(const ip_port& ipp) const {
				return ipp.ip + (uint64_t(ipp.port) << 32);
			}
	};
}


enum socket_type {DEV_LISTENER, DEVICE, CLI_LISTENER, CLIENT};

struct listener_data {
	listener_data(socket_type type, ip_port ipp, int fd) :
		type(type), ipp(ipp), fd(fd) {}

	socket_type type;
	ip_port ipp;
	int fd;
};

struct negotiator_data : public listener_data {
	using listener_data::listener_data;

	std::vector<uint8_t> buffer;
	size_t head = 0, size = 0;
};


class GateWay {
	const static size_t MAX_READS_PER_SESSION = 10;
	const static size_t MAX_EVENTS_PER_EPOLL = 3;
	const static size_t BUFFER_SIZE = 0xFFFF + 8;
	const static size_t OF_VERSION = 0x4;

	std::unordered_map<uint32_t, negotiator_data*> transactinos;
	std::unordered_map<ip_port, listener_data*> dev_listeners;
	std::unordered_map<ip_port, listener_data*> cli_listeners;
	std::unordered_set<negotiator_data*> clients;
	negotiator_data* device = nullptr;

	enum proxy_state {OFF, RUNNING, STOPPING};
	proxy_state state = OFF;
	int event_fd, epoll_fd;
	std::thread worker;
	std::mutex mut;

	void add_listener(std::unordered_map<ip_port, listener_data*>& listeners, listener_data* data);
	void del_listener(std::unordered_map<ip_port, listener_data*>& listeners, ip_port pp);

	bool handle_negotiator_safe(negotiator_data* entry);
	void handle_negotiator(negotiator_data* entry);

	void remove_device(negotiator_data* device, std::exception* e);
	void remove_client(negotiator_data* client, std::exception* e);

	void handle_message(negotiator_data* device, uint8_t *buffer, size_t size);
	void handle_device_msg(negotiator_data* device, uint8_t *buffer, size_t size);
	void handle_client_msg(negotiator_data* client, uint8_t *buffer, size_t size);

	void handle_device_listener_fd(listener_data* dev_listener);
	void handle_client_listener_fd(listener_data* cli_listener);

	void clean_running_config();
	void main_loop_safe();
	void main_loop();

public:

	void start(bool capture_thread);
	void request_termination();

	void add_device_listener(const std::string& ip_port);
	void add_client_listener(const std::string& ip_port);

	void del_device_listener(const std::string& ip_port);
	void del_client_listener(const std::string& ip_port);

	~GateWay();
};
