#pragma once

#include <cstdint>
#include <string>

namespace opennet
{
    struct config
    {
        std::string bind_address{"0.0.0.0"};
        std::string advertised_address{};
        std::string alternate_bind_address{};
        std::string alternate_advertised_address{};
        std::string advertised_ipv6_address{};
        std::uint16_t stun_port{3478};
        std::uint16_t alternate_stun_port{3479};
        std::uint16_t http_port{48100};
        int probe_timeout_ms{2500};
        int max_probes_per_minute{60};
        std::string directory_url{};
        std::string directory_api_key{};
        std::string node_name{"OpenNet traversal node"};
        int heartbeat_seconds{60};
    };

    config parse_config(int argc, char** argv);
    std::string usage();
}
