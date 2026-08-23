#include "opennet/advertised_addresses.hpp"
#include "opennet/config.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	opennet::config parse(std::initializer_list<char const*> arguments)
	{
		std::vector<std::string> storage;
		storage.emplace_back("OpenNet.Traversal");
		for (char const* argument : arguments)
			storage.emplace_back(argument);

		std::vector<char*> argv;
		argv.reserve(storage.size());
		for (std::string& argument : storage)
			argv.push_back(argument.data());
		return opennet::parse_config(static_cast<int>(argv.size()), argv.data());
	}
}

int main()
{
	opennet::config const dynamic = parse({
		"--bind", "0.0.0.0",
		"--advertise-hostname", "localhost" });
	require(dynamic.advertised_hostname == "localhost", "hostname was not parsed");
	require(dynamic.advertised_address.empty(), "dynamic mode retained a static IPv4");

	opennet::advertised_addresses dynamic_addresses{ dynamic };
	require(dynamic_addresses.is_dynamic(), "dynamic mode was not enabled");
	require(!dynamic_addresses.snapshot().ipv4.empty(), "localhost has no IPv4 result");

	opennet::config const fixed = parse({ "--advertise", "192.0.2.10" });
	opennet::advertised_addresses fixed_addresses{ fixed };
	require(!fixed_addresses.is_dynamic(), "static mode was marked dynamic");
	require(
		fixed_addresses.snapshot().ipv4 == "192.0.2.10",
		"static IPv4 was not preserved");

	bool conflict_rejected = false;
	try
	{
		static_cast<void>(parse({
			"--advertise", "192.0.2.10",
			"--advertise-hostname", "localhost" }));
	}
	catch (std::invalid_argument const&)
	{
		conflict_rejected = true;
	}
	require(conflict_rejected, "conflicting advertise options were accepted");
}
