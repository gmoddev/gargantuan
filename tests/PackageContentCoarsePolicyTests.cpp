#include "gargantuan/content/ContentAvailability.hpp"

#include <stdexcept>
#include <iostream>
#include <vector>

using namespace gargantuan;

namespace {
	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void DenseCellPages() {
		PackageContentManifest Manifest;
		for (std::size_t Index = 0; Index < 9; ++Index) {
			PackageContentEntry Entry;
			Entry.Key = "region/" + std::to_string(Index);
			Entry.CoarseBounds = PackageCoarseBounds{{0, 0, 0}, {0, 0, 0}};
			Manifest.Entries.push_back(std::move(Entry));
		}
		PackageContentCoarseIndex Index(Manifest, 1.0f);
		const PackageCoarseBounds Bounds{{0, 0, 0}, {0, 0, 0}};
		const PackageCoarseQueryLimits Limits{1, 2, 2};
		std::optional<PackageCoarseQueryCursor> Cursor;
		std::vector<std::size_t> All;
		for (std::size_t PageNumber = 0; PageNumber < 9; ++PageNumber) {
			auto Page = Index.QueryPage("default", Bounds, Limits, Cursor);
			Check(Page.CellsVisited <= 1 && Page.MembershipVisits <= 2 && Page.Entries.size() <= 2,
				"coarse page exceeded an independent limit");
			All.insert(All.end(), Page.Entries.begin(), Page.Entries.end());
			if (Page.Status == PackageCoarseQueryStatus::Complete) {
				Check(!Page.NextCursor && All.size() == 9, "coarse query completed without every dense entry");
				for (std::size_t Entry = 0; Entry < All.size(); ++Entry)
					Check(All[Entry] == Entry, "coarse query changed deterministic membership order");
				return;
			}
			Check(Page.Status == PackageCoarseQueryStatus::More && Page.NextCursor,
				"coarse query lost its continuation");
			Cursor = std::move(Page.NextCursor);
		}
		throw std::runtime_error("dense coarse query failed to terminate");
	}

	void CursorAndLimits() {
		PackageContentManifest Manifest;
		PackageContentEntry Entry;
		Entry.Key = "region/0";
		Entry.CoarseBounds = PackageCoarseBounds{{0, 0, 0}, {2, 0, 0}};
		Manifest.Entries.push_back(std::move(Entry));
		PackageContentCoarseIndex Index(Manifest, 1.0f);
		PackageContentCoarseIndex Replacement(Manifest, 1.0f);
		const PackageCoarseBounds Bounds{{0, 0, 0}, {2, 0, 0}};
		const PackageCoarseQueryLimits Limits{1, 1, 1};
		auto First = Index.QueryPage("default", Bounds, Limits);
		Check(First.Status == PackageCoarseQueryStatus::More && First.NextCursor &&
			First.CellsVisited == 1 && First.MembershipVisits == 1,
			"coarse query did not stop at the first cell budget");
		Check(Replacement.QueryPage("default", Bounds, Limits, First.NextCursor).Status ==
			PackageCoarseQueryStatus::InvalidCursor, "replacement index accepted a stale cursor");
		Check(Index.QueryPage("other", Bounds, Limits, First.NextCursor).Status ==
			PackageCoarseQueryStatus::InvalidCursor, "different package space accepted a cursor");
		Check(Index.QueryPage("default", {{0, 0, 0}, {1, 0, 0}}, Limits, First.NextCursor).Status ==
			PackageCoarseQueryStatus::InvalidCursor, "different bounds accepted a cursor");
		auto Second = Index.QueryPage("default", Bounds, Limits, First.NextCursor);
		Check(Second.Status == PackageCoarseQueryStatus::More && Second.Entries == std::vector<std::size_t>{0},
			"coarse page did not expose deterministic repeated multi-cell membership");
		Check(Index.QueryPage("default", {{0, 0, 0}, {4'096, 0, 0}}, Limits).Status ==
			PackageCoarseQueryStatus::QueryCellLimit, "oversized volume silently returned an empty candidate set");
		Check(Index.QueryPage("default", Bounds, {0, 1, 1}).Status == PackageCoarseQueryStatus::InvalidInput,
			"zero cell budget was accepted");
		Check(Index.Query("default", Bounds, 1) == std::vector<std::size_t>{0},
			"legacy coarse query changed");
	}

	void AggregateMembershipBound() {
		PackageContentManifest Manifest;
		for (std::size_t Unit = 0; Unit < MaximumPackageCoarseIndexMemberships / 4096 + 1; ++Unit) {
			PackageContentEntry Entry;
			Entry.Key = "bounded/" + std::to_string(Unit);
			Entry.CoarseBounds = PackageCoarseBounds{{0, 0, 0}, {4095, 0, 0}};
			Manifest.Entries.push_back(std::move(Entry));
		}
		bool Rejected = false;
		try { PackageContentCoarseIndex Index(Manifest, 1.0f); }
		catch (const std::length_error &) { Rejected = true; }
		Check(Rejected, "legal per-unit overlap bypassed the aggregate index memory ceiling");
	}
}

int main() {
	try {
		DenseCellPages();
		CursorAndLimits();
		AggregateMembershipBound();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:CoarseTests] " << Error.what() << '\n';
		return 1;
	}
}
