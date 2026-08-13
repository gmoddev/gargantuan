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
		return Commit({}, object, std::move(payload));
	}

	std::uint64_t ChangeJournal::Commit(ObjectId scope, ObjectId object, ChangePayload payload) {
		AssertAuthoritativeMutation("ChangeJournal::Commit");
		if (SuppressionDepth != 0) return 0;
		std::scoped_lock lock(Mutex);
		auto &stream = Streams[scope];
		const auto sequence = stream.NextSequence++;
		stream.Records.push_back({sequence, scope, object, std::move(payload)});
		while (stream.Records.size() > Capacity) stream.Records.pop_front();
		return sequence;
	}

	std::vector<ChangeRecord> ChangeJournal::ReadSince(std::uint64_t sequence) const {
		std::scoped_lock lock(Mutex);
		std::vector<ChangeRecord> result;
		const auto found = Streams.find({});
		if (found == Streams.end()) return result;
		for (const auto &record : found->second.Records) {
			if (record.Sequence > sequence) result.push_back(record);
		}
		return result;
	}

	ChangeCursor ChangeJournal::CreateCursor(ObjectId scope) const {
		std::scoped_lock lock(Mutex);
		auto found = Streams.find(scope);
		return {scope, found == Streams.end() ? 1 : found->second.NextSequence};
	}

	ChangeReadResult ChangeJournal::Read(ChangeCursor cursor, std::size_t maximumRecords) const {
		std::scoped_lock lock(Mutex);
		ChangeReadResult result{.Cursor = cursor};
		auto found = Streams.find(cursor.Scope);
		if (found == Streams.end()) return result;
		const auto &stream = found->second;
		const auto oldest = stream.Records.empty() ? stream.NextSequence : stream.Records.front().Sequence;
		if (cursor.NextSequence < oldest) {
			result.Status = ChangeReadStatus::ResnapshotRequired;
			result.Cursor.NextSequence = oldest;
			return result;
		}

		for (const auto &record : stream.Records) {
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
		for (auto &[scope, stream] : Streams) {
			(void)scope;
			while (stream.Records.size() > Capacity) stream.Records.pop_front();
		}
	}

	std::size_t ChangeJournal::GetCapacity() const {
		std::scoped_lock lock(Mutex);
		return Capacity;
	}

	void ChangeJournal::Clear() {
		std::scoped_lock lock(Mutex);
		for (auto &[scope, stream] : Streams) {
			(void)scope;
			stream.Records.clear();
		}
	}

	ScopedChangeJournalSuppression::ScopedChangeJournalSuppression() {
		++SuppressionDepth;
	}

	ScopedChangeJournalSuppression::~ScopedChangeJournalSuppression() {
		--SuppressionDepth;
	}
}
