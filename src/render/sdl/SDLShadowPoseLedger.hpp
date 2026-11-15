// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <stdexcept>
#include <unordered_map>

namespace gargantuan {
	using SDLShadowPoseRevisions = std::unordered_map<ObjectId, std::uint64_t>;

	inline void MergeSDLShadowPoseRevision(SDLShadowPoseRevisions &Destination,
		ObjectId Object, std::uint64_t Revision) {
		const auto [Entry, Inserted] = Destination.try_emplace(Object, Revision);
		if (!Inserted && Entry->second != Revision)
			throw std::logic_error("[Renderer:Shadow] Depth layers resolved inconsistent animation poses");
	}

	inline void MergeSDLShadowPoseRevisions(SDLShadowPoseRevisions &Destination,
		const SDLShadowPoseRevisions &Source) {
		for (const auto &[Object, Revision] : Source)
			MergeSDLShadowPoseRevision(Destination, Object, Revision);
	}
}
