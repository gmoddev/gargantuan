// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/datatypes/CFrame.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace gargantuan::blockgrid {
	struct AssemblyProjection {
		static constexpr std::uint8_t CurrentVersion=1;
		std::uint8_t Version{CurrentVersion};
		std::uint64_t Revision{1};
		bool Dynamic{};
		CFrame Pose;
		glm::vec3 LinearVelocity{},AngularVelocity{};
		float Mass{};
		glm::vec3 LocalCenterOfMass{};
		glm::mat3 LocalInertia{0.f};
	};
	[[nodiscard]] bool ValidAssemblyProjection(const AssemblyProjection& Value) noexcept;
}
namespace gargantuan {
	class Instance;
	class WorldRoot;
	void ValidateBlockGridDomainChange(const Instance& Moving,const WorldRoot* Destination);
	// Main-only optional preflight for a live simulation body whose effective world would change.
	void ValidateBlockGridReparent(const Instance& Moving,const std::shared_ptr<Instance>& NewParent,
		std::span<const std::shared_ptr<Instance>> Subtree);
}
