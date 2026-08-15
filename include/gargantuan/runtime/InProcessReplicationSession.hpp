#pragma once

#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireJournal.hpp"

#include <memory>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace gargantuan {
	class InProcessReplicationSession;

	enum class ReplicationApplyStatus {
		Success,
		NoChanges,
		ResnapshotRequired,
		DuplicateRecord,
		OutOfOrderRecord,
		MalformedRecord,
		ApplyRejected,
	};

	struct ReplicationApplyResult {
		ReplicationApplyStatus Status = ReplicationApplyStatus::ApplyRejected;
		std::size_t AppliedRecords = 0;
		std::string Message;
		[[nodiscard]] bool Succeeded() const {
			return Status == ReplicationApplyStatus::Success || Status == ReplicationApplyStatus::NoChanges;
		}
	};

	struct ReplicationSessionStartResult {
		std::shared_ptr<InProcessReplicationSession> Session;
		std::vector<std::string> Errors;
		[[nodiscard]] bool Succeeded() const { return Session != nullptr && Errors.empty(); }
	};

	class InProcessReplicationSession {
	  public:
		static ReplicationSessionStartResult Start(const std::shared_ptr<Instance> &sourceRoot);

		ReplicationApplyResult ApplyAvailable(
			std::size_t maximumRecords = std::numeric_limits<std::size_t>::max()
		);
		ReplicationApplyResult ApplyWireRecords(const std::vector<WireJournalRecord> &records);
		[[nodiscard]] std::shared_ptr<Instance> GetReceiverRoot() const { return Receiver.Root; }
		[[nodiscard]] std::shared_ptr<Instance> ResolveReceiver(WireObjectId id) const;
		[[nodiscard]] ChangeCursor GetCursor() const { return Cursor; }

	  private:
		ReplicationApplyResult ApplyRecord(const WireJournalRecord &record);
		ReplicationApplyResult ApplyProperty(const WireJournalRecord &record, const std::shared_ptr<Instance> &instance);
		ReplicationApplyResult ApplyAttribute(const WireJournalRecord &record, const std::shared_ptr<Instance> &instance);
		ReplicationApplyResult ApplyExtensionProperty(
			const WireJournalRecord &record,
			const std::shared_ptr<Instance> &instance
		);
		ReplicationApplyResult PreflightCustomClassRecords(
			const std::vector<WireJournalRecord> &records
		) const;

		SnapshotLoadResult Receiver;
		ChangeCursor Cursor;
		std::unordered_set<WireObjectId> TrackedObjects;
		std::unordered_set<WireObjectId> CandidateObjects;
	};
}
