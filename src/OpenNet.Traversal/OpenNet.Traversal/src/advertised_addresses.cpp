#include "opennet/advertised_addresses.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

#include <arpa/inet.h>

namespace
{
    std::string numeric_address(sockaddr const* address)
    {
        std::array<char, INET6_ADDRSTRLEN> text{};
        void const* bytes{};
        if (address->sa_family == AF_INET)
        {
            bytes = &reinterpret_cast<sockaddr_in const*>(address)->sin_addr;
        }
        else if (address->sa_family == AF_INET6)
        {
            bytes = &reinterpret_cast<sockaddr_in6 const*>(address)->sin6_addr;
        }
        else
        {
            return {};
        }

        if (::inet_ntop(
                address->sa_family,
                bytes,
                text.data(),
                static_cast<socklen_t>(text.size())) == nullptr)
            return {};
        return text.data();
    }
}

namespace opennet
{
    advertised_addresses::advertised_addresses(config const& settings)
        : hostname_{settings.advertised_hostname},
          alternate_ipv4_{settings.alternate_advertised_address},
          current_{settings.advertised_address, settings.advertised_ipv6_address}
    {
        if (!hostname_.empty())
        {
            current_ = resolve_hostname();
            if (current_.ipv4.empty())
            {
                throw std::runtime_error(
                    "--advertise-hostname did not resolve to an IPv4 address: "
                    + hostname_);
            }
            if (!alternate_ipv4_.empty() && current_.ipv4 == alternate_ipv4_)
            {
                throw std::runtime_error(
                    "--advertise-hostname resolved to the configured alternate IPv4 address");
            }
        }
    }

    advertised_address_snapshot advertised_addresses::snapshot() const
    {
        std::shared_lock lock{mutex_};
        return current_;
    }

    bool advertised_addresses::refresh()
    {
        if (hostname_.empty())
            return false;

        advertised_address_snapshot resolved;
        try
        {
            resolved = resolve_hostname();
        }
        catch (std::exception const& exception)
        {
            std::cerr << "DDNS refresh failed for " << hostname_ << ": "
                      << exception.what() << "; retaining last known addresses\n";
            return false;
        }

        std::unique_lock lock{mutex_};
        if (resolved.ipv4.empty())
        {
            std::cerr << "DDNS refresh returned no IPv4 address for " << hostname_
                      << "; retaining " << current_.ipv4 << '\n';
            return false;
        }
        if (!alternate_ipv4_.empty() && resolved.ipv4 == alternate_ipv4_)
        {
            std::cerr << "DDNS refresh resolved the primary hostname to its RFC 5780 "
                         "alternate IPv4 address; retaining last known addresses\n";
            return false;
        }

        if (resolved.ipv4 == current_.ipv4 && resolved.ipv6 == current_.ipv6)
            return false;

        std::cerr << "DDNS advertised addresses changed for " << hostname_
                  << ": IPv4 " << current_.ipv4 << " -> " << resolved.ipv4;
        if (resolved.ipv6 != current_.ipv6)
        {
            std::cerr << ", IPv6 "
                      << (current_.ipv6.empty() ? "<none>" : current_.ipv6)
                      << " -> "
                      << (resolved.ipv6.empty() ? "<none>" : resolved.ipv6);
        }
        std::cerr << '\n';
        current_ = std::move(resolved);
        return true;
    }

    bool advertised_addresses::is_dynamic() const noexcept
    {
        return !hostname_.empty();
    }

    advertised_address_snapshot advertised_addresses::resolve_hostname() const
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* raw{};
        int result = ::getaddrinfo(hostname_.c_str(), nullptr, &hints, &raw);
        if (result != 0)
            throw std::runtime_error(::gai_strerror(result));

        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses{
            raw,
            &::freeaddrinfo};
        advertised_address_snapshot resolved;
        for (addrinfo const* item = addresses.get(); item != nullptr; item = item->ai_next)
        {
            std::string value = numeric_address(item->ai_addr);
            if (item->ai_family == AF_INET && resolved.ipv4.empty())
                resolved.ipv4 = std::move(value);
            else if (item->ai_family == AF_INET6 && resolved.ipv6.empty())
                resolved.ipv6 = std::move(value);
        }
        return resolved;
    }
}
