// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/classes/generated/FileLink.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"

#include <vector>

namespace gargantuan {
	class FileLink : public Instance {
		I_FileLink;

		std::vector<std::shared_ptr<Instance>> OwnedSiblings;
		bool Synchronizing = false;

		[[nodiscard]] SourceMountResult<std::size_t> Synchronize(SourceMount &Mount);

		FileLink();
	};
}
