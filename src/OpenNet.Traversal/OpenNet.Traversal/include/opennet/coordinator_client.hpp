#pragma once

#include "opennet/config.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace opennet
{
    class coordinator_client
    {
    public:
        explicit coordinator_client(config settings);
        ~coordinator_client();
        coordinator_client(coordinator_client const&) = delete;
        coordinator_client& operator=(coordinator_client const&) = delete;

        void start();
        void stop();

    private:
        void run();
        bool register_node();

        config settings_;
        std::thread worker_;
        std::atomic<bool> stopping_{false};
        std::condition_variable condition_;
        std::mutex mutex_;
    };
}
