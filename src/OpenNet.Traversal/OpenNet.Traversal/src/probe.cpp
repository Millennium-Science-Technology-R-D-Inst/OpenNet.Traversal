#include "opennet/probe.hpp"
#include "opennet/socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <random>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{
    using clock_type = std::chrono::steady_clock;

    void set_port(sockaddr_storage& address, std::uint16_t port)
    {
        if (address.ss_family == AF_INET)
            reinterpret_cast<sockaddr_in&>(address).sin_port = htons(port);
        else
            reinterpret_cast<sockaddr_in6&>(address).sin6_port = htons(port);
    }

    int elapsed_ms(clock_type::time_point start)
    {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clock_type::now() - start).count());
    }

    std::array<std::uint8_t, 68> make_bittorrent_handshake()
    {
        std::array<std::uint8_t, 68> packet{};
        packet[0] = 19;
        constexpr std::string_view protocol{"BitTorrent protocol"};
        std::copy(protocol.begin(), protocol.end(), packet.begin() + 1);

        std::random_device random;
        for (std::size_t index = 28; index < packet.size(); ++index)
            packet[index] = static_cast<std::uint8_t>(random());

        constexpr std::string_view peer_prefix{"-ON0001-"};
        std::copy(peer_prefix.begin(), peer_prefix.end(), packet.begin() + 48);
        return packet;
    }

    std::array<std::uint8_t, 20> make_utp_syn(std::uint16_t connection_id)
    {
        std::array<std::uint8_t, 20> packet{};
        packet[0] = 0x41; // ST_SYN (4), uTP version 1
        packet[2] = static_cast<std::uint8_t>(connection_id >> 8U);
        packet[3] = static_cast<std::uint8_t>(connection_id);

        std::random_device random;
        std::uint16_t sequence = static_cast<std::uint16_t>(random());
        packet[16] = static_cast<std::uint8_t>(sequence >> 8U);
        packet[17] = static_cast<std::uint8_t>(sequence);
        return packet;
    }

    bool same_endpoint(sockaddr_storage const& left, sockaddr_storage const& right)
    {
        if (left.ss_family != right.ss_family)
            return false;
        if (left.ss_family == AF_INET)
        {
            auto const& a = reinterpret_cast<sockaddr_in const&>(left);
            auto const& b = reinterpret_cast<sockaddr_in const&>(right);
            return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
        }

        auto const& a = reinterpret_cast<sockaddr_in6 const&>(left);
        auto const& b = reinterpret_cast<sockaddr_in6 const&>(right);
        return a.sin6_port == b.sin6_port
            && std::memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(in6_addr)) == 0;
    }
}

namespace opennet
{
    probe_result probe_bittorrent_tcp(
        sockaddr_storage const& original_target,
        socklen_t target_length,
        std::uint16_t port,
        int timeout_ms)
    {
        auto start = clock_type::now();
        sockaddr_storage target = original_target;
        set_port(target, port);

        unique_socket socket_handle{
            ::socket(target.ss_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
        if (!socket_handle)
            return {false, elapsed_ms(start), "socket-error"};

        int flags = ::fcntl(socket_handle.get(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket_handle.get(), F_SETFL, flags | O_NONBLOCK) < 0)
            return {false, elapsed_ms(start), "nonblocking-error"};

        int connected = ::connect(
            socket_handle.get(),
            reinterpret_cast<sockaddr const*>(&target),
            target_length);
        if (connected < 0 && errno != EINPROGRESS)
            return {false, elapsed_ms(start), "connection-refused"};

        pollfd descriptor{socket_handle.get(), POLLOUT, 0};
        if (::poll(&descriptor, 1, timeout_ms) <= 0)
            return {false, elapsed_ms(start), "timeout"};

        int socket_error{};
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(
                socket_handle.get(),
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_size) < 0
            || socket_error != 0)
        {
            return {false, elapsed_ms(start), "connection-refused"};
        }

        auto handshake = make_bittorrent_handshake();
        ssize_t sent = ::send(
            socket_handle.get(),
            handshake.data(),
            handshake.size(),
            MSG_NOSIGNAL);
        return {
            sent == static_cast<ssize_t>(handshake.size()),
            elapsed_ms(start),
            sent == static_cast<ssize_t>(handshake.size())
                ? "tcp-connected-and-bittorrent-handshake-sent"
                : "handshake-send-failed"};
    }

    probe_result probe_bittorrent_utp(
        sockaddr_storage const& original_target,
        socklen_t target_length,
        std::uint16_t port,
        int timeout_ms)
    {
        auto start = clock_type::now();
        sockaddr_storage target = original_target;
        set_port(target, port);

        unique_socket socket_handle{
            ::socket(target.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP)};
        if (!socket_handle)
            return {false, elapsed_ms(start), "socket-error"};

        std::random_device random;
        auto request = make_utp_syn(static_cast<std::uint16_t>(random()));
        ssize_t sent = ::sendto(
            socket_handle.get(),
            request.data(),
            request.size(),
            0,
            reinterpret_cast<sockaddr const*>(&target),
            target_length);
        if (sent != static_cast<ssize_t>(request.size()))
            return {false, elapsed_ms(start), "utp-syn-send-failed"};

        pollfd descriptor{socket_handle.get(), POLLIN, 0};
        if (::poll(&descriptor, 1, timeout_ms) <= 0)
            return {false, elapsed_ms(start), "timeout"};

        std::array<std::uint8_t, 1500> response{};
        sockaddr_storage sender{};
        socklen_t sender_length = sizeof(sender);
        ssize_t received = ::recvfrom(
            socket_handle.get(),
            response.data(),
            response.size(),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &sender_length);
        bool valid = received >= 20
            && same_endpoint(sender, target)
            && (response[0] & 0x0fU) == 1;
        return {
            valid,
            elapsed_ms(start),
            valid ? "utp-response-received" : "unexpected-udp-response"};
    }
}
