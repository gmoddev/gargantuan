// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/physics/SoftBodyTypes.hpp"

#include <memory>
#include <optional>

namespace gargantuan {
	class ISoftBodyBackend {
	  public:
		virtual ~ISoftBodyBackend() = default;

		[[nodiscard]] virtual bool IsValid() const = 0;
		[[nodiscard]] virtual bool IsBodyValid(SoftBodyId Body) const = 0;
		[[nodiscard]] virtual SoftBodyId CreateBody(const SoftBodyDefinition &Definition) = 0;
		virtual PhysicsOperationResult UpdateBody(SoftBodyId Body, const SoftBodyDefinition &Definition) = 0;
		virtual PhysicsOperationResult DestroyBody(SoftBodyId Body) = 0;
		virtual PhysicsOperationResult ApplyForce(SoftBodyId Body, glm::vec3 Force) = 0;
		virtual PhysicsOperationResult ApplyImpulse(SoftBodyId Body, glm::vec3 Impulse) = 0;
		[[nodiscard]] virtual std::optional<SoftBodyState> GetBodyState(SoftBodyId Body) const = 0;
		[[nodiscard]] virtual SoftBodyStepResult Step(const SoftBodyStepConfig &Config) = 0;
		[[nodiscard]] virtual const SoftBodyWorldLimits &GetLimits() const = 0;
	};

	[[nodiscard]] std::unique_ptr<ISoftBodyBackend> CreateSoftBodyBackend(SoftBodyWorldLimits Limits = {});

	class SoftBodyWorld final {
	  public:
		explicit SoftBodyWorld(SoftBodyWorldLimits Limits = {});
		explicit SoftBodyWorld(std::unique_ptr<ISoftBodyBackend> Backend);
		~SoftBodyWorld();

		SoftBodyWorld(const SoftBodyWorld &) = delete;
		SoftBodyWorld &operator=(const SoftBodyWorld &) = delete;
		SoftBodyWorld(SoftBodyWorld &&) noexcept;
		SoftBodyWorld &operator=(SoftBodyWorld &&) noexcept;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] bool IsBodyValid(SoftBodyId Body) const;
		[[nodiscard]] SoftBodyId CreateBody(const SoftBodyDefinition &Definition);
		PhysicsOperationResult UpdateBody(SoftBodyId Body, const SoftBodyDefinition &Definition);
		PhysicsOperationResult DestroyBody(SoftBodyId Body);
		PhysicsOperationResult ApplyForce(SoftBodyId Body, glm::vec3 Force);
		PhysicsOperationResult ApplyImpulse(SoftBodyId Body, glm::vec3 Impulse);
		[[nodiscard]] std::optional<SoftBodyState> GetBodyState(SoftBodyId Body) const;
		[[nodiscard]] SoftBodyStepResult Step(const SoftBodyStepConfig &Config);
		[[nodiscard]] const SoftBodyWorldLimits &GetLimits() const;

	  private:
		std::unique_ptr<ISoftBodyBackend> Backend;
	};
}
