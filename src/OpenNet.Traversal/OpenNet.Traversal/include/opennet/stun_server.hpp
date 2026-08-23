#pragma once

#include "opennet/config.hpp"
#include "opennet/advertised_addresses.hpp"
#include "opennet/socket.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace opennet
{
	class stun_server
	{
	public:
		stun_server(config settings, advertised_addresses_ptr advertised_addresses);
		~stun_server();
		stun_server(stun_server const&) = delete;
		stun_server& operator=(stun_server const&) = delete;

		void start();
		void stop();

	private:
		struct endpoint_socket
		{
			unique_socket socket;
			std::uint16_t port{};
			bool alternate_address{};
			bool alternate_port{};
		};

		void receive_loop(std::size_t endpoint_index);
		config settings_;
		advertised_addresses_ptr advertised_addresses_;
		std::vector<endpoint_socket> endpoints_;
		std::vector<std::thread> workers_;
		std::atomic<bool> stopping_{ false };
	};
}
