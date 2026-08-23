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
	std::size_t collect_response(
		char* data,
		std::size_t size,
		std::size_t count,
		void* context)
	{
		std::size_t const length = size * count;
		static_cast<std::string*>(context)->append(data, length);
		return length;
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

	std::string extract_server_id(std::string_view json)
	{
		std::size_t key = json.find("\"id\"");
		if (key == std::string_view::npos)
			return {};
		std::size_t colon = json.find(':', key + 4);
		std::size_t quote = colon == std::string_view::npos
			? std::string_view::npos
			: json.find('"', colon + 1);
		std::size_t end = quote == std::string_view::npos
			? std::string_view::npos
			: json.find('"', quote + 1);
		if (end == std::string_view::npos)
			return {};
		return std::string{ json.substr(quote + 1, end - quote - 1) };
	}
}

namespace opennet
{
	coordinator_client::coordinator_client(
		config settings,
		advertised_addresses_ptr advertised_addresses)
		: settings_{ std::move(settings) },
		advertised_addresses_{ std::move(advertised_addresses) }
	{
	}

	coordinator_client::~coordinator_client()
	{
		stop();
	}

	void coordinator_client::start()
	{
		bool const registration_enabled = !settings_.directory_url.empty()
			&& !settings_.directory_api_key.empty();
		if (!registration_enabled)
		{
			std::cerr
				<< "directory registration disabled: configure both the URL and API key\n";
			if (!advertised_addresses_->is_dynamic())
				return;
		}
		stopping_.store(false);
		worker_ = std::thread([this]
							  {
								  run();
							  });
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
			advertised_addresses_->refresh();
			bool const registration_enabled = !settings_.directory_url.empty()
				&& !settings_.directory_api_key.empty();
			if (registration_enabled && !register_node())
				std::cerr << "failed to refresh traversal directory registration\n";

			std::unique_lock lock(mutex_);
			condition_.wait_for(
				lock,
				std::chrono::seconds(settings_.heartbeat_seconds),
				[this]
				{
					return stopping_.load();
				});
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
		advertised_address_snapshot const advertised = advertised_addresses_->snapshot();
		std::string const id = server_id_.empty()
			? "null"
			: std::format("\"{}\"", json_escape(server_id_));
		std::string ipv6 = advertised.ipv6.empty()
			? "null"
			: std::format("\"{}\"", json_escape(advertised.ipv6));
		std::string body = std::format(
			R"({{"id":{},"name":"{}","ipv4Address":"{}","ipv6Address":{},"apiPort":{},"stunPort":{},"alternateStunPort":{},"alternateIPv4Address":{},"priority":100}})",
			id,
			json_escape(settings_.node_name),
			json_escape(advertised.ipv4),
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
		std::string response_body;
		::curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, collect_response);
		::curl_easy_setopt(raw, CURLOPT_WRITEDATA, &response_body);

		CURLcode result = ::curl_easy_perform(raw);
		long status{};
		::curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
		::curl_slist_free_all(headers);
		::curl_easy_cleanup(raw);
		bool const succeeded = result == CURLE_OK && status >= 200 && status < 300;
		if (succeeded)
		{
			std::string returned_id = extract_server_id(response_body);
			if (!returned_id.empty())
				server_id_ = std::move(returned_id);
		}
		return succeeded;
	}
}
