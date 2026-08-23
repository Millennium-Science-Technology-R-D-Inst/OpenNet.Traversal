#include "opennet/config.hpp"
#include "opennet/coordinator_client.hpp"
#include "opennet/advertised_addresses.hpp"
#include "opennet/http_probe_server.hpp"
#include "opennet/stun_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>

namespace
{
	volatile std::sig_atomic_t stop_requested = 0;

	void signal_handler(int)
	{
		stop_requested = 1;
	}
}

int main(int argc, char** argv)
{
	try
	{
		opennet::config settings = opennet::parse_config(argc, argv);
		auto advertised_addresses = std::make_shared<opennet::advertised_addresses>(settings);
		std::signal(SIGINT, signal_handler);
		std::signal(SIGTERM, signal_handler);

		opennet::stun_server stun{ settings, advertised_addresses };
		opennet::http_probe_server probes{ settings };
		opennet::coordinator_client coordinator{ settings, advertised_addresses };
		stun.start();
		probes.start();
		coordinator.start();

		std::cout
			<< "OpenNet.Traversal listening: STUN "
			<< settings.bind_address << ':' << settings.stun_port
			<< '/' << settings.alternate_stun_port
			<< ", probe API [::]:" << settings.http_port << '\n';

		while (!stop_requested)
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		coordinator.stop();
		probes.stop();
		stun.stop();
		return 0;
	}
	catch (std::invalid_argument const& exception)
	{
		std::cerr << exception.what() << '\n';
		return 2;
	}
	catch (std::exception const& exception)
	{
		std::cerr << "fatal: " << exception.what() << '\n';
		return 1;
	}
}
