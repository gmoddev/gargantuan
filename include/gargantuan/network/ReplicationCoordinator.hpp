#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace gargantuan {
	class Instance;
}

namespace gargantuan::network {
	struct ReplicationMetrics {
		std::uint64_t ObjectsPublished = 0;
		std::uint64_t ObjectsUnpublished = 0;
		std::uint64_t ObjectsDestroyed = 0;
		std::uint64_t OperationsGenerated = 0;
		std::uint64_t OperationsCoalesced = 0;
		std::uint64_t BaselineBytes = 0;
		std::uint64_t IncrementalBytes = 0;
		std::uint64_t BaselineObjects = 0;
		std::uint64_t RejectedInvalidReferences = 0;
		std::uint64_t ReplicationBacklog = 0;
	};

	struct ReplicationProduceResult {
		std::optional<ReplicationFrame> Frame;
		std::string Error;
		[[nodiscard]] bool Succeeded() const {
			return Frame.has_value();
		}
	};

	class ReplicationCoordinator {
	  public:
		explicit ReplicationCoordinator(std::shared_ptr<Instance> SourceRoot);

		[[nodiscard]] ReplicationProduceResult AddPeer(ConnectionId Connection, ReplicationEpoch Epoch);
		[[nodiscard]] ReplicationProduceResult
		ProduceIncremental(ConnectionId Connection, std::size_t MaximumJournalRecords = MaximumWireJournalRecords);
		[[nodiscard]] ReplicationProduceResult SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant);
		bool RemovePeer(ConnectionId Connection);
		[[nodiscard]] const ReplicationView *GetView(ConnectionId Connection) const;
		[[nodiscard]] const ReplicationMetrics &GetMetrics() const {
			return Metrics;
		}

	  private:
		struct PeerState {
			ReplicationView View;
			ChangeCursor JournalCursor;
			ReliableReplicationSequence NextSequence{1};
		};
		std::shared_ptr<Instance> SourceRoot;
		std::map<ConnectionId, PeerState> Peers;
		ReplicationMetrics Metrics;
	};
}
