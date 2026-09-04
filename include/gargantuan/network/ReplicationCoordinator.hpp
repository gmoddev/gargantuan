#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace gargantuan {
	class Instance;
}

namespace gargantuan::network {
	inline constexpr std::size_t MaximumRelevanceTransitionsPerFrame = 4'096;

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
		std::uint64_t SnapshotCaptureCpuNanoseconds = 0;
		std::uint64_t BaselineDiscoveryCpuNanoseconds = 0;
		std::uint64_t BaselineEncodeCpuNanoseconds = 0;
		std::uint64_t CatalogObjects = 0;
		std::uint64_t CatalogRefreshes = 0;
		std::uint64_t DependencyObjects = 0;
		std::uint64_t DependencyLimitFailures = 0;
		std::uint64_t SoftReferenceFixups = 0;
		std::uint64_t RelevanceTransitions = 0;
		std::uint64_t RelevanceTransitionCpuNanoseconds = 0;
		std::uint64_t MaterializationBacklog = 0;
		std::uint64_t StructuralTemplateBuilds = 0;
		std::uint64_t StructuralTemplateHits = 0;
		std::uint64_t StructuralTemplateMisses = 0;
		std::uint64_t StructuralTemplateInvalidations = 0;
		std::uint64_t StructuralTemplateBytes = 0;
		std::uint64_t PeerMaterializationPlans = 0;
		std::uint64_t PeerPatchOperations = 0;
		std::uint64_t ReferencePatchOperations = 0;
		std::uint64_t StructuralBytesEncoded = 0;
		std::uint64_t StructuralBytesReused = 0;
		std::uint64_t ScratchHighWaterBytes = 0;
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
		using InitialRelevancePolicy = std::function<bool(ObjectId)>;
		explicit ReplicationCoordinator(
			std::shared_ptr<Instance> SourceRoot,
			InitialRelevancePolicy IsInitiallyRelevant = {},
			bool StructuralTemplateReuseEnabled = true
		);

		[[nodiscard]] ReplicationProduceResult AddPeer(ConnectionId Connection, ReplicationEpoch Epoch);
		[[nodiscard]] ReplicationProduceResult
		AddPeer(ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection);
		[[nodiscard]] ReplicationProduceResult
		UpdateRelevance(ConnectionId Connection, const PeerRelevanceSelection &Selection);
		[[nodiscard]] ReplicationProduceResult
		ProduceIncremental(ConnectionId Connection, std::size_t MaximumJournalRecords = MaximumWireJournalRecords);
		[[nodiscard]] ReplicationProduceResult SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant);
		bool RemovePeer(ConnectionId Connection);
		[[nodiscard]] const ReplicationView *GetView(ConnectionId Connection) const;
		[[nodiscard]] bool HasPendingRelevance(ConnectionId Connection) const;
		[[nodiscard]] const ReplicationMetrics &GetMetrics() const {
			return Metrics;
		}

	  private:
		struct PeerState {
			ReplicationView View;
			ChangeCursor JournalCursor;
			ReliableReplicationSequence NextSequence{1};
			std::set<ObjectId> DesiredObjects;
			std::size_t PendingRelevanceTransitions = 0;
			bool PolicyManaged = false;
		};
		std::shared_ptr<Instance> SourceRoot;
		InitialRelevancePolicy IsInitiallyRelevant;
		bool StructuralTemplateReuseEnabled = true;
		ObjectId WorldGeneration;
		std::map<ObjectId, std::shared_ptr<const StructuralMaterializationTemplate>> Catalog;
		std::set<ObjectId> RequestedTemplates;
		std::set<ObjectId> RetiredObjects;
		ChangeCursor CatalogCursor;
		std::map<ConnectionId, PeerState> Peers;
		ReplicationMetrics Metrics;

		bool RefreshCatalog(std::string &Error);
		bool BuildDependencyClosure(
			const PeerRelevanceSelection &Selection, std::set<ObjectId> &Closure, std::string &Error
		);
		PreparedPublishReplication MakePeerPublish(
			ObjectId Object,
			const std::set<ObjectId> &Known,
			ReplicationMetrics &CandidateMetrics,
			std::set<ObjectId> &CandidateRequestedTemplates,
			bool AllowSoftReferencePatches = true
		);
		ReplicationIntent FinalizePeerPublish(PreparedPublishReplication Publish) const;
		ReplicationProduceResult ProduceRelevanceFrame(
			ConnectionId Connection, const PeerRelevanceSelection &Selection, ReplicationMessageKind Kind
		);
	};
}
