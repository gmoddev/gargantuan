#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace gargantuan {
	class Instance;
}

namespace gargantuan::network {
	enum class ReplicaApplyStatus : std::uint8_t {
		Applied,
		MalformedFrame,
		SchemaMismatch,
		StaleEpoch,
		StaleSequence,
		OutOfOrderSequence,
		SemanticRejection,
	};

	struct ReplicaApplyResult {
		ReplicaApplyStatus Status = ReplicaApplyStatus::SemanticRejection;
		std::size_t AppliedOperations = 0;
		std::string Message;
		[[nodiscard]] bool Succeeded() const {
			return Status == ReplicaApplyStatus::Applied;
		}
	};

	struct ReplicaMetrics {
		std::uint64_t FramesApplied = 0;
		std::uint64_t OperationsApplied = 0;
		std::uint64_t RejectedStaleOperations = 0;
		std::uint64_t RejectedInvalidReferences = 0;
	};

	class ReplicaApplier {
	  public:
		[[nodiscard]] ReplicaApplyResult ApplyBytes(std::span<const std::byte> Bytes);
		[[nodiscard]] ReplicaApplyResult ApplyFrame(const ReplicationFrame &Frame);
		void Reset();

		[[nodiscard]] std::shared_ptr<Instance> GetReplicaRoot() const {
			return Receiver.Root;
		}
		[[nodiscard]] std::shared_ptr<Instance> Resolve(ObjectId SourceObject) const;
		[[nodiscard]] ReplicationEpoch GetEpoch() const {
			return Epoch;
		}
		[[nodiscard]] ReliableReplicationSequence GetNextSequence() const {
			return NextSequence;
		}
		[[nodiscard]] const ReplicaMetrics &GetMetrics() const {
			return Metrics;
		}

	  private:
		Snapshot SemanticState;
		SnapshotLoadResult Receiver;
		ReplicationEpoch Epoch;
		ReliableReplicationSequence NextSequence;
		ReplicaMetrics Metrics;
	};
}
