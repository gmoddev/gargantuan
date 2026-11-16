// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/generated/AttachmentMesh.hpp"

#include <string>

namespace gargantuan {
	class AttachmentMesh final : public Attachment {
		I_AttachmentMesh;

	  private:
		std::string Mesh;
		std::string Material;
		glm::vec3 Size{1.0f};
		Color3 Color{1.0f, 1.0f, 1.0f};
		float Transparency = 0.0f;
	};
}
