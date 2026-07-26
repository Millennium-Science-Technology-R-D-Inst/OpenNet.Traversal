#include "opennet/http_probe_server.hpp"
#include "opennet/probe.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{
    std::string address_to_string(sockaddr_storage const& address)
    {
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        void const* source{};
        if (address.ss_family == AF_INET)
            source = &reinterpret_cast<sockaddr_in const&>(address).sin_addr;
        else
            source = &reinterpret_cast<sockaddr_in6 const&>(address).sin6_addr;
        return ::inet_ntop(address.ss_family, source, buffer.data(), buffer.size())
            ? std::string(buffer.data())
            : std::string{};
    }

    std::optional<std::uint16_t> parse_port(std::string_view body)
    {
        std::size_t key = body.find("\"port\"");
        if (key == std::string_view::npos)
            return std::nullopt;
        std::size_t colon = body.find(':', key + 6);
        if (colon == std::string_view::npos)
            return std::nullopt;
        std::size_t begin = body.find_first_of("0123456789", colon + 1);
        if (begin == std::string_view::npos)
            return std::nullopt;
        std::size_t end = body.find_first_not_of("0123456789", begin);
        if (end == std::string_view::npos)
            end = body.size();

        unsigned int value{};
        auto const [parsed_end, error] =
            std::from_chars(body.data() + begin, body.data() + end, value);
        if (error != std::errc{} || parsed_end != body.data() + end
            || value == 0 || value > 65535)
        {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(value);
    }

    std::optional<std::size_t> content_length(std::string_view headers)
    {
        std::string normalized{headers};
        for (char& character : normalized)
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));

        constexpr std::string_view name{"\r\ncontent-length:"};
        std::size_t position = normalized.find(name);
        if (position == std::string::npos)
            return 0;
        position += name.size();
        while (position < normalized.size()
            && std::isspace(static_cast<unsigned char>(normalized[position])))
        {
            ++position;
        }

        std::size_t end = normalized.find("\r\n", position);
        if (end == std::string::npos)
            return std::nullopt;
        std::size_t value{};
        auto const [parsed_end, error] =
            std::from_chars(normalized.data() + position, normalized.data() + end, value);
        if (error != std::errc{} || parsed_end != normalized.data() + end)
            return std::nullopt;
        return value;
    }

    void send_response(
        int socket_handle,
        int status,
        std::string_view reason,
        std::string_view body)
    {
        std::string response = std::format(
            "HTTP/1.1 {} {}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: {}\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n{}",
            status,
            reason,
            body.size(),
            body);
        ::send(socket_handle, response.data(), response.size(), MSG_NOSIGNAL);
    }

    std::string json_escape(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (char character : value)
        {
            if (character == '"' || character == '\\')
                result.push_back('\\');
            result.push_back(character);
        }
        return result;
    }
}

namespace opennet
{
    http_probe_server::http_probe_server(config settings)
        : settings_{std::move(settings)}
    {
    }

    http_probe_server::~http_probe_server()
    {
        stop();
    }

    void http_probe_server::start()
    {
        stopping_.store(false);
        listener_.reset(::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));

        int reuse = 1;
        if (listener_)
        {
            int dual_stack = 0;
            ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            ::setsockopt(
                listener_.get(),
                IPPROTO_IPV6,
                IPV6_V6ONLY,
                &dual_stack,
                sizeof(dual_stack));

            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_any;
            address.sin6_port = htons(settings_.http_port);
            if (::bind(
                    listener_.get(),
                    reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) < 0)
            {
                throw_socket_error("bind dual-stack HTTP socket");
            }
        }
        else
        {
            listener_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
            if (!listener_)
                throw_socket_error("HTTP socket");
            ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(settings_.http_port);
            if (::bind(
                    listener_.get(),
                    reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) < 0)
            {
                throw_socket_error("bind IPv4 HTTP socket");
            }
        }
        if (::listen(listener_.get(), 128) < 0)
            throw_socket_error("listen HTTP socket");

