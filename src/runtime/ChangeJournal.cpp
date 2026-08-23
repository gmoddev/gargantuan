#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <chrono>
#include <stdexcept>

namespace gargantuan {
	namespace {
		thread_local std::size_t SuppressionDepth = 0;
		thread_local std::vector<BufferedChangeRecord> *CapturedRecords = nullptr;
		using ProfileClock = std::chrono::steady_clock;

		std::uint64_t ProfileNanoseconds(ProfileClock::duration Duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count()
			);
		}
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
		if (CapturedRecords) {
			CapturedRecords->push_back({scope, object, std::move(payload)});
			return 0;
		}
		RenderDirtyAccumulator::Get().RecordChange(scope, object, payload);
		const bool Profile = ProfilingEnabled.load(std::memory_order_relaxed);
		const auto ProfileStart = Profile ? ProfileClock::now() : ProfileClock::time_point{};
		std::uint64_t Evicted = 0;
		std::scoped_lock lock(Mutex);
		auto &stream = Streams[scope];
		if (stream.NextSequence == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Change journal sequence is exhausted");
		const auto sequence = stream.NextSequence;
		stream.Records.push_back({sequence, scope, object, std::move(payload)});
		++stream.NextSequence;
		while (stream.Records.size() > Capacity) {
			stream.Records.pop_front();
			++Evicted;
		}
		if (Profile) {
			ProfileCommitCount.fetch_add(1, std::memory_order_relaxed);
			ProfileEvictedRecordCount.fetch_add(Evicted, std::memory_order_relaxed);
			ProfileJournalNanoseconds.fetch_add(ProfileNanoseconds(ProfileClock::now() - ProfileStart), std::memory_order_relaxed);
		}
		return sequence;
	}

	void ChangeJournal::CommitBatch(ObjectId scope, std::vector<std::pair<ObjectId, ChangePayload>> changes) {
		AssertAuthoritativeMutation("ChangeJournal::CommitBatch");
		if (SuppressionDepth != 0 || changes.empty()) return;
		if (CapturedRecords) {
			for (auto &[object, payload] : changes)
				CapturedRecords->push_back({scope, object, std::move(payload)});
			return;
		}
		for (const auto &[Object, Payload] : changes)
			RenderDirtyAccumulator::Get().RecordChange(scope, Object, Payload);
		const bool Profile = ProfilingEnabled.load(std::memory_order_relaxed);
		const auto ProfileStart = Profile ? ProfileClock::now() : ProfileClock::time_point{};
		std::scoped_lock lock(Mutex);
		auto &stream = Streams[scope];
		auto nextSequence = stream.NextSequence;
		auto Replacement = stream.Records;
		for (auto &[object, payload] : changes) {
			if (nextSequence == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("Change journal sequence is exhausted");
			Replacement.push_back({nextSequence, scope, object, std::move(payload)});
			++nextSequence;
		}
		std::uint64_t Evicted = 0;
		while (Replacement.size() > Capacity) {
			Replacement.pop_front();
			++Evicted;
		}
		stream.Records.swap(Replacement);
		stream.NextSequence = nextSequence;
		if (Profile) {
			ProfileCommitCount.fetch_add(changes.size(), std::memory_order_relaxed);
			ProfileEvictedRecordCount.fetch_add(Evicted, std::memory_order_relaxed);
			ProfileJournalNanoseconds.fetch_add(ProfileNanoseconds(ProfileClock::now() - ProfileStart), std::memory_order_relaxed);
		}
	}

	void ChangeJournal::EnsureCanCommit(ObjectId scope, std::size_t count) const {
		AssertAuthoritativeMutation("ChangeJournal::EnsureCanCommit");
		if (SuppressionDepth != 0 || count == 0) return;
		std::scoped_lock lock(Mutex);
		auto Found = Streams.find(scope);
		const auto Next = Found == Streams.end() ? std::uint64_t{1} : Found->second.NextSequence;
		if (count > std::numeric_limits<std::uint64_t>::max() - Next)
			throw std::overflow_error("Change journal sequence is exhausted");
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

	void ChangeJournal::ResetProfile() {
		ProfileCommitCount.store(0, std::memory_order_relaxed);
		ProfileEvictedRecordCount.store(0, std::memory_order_relaxed);
		ProfileJournalNanoseconds.store(0, std::memory_order_relaxed);
	}

	ChangeJournalProfile ChangeJournal::GetProfile() const {
		return {
			ProfileCommitCount.load(std::memory_order_relaxed),
			ProfileEvictedRecordCount.load(std::memory_order_relaxed),
			ProfileJournalNanoseconds.load(std::memory_order_relaxed),
		};
	}

	ScopedChangeJournalSuppression::ScopedChangeJournalSuppression() {
		++SuppressionDepth;
	}

	ScopedChangeJournalSuppression::~ScopedChangeJournalSuppression() {
		--SuppressionDepth;
	}

	ScopedChangeJournalCapture::ScopedChangeJournalCapture() {
		if (CapturedRecords) throw std::logic_error("Change journal capture is already active");
		CapturedRecords = &Records;
	}

	ScopedChangeJournalCapture::~ScopedChangeJournalCapture() {
		if (CapturedRecords == &Records) CapturedRecords = nullptr;
	}

	std::vector<BufferedChangeRecord> ScopedChangeJournalCapture::Take() {
		if (CapturedRecords == &Records) CapturedRecords = nullptr;
		return std::move(Records);
	}
}
