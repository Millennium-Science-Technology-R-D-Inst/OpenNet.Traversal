#include "opennet/stun_server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <vector>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{
    constexpr std::uint32_t stun_magic_cookie = 0x2112A442;
    constexpr std::uint16_t binding_request = 0x0001;
    constexpr std::uint16_t binding_success = 0x0101;
    constexpr std::uint16_t attr_change_request = 0x0003;
    constexpr std::uint16_t attr_xor_mapped_address = 0x0020;
    constexpr std::uint16_t attr_response_origin = 0x802b;
    constexpr std::uint16_t attr_other_address = 0x802c;

    std::uint16_t read_u16(std::uint8_t const* value)
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(value[0]) << 8U) | value[1]);
    }

    std::uint32_t read_u32(std::uint8_t const* value)
    {
        return (static_cast<std::uint32_t>(value[0]) << 24U)
            | (static_cast<std::uint32_t>(value[1]) << 16U)
            | (static_cast<std::uint32_t>(value[2]) << 8U)
            | value[3];
    }

    void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
    {
        output.push_back(static_cast<std::uint8_t>(value >> 8U));
        output.push_back(static_cast<std::uint8_t>(value));
    }

    void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value)
    {
        output.push_back(static_cast<std::uint8_t>(value >> 24U));
        output.push_back(static_cast<std::uint8_t>(value >> 16U));
        output.push_back(static_cast<std::uint8_t>(value >> 8U));
        output.push_back(static_cast<std::uint8_t>(value));
    }

    void append_address_attribute(
        std::vector<std::uint8_t>& output,
        std::uint16_t type,
        sockaddr_in const& address,
        bool xor_address)
    {
        append_u16(output, type);
        append_u16(output, 8);
        output.push_back(0);
        output.push_back(1);
        std::uint16_t port = ntohs(address.sin_port);
        std::uint32_t ipv4 = ntohl(address.sin_addr.s_addr);
        if (xor_address)
        {
            port ^= static_cast<std::uint16_t>(stun_magic_cookie >> 16U);
            ipv4 ^= stun_magic_cookie;
        }
        append_u16(output, port);
        append_u32(output, ipv4);
    }

    sockaddr_in make_address(std::string const& ipv4, std::uint16_t port)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, ipv4.c_str(), &address.sin_addr) != 1)
            throw std::invalid_argument("invalid IPv4 address: " + ipv4);
        return address;
    }

    opennet::unique_socket bind_udp(std::string const& ipv4, std::uint16_t port)
    {
        opennet::unique_socket socket_handle{
            ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP)};
        if (!socket_handle)
            opennet::throw_socket_error("socket");

        int reuse = 1;
        ::setsockopt(socket_handle.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address = make_address(ipv4, port);
        if (::bind(
                socket_handle.get(),
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0)
        {
            opennet::throw_socket_error("bind STUN socket");
        }
        return socket_handle;
    }
}

namespace opennet
{
    stun_server::stun_server(config settings)
        : settings_{std::move(settings)}
    {
    }

    stun_server::~stun_server()
    {
        stop();
    }

    void stun_server::start()
    {
        stopping_.store(false);

        auto add_pair = [this](
                            std::string const& bind_address,
                            std::string const& advertised_address,
                            bool alternate_address)
        {
            endpoints_.push_back({
                bind_udp(bind_address, settings_.stun_port),
                advertised_address,
                settings_.stun_port,
                alternate_address,
                false});
            endpoints_.push_back({
                bind_udp(bind_address, settings_.alternate_stun_port),
                advertised_address,
                settings_.alternate_stun_port,
                alternate_address,
                true});
        };

        add_pair(settings_.bind_address, settings_.advertised_address, false);
        if (!settings_.alternate_bind_address.empty())
        {
            add_pair(
                settings_.alternate_bind_address,
                settings_.alternate_advertised_address,
                true);
        }

        for (std::size_t index = 0; index < endpoints_.size(); ++index)
            workers_.emplace_back([this, index] { receive_loop(index); });
    }

