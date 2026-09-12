#pragma once

#include "gargantuan/runtime/ObjectId.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

namespace gargantuan::network::detail {

inline constexpr std::size_t MaximumPlanningLookupAdvances = 8;

// Call-local ordered join, not a cache or resumable materialization frontier.
// Requests must be nondecreasing full ObjectIds, and Values must not mutate
// during the pass. Sparse queries jump after a bounded walk instead of scanning
// unrelated catalog/accepted objects. Equality still validates the generation.
template <typename Map>
typename Map::const_iterator FindPlanningObject(
	const Map &Values, typename Map::const_iterator &Cursor, ObjectId Object
) {
	std::size_t Advances = 0;
	while (Cursor != Values.cend() && Cursor->first < Object && Advances < MaximumPlanningLookupAdvances) {
		++Cursor;
		++Advances;
	}
	runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningLookupAdvances, Advances);
	runtime_detail::MaximumWork(runtime_detail::WorkCounter::PlanningLookupMaximumAdvances, Advances);
	if (Cursor != Values.cend() && Cursor->first < Object) {
		Cursor = Values.lower_bound(Object);
		runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningLookupSearches);
	}
	return Cursor != Values.cend() && Cursor->first == Object ? Cursor : Values.cend();
}

}
