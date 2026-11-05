#pragma once

#include "gargantuan/content/ResidencyDemandPolicy.hpp"

namespace gargantuan {
	enum class StreamingDiagnosticStatus : std::uint8_t {
		Disabled, NotConfigured, AwaitingManifest, Preparing, Active, PreparationFailed, Stopped,
	};

	// Value projection only. Contains no provider, filesystem, network endpoint,
	// Instance, callback or content-demand authority. All reads run on Main.
	struct StreamingDiagnostics final {
		std::uint32_t Version = 1;
		StreamingDiagnosticStatus Status = StreamingDiagnosticStatus::Disabled;
		bool Enabled = false;
		bool ContentConfigured = false;
		bool ManifestAvailable = false;
		std::uint64_t SimulationTick = 0;
		std::uint64_t LastContentStepNanoseconds = 0;
		ResidencyPolicyDiagnostics Policy;
		ContentAvailabilityDiagnosticCounters Content;
	};
	[[nodiscard]] const char *GetStreamingDiagnosticStatusName(StreamingDiagnosticStatus Status);
}
