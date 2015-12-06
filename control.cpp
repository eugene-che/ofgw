#include <iostream>

#include "gateway.hpp"

#include <boost/program_options.hpp>
namespace po = boost::program_options;

// GateWay keeps a long term connection with a switch by prentending to be an OpenFlow controller.
// Thus clients may use GateWay to connect the switch and send it arbitrary commands or requests.

// GatWay listens for new device and client connection at all the specified addresses at once.
// It may keep any number of clients, however supports only one device connection at a time.

int main(int argc, char** argv) {
	po::options_description desc(
			"Start OpenFlow GateWay to enable requests"
			" to a switch without passive connection"
	);

	po::variables_map vm;
	desc.add_options()
		("help,h", "produce help message")
		("dev_listener,d",
		 po::value<std::vector<std::string>>()->multitoken()->default_value(
			 std::vector<std::string>{"127.0.0.1:6654"}, "127.0.0.1:6654"),
		 "'ip:port' OpenFlow device would connect")
		("cli_listener,c",
		 po::value<std::vector<std::string>>()->multitoken()->default_value(
			 std::vector<std::string>{"127.0.0.1:6653"}, "127.0.0.1:6653"),
		 "'ip:port' OpenFlow client would connect")
	;

	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	} catch (...) {
		std::cerr << desc << std::endl;
		return 1;
	}

	if (vm.count("help")) {
		std::cerr << desc << std::endl;
		return 1;
	}

	try {
		GateWay gw;
		auto dev_listeners = vm["dev_listener"].as<std::vector<std::string>>();
		for(auto& listener : dev_listeners) gw.add_device_listener(listener);
		auto cli_listeners = vm["cli_listener"].as<std::vector<std::string>>();
		for(auto& listener : cli_listeners) gw.add_client_listener(listener);
		gw.start(true); // capture main thread
	}
	catch (std::exception& e) {
		std::cerr << e.what() << std::endl;
		return 2;
	}

	return 0;
}
