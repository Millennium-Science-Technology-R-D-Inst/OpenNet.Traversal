#pragma once

#include "opennet/config.hpp"

#include <memory>
#include <shared_mutex>
#include <string>

namespace opennet
{
	struct advertised_address_snapshot
	{
		std::string ipv4;
		std::string ipv6;
	};

	class advertised_addresses
	{
	public:
		explicit advertised_addresses(config const& settings);

		advertised_address_snapshot snapshot() const;
		bool refresh();
		bool is_dynamic() const noexcept;

	private:
		advertised_address_snapshot resolve_hostname() const;

		std::string hostname_;
		std::string alternate_ipv4_;
		mutable std::shared_mutex mutex_;
		advertised_address_snapshot current_;
	};

	using advertised_addresses_ptr = std::shared_ptr<advertised_addresses>;
}
