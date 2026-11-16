// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/classes/AttachmentMesh.hpp"

#include "gargantuan/assets/AssetTypes.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	std::string AttachmentMesh::GetMesh() const { return Mesh; }

	void AttachmentMesh::SetMesh(std::string Value) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Creator:AttachmentMesh] Mesh requires a strict asset reference");
		ValidatePropertyMutation("Mesh", Value);
		if (Mesh == Value) return;
		Mesh = std::move(Value);
		NotifyPropertyCommitted("Mesh");
	}

	std::string AttachmentMesh::GetMaterial() const { return Material; }

	void AttachmentMesh::SetMaterial(std::string Value) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Creator:AttachmentMesh] Material requires a strict asset reference");
		ValidatePropertyMutation("Material", Value);
		if (Material == Value) return;
		Material = std::move(Value);
		NotifyPropertyCommitted("Material");
	}

	glm::vec3 AttachmentMesh::GetSize() const { return Size; }

	void AttachmentMesh::SetSize(glm::vec3 Value) {
		AssertCanMutate();
		for (const auto Component : {Value.x, Value.y, Value.z})
			if (!std::isfinite(Component) || Component < 0.01f || Component > 1024.0f)
				throw std::invalid_argument("[Creator:AttachmentMesh] Size must be finite in [0.01,1024]");
		ValidatePropertyMutation("Size", Value);
		if (Size == Value) return;
		Size = Value;
		NotifyPropertyCommitted("Size");
	}

	Color3 AttachmentMesh::GetColor() const { return Color; }

	void AttachmentMesh::SetColor(Color3 Value) {
		AssertCanMutate();
		for (const auto Component : {Value.R, Value.G, Value.B})
			if (!std::isfinite(Component) || Component < 0.0f || Component > 1.0f)
				throw std::invalid_argument("[Creator:AttachmentMesh] Color channels must be finite in [0,1]");
		ValidatePropertyMutation("Color", Value);
		if (static_cast<glm::vec3>(Color) == static_cast<glm::vec3>(Value)) return;
		Color = Value;
		NotifyPropertyCommitted("Color");
	}

	float AttachmentMesh::GetTransparency() const { return Transparency; }

	void AttachmentMesh::SetTransparency(float Value) {
		AssertCanMutate();
		if (!std::isfinite(Value) || Value < 0.0f || Value > 1.0f)
			throw std::invalid_argument("[Creator:AttachmentMesh] Transparency must be finite in [0,1]");
		ValidatePropertyMutation("Transparency", Value);
		if (Transparency == Value) return;
		Transparency = Value;
		NotifyPropertyCommitted("Transparency");
	}
}
