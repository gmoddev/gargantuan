#include "gargantuan/classes/MeshPart.hpp"

#include "gargantuan/assets/AssetTypes.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	std::string MeshPart::GetMesh() const { return Mesh; }

	void MeshPart::SetMesh(std::string Value) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Asset:Reference] MeshPart.Mesh requires a strict asset:// or builtin:// reference");
		ValidatePropertyMutation("Mesh", Value);
		if (Mesh == Value) return;
		Mesh = std::move(Value);
		NotifyPropertyCommitted("Mesh");
	}

	std::string MeshPart::GetMaterial() const { return Material; }

	void MeshPart::SetMaterial(std::string Value) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Asset:Reference] MeshPart.Material requires a strict asset:// or builtin:// reference");
		ValidatePropertyMutation("Material", Value);
		if (Material == Value) return;
		Material = std::move(Value);
		NotifyPropertyCommitted("Material");
	}

	PhysicsShapeDesc MeshPart::GetPhysicsShape() const {
		return {.Kind = PhysicsShapeKind::Box, .Size = GetSize()};
	}
}
