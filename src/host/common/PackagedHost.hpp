#pragma once

#include "gargantuan/network/Transport.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan::host {
	[[nodiscard]] std::optional<network::TransportEndpoint> ParseEndpoint(std::string_view Value);
	[[nodiscard]] std::optional<RuntimePackagePayload> BootstrapPackagedRuntime(
		const std::filesystem::path &PackageRoot,
		const std::filesystem::path &RuntimeRoot,
		std::string_view HostName,
		int &ExitCode
	);
}