    void stun_server::stop()
    {
        if (stopping_.exchange(true))
            return;

        for (endpoint_socket& endpoint : endpoints_)
        {
            if (endpoint.socket)
                ::shutdown(endpoint.socket.get(), SHUT_RDWR);
        }
        for (std::thread& worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
        workers_.clear();
        for (endpoint_socket& endpoint : endpoints_)
            endpoint.socket.reset();
        endpoints_.clear();
    }

    void stun_server::receive_loop(std::size_t endpoint_index)
    {
        std::array<std::uint8_t, 1500> request{};
        endpoint_socket const& incoming = endpoints_[endpoint_index];

        while (!stopping_.load())
        {
            pollfd descriptor{incoming.socket.get(), POLLIN, 0};
            int poll_result = ::poll(&descriptor, 1, 250);
            if (poll_result <= 0)
                continue;

            sockaddr_in client{};
            socklen_t client_length = sizeof(client);
            ssize_t received = ::recvfrom(
                incoming.socket.get(),
                request.data(),
                request.size(),
                0,
                reinterpret_cast<sockaddr*>(&client),
                &client_length);
            if (received < 0)
            {
                if (!stopping_.load())
                    std::cerr << "STUN receive failed: " << std::strerror(errno) << '\n';
                continue;
            }
            if (received < 20
                || read_u16(request.data()) != binding_request
                || read_u32(request.data() + 4) != stun_magic_cookie)
            {
                continue;
            }

            std::size_t message_length = read_u16(request.data() + 2);
            if (20 + message_length > static_cast<std::size_t>(received))
                continue;

            bool change_ip = false;
            bool change_port = false;
            for (std::size_t offset = 20; offset + 4 <= 20 + message_length;)
            {
                std::uint16_t type = read_u16(request.data() + offset);
                std::uint16_t length = read_u16(request.data() + offset + 2);
                if (offset + 4 + length > 20 + message_length)
                    break;
                if (type == attr_change_request && length == 4)
                {
                    std::uint32_t flags = read_u32(request.data() + offset + 4);
                    change_ip = (flags & 0x04U) != 0;
                    change_port = (flags & 0x02U) != 0;
                }
                offset += 4 + ((length + 3U) & ~3U);
            }

            bool desired_address = incoming.alternate_address ^ change_ip;
            bool desired_port = incoming.alternate_port ^ change_port;
            auto selected = std::find_if(
                endpoints_.begin(),
                endpoints_.end(),
                [desired_address, desired_port](endpoint_socket const& endpoint)
                {
                    return endpoint.alternate_address == desired_address
                        && endpoint.alternate_port == desired_port;
                });
            if (selected == endpoints_.end())
                continue;

            endpoint_socket const& other =
                endpoints_.size() >= 4
                    ? endpoints_[
                        (incoming.alternate_address ? 0U : 2U)
                        + (incoming.alternate_port ? 0U : 1U)]
                    : endpoints_[incoming.alternate_port ? 0U : 1U];

            std::vector<std::uint8_t> response;
            response.reserve(64);
            append_u16(response, binding_success);
            append_u16(response, 0);
            append_u32(response, stun_magic_cookie);
            response.insert(response.end(), request.begin() + 8, request.begin() + 20);

            append_address_attribute(response, attr_xor_mapped_address, client, true);
            sockaddr_in origin = make_address(selected->advertised_address, selected->port);
            append_address_attribute(response, attr_response_origin, origin, false);
            sockaddr_in other_address =
                make_address(other.advertised_address, other.port);
            append_address_attribute(response, attr_other_address, other_address, false);

            std::size_t attributes_size = response.size() - 20;
            response[2] = static_cast<std::uint8_t>(attributes_size >> 8U);
            response[3] = static_cast<std::uint8_t>(attributes_size);
            ::sendto(
                selected->socket.get(),
                response.data(),
                response.size(),
                0,
                reinterpret_cast<sockaddr*>(&client),
                client_length);
        }
    }
}
