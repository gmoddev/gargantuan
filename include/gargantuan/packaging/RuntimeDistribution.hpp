#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	class BaseFilesystem;

	enum class RuntimeDistributionTarget : std::uint8_t { OfflinePlayer, DistributedPlayer, DedicatedServer };
	enum class RuntimeDistributionPreset : std::uint8_t { Open, Compiled, Hybrid, Protected };
	enum class RuntimeDistributionRepresentation : std::uint8_t { Source, Compiled, Protected, Excluded };
	enum class RuntimeDistributionDiagnostics : std::uint8_t { Minimal, LineInfo, PrivateSymbols };
	enum class RuntimeDistributionKeyAccess : std::uint8_t { None, LocalProvisioned, HostProvisioned };

	struct RuntimeDistributionError final : std::runtime_error {
		RuntimeDistributionError(std::string_view Code, const char *Message, std::string_view Item = {});
		std::string Code;
		std::string Item;
	};

	struct RuntimeDistributionRequest final {
		RuntimeDistributionTarget Target = RuntimeDistributionTarget::OfflinePlayer;
		RuntimeDistributionPreset Preset = RuntimeDistributionPreset::Open;
		std::optional<RuntimeDistributionDiagnostics> Diagnostics;
		bool RequirePublisher = false;
		std::string SigningKeyId;
		RuntimeDistributionKeyAccess KeyAccess = RuntimeDistributionKeyAccess::None;
		std::string ContentKeyId;
		std::string PolicyTarget;
	};

	struct RuntimeDistributionScript final {
		std::string SourceId;
		std::vector<std::string> Path;
		std::string Class;
		std::string Source;
		bool Eligible = false;
		RuntimeDistributionRepresentation Representation = RuntimeDistributionRepresentation::Excluded;
		RuntimeDistributionDiagnostics Diagnostics = RuntimeDistributionDiagnostics::Minimal;
		std::string Origin;
	};

	struct RuntimeDistributionPlan final {
		RuntimeDistributionRequest Request;
		std::vector<RuntimeDistributionScript> Scripts;
		std::string PolicySha256;
		std::string ClientCodeIdentity;
		std::string SessionIdentity;
		bool SpatialContent{};
		std::string WorkspaceTerrainManifest;
		int WorkspaceTerrainSpacingShift{};
	};

	[[nodiscard]] RuntimeDistributionRequest ParseRuntimeDistributionRequest(std::string_view Json);
	void ResolveRuntimeDistributionPolicy(BaseFilesystem &Filesystem, RuntimeDistributionPlan &Plan);
	[[nodiscard]] bool IsRuntimeDistributionIdentifier(std::string_view Value);
	[[nodiscard]] std::string_view GetRuntimeDistributionName(RuntimeDistributionTarget Value);
	[[nodiscard]] std::string_view GetRuntimeDistributionName(RuntimeDistributionRepresentation Value);
	[[nodiscard]] std::string_view GetRuntimeDistributionName(RuntimeDistributionDiagnostics Value);
	[[nodiscard]] std::string_view GetRuntimeDistributionName(RuntimeDistributionKeyAccess Value);
	[[nodiscard]] std::string GetRuntimeDistributionGuiDigest(
		std::string_view ClientCodeIdentity, std::string_view SourceId, int SourceVersion);
}
