#pragma once

#include "gargantuan/reflection/RuntimeSchema.hpp"

namespace gargantuan {
	class Instance;
	// A projection of frozen class policy and current ancestry, never a name,
	// script domain grant, second service registry or peer materialization view.
	[[nodiscard]] SchemaServicePolicy GetInstanceServicePolicy(const Instance &Object);
}
