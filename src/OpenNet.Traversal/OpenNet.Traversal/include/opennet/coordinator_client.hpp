#pragma once

#include "opennet/config.hpp"
#include "opennet/advertised_addresses.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace opennet
{
	class coordinator_client
	{
	public:
		coordinator_client(config settings, advertised_addresses_ptr advertised_addresses);
		~coordinator_client();
		coordinator_client(coordinator_client const&) = delete;
		coordinator_client& operator=(coordinator_client const&) = delete;

		void start();
		void stop();

	private:
		void run();
		bool register_node();

		config settings_;
		advertised_addresses_ptr advertised_addresses_;
		std::string server_id_;
		std::thread worker_;
		std::atomic<bool> stopping_{ false };
		std::condition_variable condition_;
		std::mutex mutex_;
	};
}
