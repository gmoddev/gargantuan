#pragma once

#include "gargantuan/runtime/Snapshot.hpp"

namespace gargantuan {
	// Private semantic validation shared by codec experiments. This is not a
	// serializer contract and must not be exposed through public headers.
	void ValidateSnapshotSemantic(const Snapshot &SnapshotValue);

	namespace runtime_detail {
		class ReplicaCharacterProjection {
		  public:
			static bool TryWrite(Instance &Target, std::string_view Property, const std::shared_ptr<Instance> &Value);
		};

		SnapshotLoadResult LoadReplicaSnapshot(const Snapshot &Value, SnapshotLoadProfile *Profile = nullptr);
	}
}
