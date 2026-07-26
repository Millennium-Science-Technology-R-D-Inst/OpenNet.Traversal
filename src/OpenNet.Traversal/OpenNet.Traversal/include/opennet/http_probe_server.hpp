#pragma once

#include "opennet/config.hpp"
#include "opennet/socket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <sys/socket.h>

namespace opennet
{
    class http_probe_server
    {
    public:
        explicit http_probe_server(config settings);
        ~http_probe_server();
        http_probe_server(http_probe_server const&) = delete;
        http_probe_server& operator=(http_probe_server const&) = delete;

        void start();
        void stop();

    private:
        struct rate_bucket
        {
            std::chrono::steady_clock::time_point started;
            int count{};
        };

        bool consume_rate_limit(std::string const& client_address);
        void accept_loop();
        void handle_client(int client_socket, sockaddr_storage peer, socklen_t peer_length);

        config settings_;
        unique_socket listener_;
        std::thread worker_;
        std::mutex clients_mutex_;
        std::condition_variable clients_condition_;
        std::size_t active_clients_{};
        std::atomic<bool> stopping_{false};
        std::mutex rate_mutex_;
        std::unordered_map<std::string, rate_bucket> rate_buckets_;
    };
}
