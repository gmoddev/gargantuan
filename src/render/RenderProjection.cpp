// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderProjection.hpp"

#include <stdexcept>
#include <unordered_set>

namespace gargantuan {
	namespace {
		bool ItemsEqual(const RenderItem &Left, const RenderItem &Right) {
			return Left.Object == Right.Object && Left.Geometry == Right.Geometry &&
				Left.ModelMatrix == Right.ModelMatrix && Left.InverseModelMatrix == Right.InverseModelMatrix &&
				Left.Color == Right.Color && Left.CastShadow == Right.CastShadow;
		}
	}

	RenderProjectionChanges RenderProjection::Apply(const RenderSnapshot &Snapshot) {
		if (Snapshot.Id == InvalidRenderSnapshotId)
			throw std::invalid_argument("Render projection requires a valid RenderSnapshot identity");

		std::unordered_set<ObjectId> Seen;
		Seen.reserve(Snapshot.Items.size());
		for (const auto &Item : Snapshot.Items) {
			if (!Item.Object.IsValid())
				throw std::invalid_argument("Render projection requires valid ObjectId values");
			if (!Seen.insert(Item.Object).second)
				throw std::invalid_argument("Render projection rejects duplicate ObjectId values");
		}

		RenderProjectionChanges Changes;
		Entries.reserve(Snapshot.Items.size());
		for (const auto &Item : Snapshot.Items) {
			auto Existing = Entries.find(Item.Object);
			if (Existing == Entries.end()) {
				Entries.emplace(Item.Object, Entry{Item});
				++Changes.Created;
				continue;
			}

			if (ItemsEqual(Existing->second.Item, Item)) {
				++Changes.Unchanged;
			} else {
				Existing->second.Item = Item;
				++Changes.Updated;
			}
		}

		for (auto Existing = Entries.begin(); Existing != Entries.end();) {
			if (Seen.contains(Existing->first)) {
				++Existing;
				continue;
			}
			Existing = Entries.erase(Existing);
			++Changes.Removed;
		}
		return Changes;
	}

	void RenderProjection::Clear() { Entries.clear(); }

	const RenderItem *RenderProjection::GetItem(ObjectId Object) const {
		const auto Existing = Entries.find(Object);
		return Existing == Entries.end() ? nullptr : &Existing->second.Item;
	}
}
