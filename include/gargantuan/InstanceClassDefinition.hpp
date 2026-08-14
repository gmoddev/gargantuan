#pragma once

#include "gargantuan/reflection/RuntimeSchema.hpp"

namespace gargantuan {
	// Compatibility name retained for generated classes and existing native
	// reflection callers. RuntimeSchemaRegistry owns the authoritative object.
	using InstanceClassDefinition = SchemaClassDefinition;
}
