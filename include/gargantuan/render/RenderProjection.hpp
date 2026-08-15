// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"

#include <cstddef>
#include <unordered_map>

namespace gargantuan {
	struct RenderProjectionChanges {
		std::size_t Created = 0;
		std::size_t Updated = 0;
		std::size_t Removed = 0;
		std::size_t Unchanged = 0;
	};

	class RenderProjection final {
	  public:
		[[nodiscard]] RenderProjectionChanges Apply(const RenderSnapshot &Snapshot);
		void Clear();

		[[nodiscard]] const RenderItem *GetItem(ObjectId Object) const;
		[[nodiscard]] std::size_t GetSize() const { return Entries.size(); }

	  private:
		struct Entry {
			RenderItem Item;
		};

		std::unordered_map<ObjectId, Entry> Entries;
	};
} // namespace gargantuan
