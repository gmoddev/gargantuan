#pragma once

#include "gargantuan/content/StreamingDiagnostics.hpp"
#include "serialization/JsonCodec.hpp"

namespace gargantuan {
	struct PlayStreamingDiagnostics;
	[[nodiscard]] JsonCodec::Json EncodeStreamingDiagnosticsSnapshot(const StreamingDiagnostics &Snapshot);
	[[nodiscard]] JsonCodec::Json EncodeStreamingDiagnosticsSnapshot(const PlayStreamingDiagnostics &Snapshot);
}
