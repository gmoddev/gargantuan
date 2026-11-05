#pragma once

#include "gargantuan/content/StreamingDiagnostics.hpp"
#include "serialization/JsonCodec.hpp"

namespace gargantuan {
	[[nodiscard]] JsonCodec::Json EncodeStreamingDiagnosticsSnapshot(const StreamingDiagnostics &Snapshot);
}
