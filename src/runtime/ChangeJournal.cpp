#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

namespace gargantuan {
	namespace {
		thread_local std::size_t SuppressionDepth = 0;
	}

	ChangeJournal &ChangeJournal::Get() {
		static ChangeJournal Journal;
		return Journal;
	}

	std::uint64_t ChangeJournal::Commit(ObjectId object, ChangePayload payload) {
		AssertAuthoritativeMutation("ChangeJournal::Commit");
		if (SuppressionDepth != 0) return 0;
		std::scoped_lock lock(Mutex);
		const auto sequence = NextSequence++;
		Records.push_back({sequence, object, std::move(payload)});
		while (Records.size() > Capacity) Records.pop_front();
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

	ChangeCursor ChangeJournal::CreateCursor() const {
		std::scoped_lock lock(Mutex);
		return {NextSequence};
	}

	ChangeReadResult ChangeJournal::Read(ChangeCursor cursor, std::size_t maximumRecords) const {
		std::scoped_lock lock(Mutex);
		ChangeReadResult result{.Cursor = cursor};
		const auto oldest = Records.empty() ? NextSequence : Records.front().Sequence;
		if (cursor.NextSequence < oldest) {
			result.Status = ChangeReadStatus::ResnapshotRequired;
			result.Cursor.NextSequence = oldest;
			return result;
		}

		for (const auto &record : Records) {
			if (record.Sequence < cursor.NextSequence) continue;
			if (result.Records.size() == maximumRecords) break;
			result.Records.push_back(record);
			result.Cursor.NextSequence = record.Sequence + 1;
		}
		return result;
	}

	void ChangeJournal::SetCapacity(std::size_t capacity) {
		AssertAuthoritativeMutation("ChangeJournal::SetCapacity");
		std::scoped_lock lock(Mutex);
		Capacity = capacity;
		while (Records.size() > Capacity) Records.pop_front();
	}

	std::size_t ChangeJournal::GetCapacity() const {
		std::scoped_lock lock(Mutex);
		return Capacity;
	}

	void ChangeJournal::Clear() {
		std::scoped_lock lock(Mutex);
		Records.clear();
	}

	ScopedChangeJournalSuppression::ScopedChangeJournalSuppression() {
		++SuppressionDepth;
	}

	ScopedChangeJournalSuppression::~ScopedChangeJournalSuppression() {
		--SuppressionDepth;
	}
}
