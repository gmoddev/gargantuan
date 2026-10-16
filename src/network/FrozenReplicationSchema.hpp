#pragma once

#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <stdexcept>

namespace gargantuan::network::detail {

// Only detached quote replay installs this scope. The coordinator owns the
// immutable registry for longer than the scope; no worker reads lifecycle
// publication state. Ordinary replication still resolves the current registry.
inline thread_local const RuntimeSchemaRegistry *FrozenReplicationSchema = nullptr;

class FrozenReplicationSchemaScope {
  public:
	explicit FrozenReplicationSchemaScope(const RuntimeSchemaRegistry &Registry)
		: Previous(FrozenReplicationSchema) {
		if (!Registry.IsFrozen()) throw std::invalid_argument("Frozen replication schema is not immutable");
		FrozenReplicationSchema = &Registry;
	}
	~FrozenReplicationSchemaScope() { FrozenReplicationSchema = Previous; }
	FrozenReplicationSchemaScope(const FrozenReplicationSchemaScope &) = delete;
	FrozenReplicationSchemaScope &operator=(const FrozenReplicationSchemaScope &) = delete;

  private:
	const RuntimeSchemaRegistry *Previous;
};

inline const RuntimeSchemaRegistry &GetReplicationSchemaRegistry() {
	return FrozenReplicationSchema ? *FrozenReplicationSchema : GetActiveRuntimeSchemaRegistry();
}

}
