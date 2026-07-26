#pragma once

#include <cstdint>
#include <string>
#include <sys/socket.h>

namespace opennet
{
    struct probe_result
    {
        bool reachable{};
        int latency_ms{};
        std::string evidence;
    };

    probe_result probe_bittorrent_tcp(
        sockaddr_storage const& target,
        socklen_t target_length,
        std::uint16_t port,
        int timeout_ms);

    probe_result probe_bittorrent_utp(
        sockaddr_storage const& target,
        socklen_t target_length,
        std::uint16_t port,
        int timeout_ms);
}