        worker_ = std::thread([this] { accept_loop(); });
    }

    void http_probe_server::stop()
    {
        if (stopping_.exchange(true))
            return;
        if (listener_)
            ::shutdown(listener_.get(), SHUT_RDWR);
        listener_.reset();
        if (worker_.joinable())
            worker_.join();
        std::unique_lock lock(clients_mutex_);
        clients_condition_.wait(lock, [this] { return active_clients_ == 0; });
    }

    bool http_probe_server::consume_rate_limit(std::string const& client_address)
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(rate_mutex_);
        rate_bucket& bucket = rate_buckets_[client_address];
        if (bucket.started.time_since_epoch().count() == 0
            || now - bucket.started >= std::chrono::minutes(1))
        {
            bucket = {now, 1};
            return true;
        }
        if (bucket.count >= settings_.max_probes_per_minute)
            return false;
        ++bucket.count;
        return true;
    }

    void http_probe_server::accept_loop()
    {
        while (!stopping_.load())
        {
            sockaddr_storage peer{};
            socklen_t peer_length = sizeof(peer);
            int client = ::accept4(
                listener_.get(),
                reinterpret_cast<sockaddr*>(&peer),
                &peer_length,
                SOCK_CLOEXEC);
            if (client < 0)
            {
                if (!stopping_.load())
                    std::cerr << "HTTP accept failed: " << std::strerror(errno) << '\n';
                continue;
            }

            timeval receive_timeout{5, 0};
            ::setsockopt(
                client,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &receive_timeout,
                sizeof(receive_timeout));

            {
                std::lock_guard lock(clients_mutex_);
                ++active_clients_;
            }
            std::thread(
                [this, client, peer, peer_length]
                {
                    try
                    {
                        handle_client(client, peer, peer_length);
                    }
                    catch (std::exception const& error)
                    {
                        std::cerr << "HTTP client failed: " << error.what() << '\n';
                    }
                    ::close(client);
                    {
                        std::lock_guard lock(clients_mutex_);
                        --active_clients_;
                    }
                    clients_condition_.notify_all();
                })
                .detach();
        }
    }

    void http_probe_server::handle_client(
        int client_socket,
        sockaddr_storage peer,
        socklen_t peer_length)
    {
        constexpr std::size_t maximum_request_size = 16 * 1024;
        std::array<char, 4096> buffer{};
        std::string request_storage;
        request_storage.reserve(buffer.size());
        while (request_storage.size() < maximum_request_size)
        {
            ssize_t received =
                ::recv(client_socket, buffer.data(), buffer.size(), 0);
            if (received <= 0)
                return;
            request_storage.append(buffer.data(), static_cast<std::size_t>(received));

            std::size_t body_start = request_storage.find("\r\n\r\n");
            if (body_start == std::string::npos)
                continue;
            auto length = content_length(
                std::string_view{request_storage}.substr(0, body_start));
            if (!length || *length > maximum_request_size - body_start - 4)
            {
                send_response(
                    client_socket,
                    400,
                    "Bad Request",
                    R"({"error":"invalid-content-length"})");
                return;
            }
            if (request_storage.size() >= body_start + 4 + *length)
                break;
        }
        if (request_storage.size() >= maximum_request_size)
        {
            send_response(
                client_socket,
                413,
                "Content Too Large",
                R"({"error":"request-too-large"})");
            return;
        }

        std::string_view request{request_storage};
        std::size_t line_end = request.find("\r\n");
        if (line_end == std::string_view::npos)
        {
            send_response(client_socket, 400, "Bad Request", R"({"error":"invalid-request"})");
            return;
        }

        std::string_view request_line = request.substr(0, line_end);
        if (request_line == "GET /health HTTP/1.1")
        {
            send_response(client_socket, 200, "OK", R"({"status":"healthy"})");
            return;
        }

        bool tcp = request_line == "POST /v1/probes/tcp HTTP/1.1";
        bool udp = request_line == "POST /v1/probes/udp HTTP/1.1";
        if (!tcp && !udp)
        {
            send_response(client_socket, 404, "Not Found", R"({"error":"not-found"})");
            return;
        }

        std::string peer_address = address_to_string(peer);
        if (!consume_rate_limit(peer_address))
        {
            send_response(
                client_socket,
                429,
                "Too Many Requests",
                R"({"error":"rate-limit-exceeded"})");
            return;
        }

        std::size_t body_start = request.find("\r\n\r\n");
        std::optional<std::uint16_t> port = body_start == std::string_view::npos
            ? std::nullopt
            : parse_port(request.substr(body_start + 4));
        if (!port)
        {
            send_response(
                client_socket,
                400,
                "Bad Request",
                R"({"error":"a-port-from-1-to-65535-is-required"})");
            return;
        }

        if (peer.ss_family == AF_INET6)
        {
            auto const& ipv6 = reinterpret_cast<sockaddr_in6 const&>(peer);
            if (IN6_IS_ADDR_V4MAPPED(&ipv6.sin6_addr))
            {
                sockaddr_in ipv4{};
                ipv4.sin_family = AF_INET;
                std::memcpy(&ipv4.sin_addr, &ipv6.sin6_addr.s6_addr[12], 4);
                peer = {};
                std::memcpy(&peer, &ipv4, sizeof(ipv4));
                peer_length = sizeof(ipv4);
                peer_address = address_to_string(peer);
            }
        }

        probe_result result = tcp
            ? probe_bittorrent_tcp(peer, peer_length, *port, settings_.probe_timeout_ms)
            : probe_bittorrent_utp(peer, peer_length, *port, settings_.probe_timeout_ms);

        std::string body = std::format(
            R"({{"targetAddress":"{}","targetPort":{},"transport":"{}","reachable":{},"latencyMs":{},"evidence":"{}"}})",
            json_escape(peer_address),
            *port,
            tcp ? "tcp" : "udp",
            result.reachable ? "true" : "false",
            result.latency_ms,
            json_escape(result.evidence));
        send_response(client_socket, 200, "OK", body);
    }
}
