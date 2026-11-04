#include "gargantuan/runtime/ServicePolicy.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <stdexcept>

namespace gargantuan {
	SchemaServicePolicy GetInstanceServicePolicy(const Instance &Object) {
		const Instance *Current = &Object;
		std::shared_ptr<Instance> ParentOwner;
		SchemaServicePolicy Result = SchemaServicePolicy::None;
		for (std::size_t Depth = 0; Current; ++Depth) {
			if (Depth > MaximumProtocolJsonDepth)
				throw std::length_error("[Storage:Policy] Service ancestry exceeds the existing hierarchy limit");
			const auto *Definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(Current));
			if (!Definition)
				throw std::runtime_error("[Storage:Policy] Service ancestry has no frozen class definition");
			if (Definition->ServicePolicy == SchemaServicePolicy::ServerOnly) return SchemaServicePolicy::ServerOnly;
			if (Definition->ServicePolicy == SchemaServicePolicy::Replicated) Result = SchemaServicePolicy::Replicated;
			auto Parent = Current->GetParent();
			ParentOwner = Parent ? *Parent : nullptr;
			Current = ParentOwner.get();
		}
		return Result;
	}
}
