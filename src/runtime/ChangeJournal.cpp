#include "gargantuan/runtime/ChangeJournal.hpp"

namespace gargantuan {
	ChangeJournal &ChangeJournal::Get() {
		static ChangeJournal Journal;
		return Journal;
	}

	std::uint64_t ChangeJournal::Commit(ObjectId object, ChangePayload payload) {
		std::scoped_lock lock(Mutex);
		const auto sequence = NextSequence++;
		Records.push_back({sequence, object, std::move(payload)});
		return sequence;
	}

	std::vector<ChangeRecord> ChangeJournal::ReadSince(std::uint64_t sequence) const {
		std::scoped_lock lock(Mutex);
		std::vector<ChangeRecord> result;
		for (const auto &record : Records) {
			if (record.Sequence > sequence) result.push_back(record);
		}
		return result;
	}

	void ChangeJournal::Clear() {
		std::scoped_lock lock(Mutex);
		Records.clear();
	}
}
