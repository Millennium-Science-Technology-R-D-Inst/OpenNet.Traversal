#include "opennet/config.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include <arpa/inet.h>

namespace
{
    std::uint16_t parse_port(std::string_view value, std::string_view option)
    {
        unsigned int parsed{};
        auto const [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size()
            || parsed == 0 || parsed > 65535)
        {
            throw std::invalid_argument(std::string(option) + " requires a port from 1 to 65535");
        }
        return static_cast<std::uint16_t>(parsed);
    }

    int parse_positive_int(std::string_view value, std::string_view option)
    {
        int parsed{};
        auto const [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed < 1)
        {
            throw std::invalid_argument(std::string(option) + " requires a positive integer");
        }
        return parsed;
    }

    void validate_address(
        std::string const& value,
        int address_family,
        std::string_view option)
    {
        std::array<unsigned char, 16> storage{};
        if (::inet_pton(address_family, value.c_str(), storage.data()) != 1)
        {
            throw std::invalid_argument(
                std::string(option) + " requires a numeric IP address");
        }
    }
}

namespace opennet
{
    config parse_config(int argc, char** argv)
    {
        config result;
        if (char const* value = std::getenv("OPENNET_DIRECTORY_URL"))
            result.directory_url = value;
        if (char const* value = std::getenv("OPENNET_DIRECTORY_API_KEY"))
            result.directory_api_key = value;

        for (int index = 1; index < argc; ++index)
        {
            std::string_view option{argv[index]};
            if (option == "--help" || option == "-h")
                throw std::invalid_argument(usage());
            if (index + 1 >= argc)
                throw std::invalid_argument(std::string(option) + " requires a value");

            std::string_view value{argv[++index]};
            if (option == "--bind")
                result.bind_address = value;
            else if (option == "--advertise")
                result.advertised_address = value;
            else if (option == "--alternate-bind")
                result.alternate_bind_address = value;
            else if (option == "--alternate-advertise")
                result.alternate_advertised_address = value;
            else if (option == "--advertise-ipv6")
                result.advertised_ipv6_address = value;
            else if (option == "--stun-port")
                result.stun_port = parse_port(value, option);
            else if (option == "--alternate-stun-port")
                result.alternate_stun_port = parse_port(value, option);
            else if (option == "--http-port")
                result.http_port = parse_port(value, option);
            else if (option == "--probe-timeout-ms")
                result.probe_timeout_ms = parse_positive_int(value, option);
            else if (option == "--max-probes-per-minute")
                result.max_probes_per_minute = parse_positive_int(value, option);
            else if (option == "--directory-url")
                result.directory_url = value;
            else if (option == "--directory-api-key")
                result.directory_api_key = value;
            else if (option == "--node-name")
                result.node_name = value;
            else if (option == "--heartbeat-seconds")
                result.heartbeat_seconds = parse_positive_int(value, option);
            else
                throw std::invalid_argument("unknown option: " + std::string(option));
        }

        if (result.advertised_address.empty())
            result.advertised_address = result.bind_address;
        if (!result.alternate_bind_address.empty()
            && result.alternate_advertised_address.empty())
        {
            result.alternate_advertised_address = result.alternate_bind_address;
        }
        if (result.advertised_address == "0.0.0.0")
        {
            throw std::invalid_argument(
                "--advertise is required when --bind is 0.0.0.0");
        }
        if (result.stun_port == result.alternate_stun_port)
        {
            throw std::invalid_argument(
                "--stun-port and --alternate-stun-port must be different");
        }
        if (result.alternate_bind_address.empty()
            != result.alternate_advertised_address.empty())
        {
            throw std::invalid_argument(
                "--alternate-bind and --alternate-advertise must be configured together");
        }
        if (!result.alternate_bind_address.empty()
            && (result.alternate_bind_address == result.bind_address
                || result.alternate_advertised_address == result.advertised_address))
        {
            throw std::invalid_argument(
                "the RFC 5780 alternate address must differ from the primary address");
        }

        validate_address(result.bind_address, AF_INET, "--bind");
        validate_address(result.advertised_address, AF_INET, "--advertise");
        if (!result.alternate_bind_address.empty())
        {
            validate_address(
                result.alternate_bind_address,
                AF_INET,
                "--alternate-bind");
            validate_address(
                result.alternate_advertised_address,
                AF_INET,
                "--alternate-advertise");
        }
        if (!result.advertised_ipv6_address.empty())
        {
            validate_address(
                result.advertised_ipv6_address,
                AF_INET6,
                "--advertise-ipv6");
        }
        return result;
    }

    std::string usage()
    {
        return
            "OpenNet.Traversal options:\n"
            "  --bind <IPv4>                    primary bind address\n"
            "  --advertise <IPv4>               public primary address\n"
            "  --alternate-bind <IPv4>          second local address for RFC 5780\n"
            "  --alternate-advertise <IPv4>     public second address\n"
            "  --advertise-ipv6 <IPv6>          address used for IPv6 port probes\n"
            "  --stun-port <port>               primary STUN port (3478)\n"
            "  --alternate-stun-port <port>     alternate STUN port (3479)\n"
            "  --http-port <port>               probe API port (48100)\n"
            "  --probe-timeout-ms <milliseconds>\n"
            "  --max-probes-per-minute <count>\n"
            "  --directory-url <url>            OpenNet.Server directory endpoint\n"
            "  --directory-api-key <key>        prefer OPENNET_DIRECTORY_API_KEY\n"
            "  --node-name <name>\n"
            "  --heartbeat-seconds <seconds>\n";
    }
}
