#include "opennet/coordinator_client.hpp"

#include <curl/curl.h>

#include <chrono>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    std::size_t discard_response(char*, std::size_t size, std::size_t count, void*)
    {
        return size * count;
    }

    std::string json_escape(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (char character : value)
        {
            switch (character)
            {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            default: result.push_back(character); break;
            }
        }
        return result;
    }
}

namespace opennet
{
    coordinator_client::coordinator_client(config settings)
        : settings_{std::move(settings)}
    {
    }

    coordinator_client::~coordinator_client()
    {
        stop();
    }

    void coordinator_client::start()
    {
        if (settings_.directory_url.empty())
            return;
        if (settings_.directory_api_key.empty())
        {
            std::cerr
                << "directory registration disabled: no API key was configured\n";
            return;
        }
        stopping_.store(false);
        worker_ = std::thread([this] { run(); });
    }

    void coordinator_client::stop()
    {
        if (stopping_.exchange(true))
            return;
        condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    void coordinator_client::run()
    {
        ::curl_global_init(CURL_GLOBAL_DEFAULT);
        while (!stopping_.load())
        {
            if (!register_node())
                std::cerr << "failed to refresh traversal directory registration\n";

            std::unique_lock lock(mutex_);
            condition_.wait_for(
                lock,
                std::chrono::seconds(settings_.heartbeat_seconds),
                [this] { return stopping_.load(); });
        }
        ::curl_global_cleanup();
    }

    bool coordinator_client::register_node()
    {
        CURL* raw = ::curl_easy_init();
        if (!raw)
            return false;

        std::string alternate = settings_.alternate_advertised_address.empty()
            ? "null"
            : std::format(
                "\"{}\"",
                json_escape(settings_.alternate_advertised_address));
        std::string ipv6 = settings_.advertised_ipv6_address.empty()
            ? "null"
            : std::format("\"{}\"", json_escape(settings_.advertised_ipv6_address));
        std::string body = std::format(
            R"({{"name":"{}","ipv4Address":"{}","ipv6Address":{},"apiPort":{},"stunPort":{},"alternateStunPort":{},"alternateIPv4Address":{},"priority":100}})",
            json_escape(settings_.node_name),
            json_escape(settings_.advertised_address),
            ipv6,
            settings_.http_port,
            settings_.stun_port,
            settings_.alternate_stun_port,
            alternate);
        std::string api_header = "X-OpenNet-Api-Key: " + settings_.directory_api_key;
        curl_slist* headers{};
        headers = ::curl_slist_append(headers, "Content-Type: application/json");
        headers = ::curl_slist_append(headers, api_header.c_str());

        ::curl_easy_setopt(raw, CURLOPT_URL, settings_.directory_url.c_str());
        ::curl_easy_setopt(raw, CURLOPT_CUSTOMREQUEST, "PUT");
        ::curl_easy_setopt(raw, CURLOPT_HTTPHEADER, headers);
        ::curl_easy_setopt(raw, CURLOPT_POSTFIELDS, body.c_str());
        ::curl_easy_setopt(raw, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        ::curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        ::curl_easy_setopt(raw, CURLOPT_TIMEOUT_MS, 5000L);
        ::curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L);
        ::curl_easy_setopt(raw, CURLOPT_USERAGENT, "OpenNet.Traversal/0.1");
        ::curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, discard_response);

        CURLcode result = ::curl_easy_perform(raw);
        long status{};
        ::curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
        ::curl_slist_free_all(headers);
        ::curl_easy_cleanup(raw);
        return result == CURLE_OK && status >= 200 && status < 300;
    }
}
