// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <functional>

namespace gargantuan::detail {
	// Native regression seam only. The ordinary backend factory installs no hooks.
	// Both callbacks run on the Step caller, outside the backend mutex. BeforeMerge
	// runs after real solver jobs have joined and before generation/revision checks.
	struct SoftBodyStepTestHooks {
		std::function<void()> StepAdmitted;
		std::function<void(const SoftBodyStepProfile &)> BeforeMerge;
	};

	[[nodiscard]] std::unique_ptr<ISoftBodyBackend> CreateSoftBodyBackendForTesting(
		SoftBodyStepTestHooks Hooks,
		SoftBodyWorldLimits Limits = {}
	);
}
