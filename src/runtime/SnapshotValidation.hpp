#pragma once

#include "gargantuan/runtime/Snapshot.hpp"

namespace gargantuan {
	// Private semantic validation shared by codec experiments. This is not a
	// serializer contract and must not be exposed through public headers.
	void ValidateSnapshotSemantic(const Snapshot &SnapshotValue);
}
