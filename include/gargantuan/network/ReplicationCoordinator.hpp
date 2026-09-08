#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <deque>
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
	inline constexpr std::size_t DefaultStructuralTransitionsPerPeerTick = 512;
	inline constexpr std::size_t DefaultStructuralTransitionsPerTick = 8'192;
	inline constexpr std::size_t DefaultStructuralPeerQuantum = 512;
	inline constexpr std::uint64_t DefaultStructuralTransitionDeadlineTicks = 600;
	inline constexpr std::size_t MaximumStructuralTransitionsPerTick = 65'536;
	inline constexpr std::size_t MaximumStructuralPendingTransitions = 1'048'576;

	struct StructuralReplicationConfiguration {
		std::size_t MaximumTransitionsPerPeerTick = DefaultStructuralTransitionsPerPeerTick;
		std::size_t MaximumTransitionsPerTick = DefaultStructuralTransitionsPerTick;
		std::size_t PeerQuantum = DefaultStructuralPeerQuantum;
		std::size_t MaximumPendingTransitionsPerPeer = MaximumPeerDesiredObjects;
		std::size_t MaximumPendingTransitions = MaximumStructuralPendingTransitions;
		std::uint64_t TransitionDeadlineTicks = DefaultStructuralTransitionDeadlineTicks;

		[[nodiscard]] bool IsValid() const;
	};

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
		std::uint64_t StructuralSchedulingTicks = 0;
		std::uint64_t StructuralTransitionsOffered = 0;
		std::uint64_t StructuralTransitionsSelected = 0;
		std::uint64_t StructuralTransitionsPrepared = 0;
		std::uint64_t StructuralTransitionsEncoded = 0;
		std::uint64_t StructuralTransitionsAccepted = 0;
		std::uint64_t StructuralTransitionsCommitted = 0;
		std::uint64_t StructuralTransitionsDeferredByBudget = 0;
		std::uint64_t StructuralTransitionsCancelled = 0;
		std::uint64_t StructuralTransitionsReplanned = 0;
		std::uint64_t StructuralDeadlineMisses = 0;
		std::uint64_t StructuralPeerFairnessRotations = 0;
		std::uint64_t StructuralGlobalBudgetExhaustions = 0;
		std::uint64_t StructuralDependencyPlanOperations = 0;
		std::uint64_t StructuralBacklogLimitFailures = 0;
		std::uint64_t StructuralJournalLagFailures = 0;
		std::uint64_t StructuralMaximumJournalLagRecords = 0;
		std::uint64_t StructuralPendingEnters = 0;
		std::uint64_t StructuralPendingLeaves = 0;
		std::uint64_t StructuralPendingCritical = 0;
		std::uint64_t StructuralActivePeers = 0;
		std::uint64_t StructuralOldestPendingAgeTicks = 0;
		std::uint64_t StructuralCriticalOldestAgeTicks = 0;
		std::uint64_t StructuralSelectionCpuNanoseconds = 0;
	};

	struct ReplicationProduceResult {
		std::optional<ReplicationFrame> Frame;
		std::string Error;
		std::size_t SelectedTransitions = 0;
		[[nodiscard]] bool Succeeded() const {
			return Frame.has_value();
		}
	};

	struct ReplicationScheduleResult {
		std::string Error;
		[[nodiscard]] bool Succeeded() const {
			return Error.empty();
		}
	};

	class ReplicationCoordinator {
	  public:
		using InitialRelevancePolicy = std::function<bool(ObjectId)>;
		explicit ReplicationCoordinator(
			std::shared_ptr<Instance> SourceRoot,
			InitialRelevancePolicy IsInitiallyRelevant = {},
			bool StructuralTemplateReuseEnabled = true,
			StructuralReplicationConfiguration Configuration = {}
		);

		[[nodiscard]] ReplicationProduceResult AddPeer(ConnectionId Connection, ReplicationEpoch Epoch);
		[[nodiscard]] ReplicationProduceResult
		AddPeer(ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection);
		[[nodiscard]] ReplicationProduceResult
		AddPeerBounded(ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection);
		[[nodiscard]] ReplicationScheduleResult
		RegisterPeerBounded(ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection);
		[[nodiscard]] ReplicationProduceResult
		UpdateRelevance(ConnectionId Connection, const PeerRelevanceSelection &Selection);
		[[nodiscard]] ReplicationScheduleResult RecordDesiredState(
			ConnectionId Connection, const PeerRelevanceSelection &Selection, std::uint64_t SimulationTick
		);
		[[nodiscard]] ReplicationProduceResult
		ProducePendingRelevance(ConnectionId Connection, std::size_t MaximumTransitions, std::uint64_t SimulationTick,
			std::size_t MaximumFrameBytes = MaximumReplicationFrameBytes);
		[[nodiscard]] ReplicationProduceResult
		ProducePendingBaseline(ConnectionId Connection, std::size_t MaximumTransitions, std::uint64_t SimulationTick,
			std::size_t MaximumFrameBytes = MaximumReplicationFrameBytes);
		[[nodiscard]] ReplicationProduceResult
		ProduceIncremental(ConnectionId Connection, std::size_t MaximumTransitions = MaximumWireJournalRecords,
			std::size_t MaximumFrameBytes = MaximumReplicationFrameBytes);
		[[nodiscard]] ReplicationProduceResult SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant);
		bool RemovePeer(ConnectionId Connection);
		[[nodiscard]] const ReplicationView *GetView(ConnectionId Connection) const;
		[[nodiscard]] bool HasPendingRelevance(ConnectionId Connection) const;
		[[nodiscard]] std::size_t GetPendingCriticalTransitionCount(ConnectionId Connection) const;
		[[nodiscard]] bool HasPendingStructuralWork() const;
		[[nodiscard]] ReplicationScheduleResult
		CommitSchedulerAcceptance(ConnectionId Connection, ReliableReplicationSequence Sequence);
		void RecordPeerFairnessRotation();
		void RecordGlobalBudgetExhaustion();
		[[nodiscard]] const ReplicationMetrics &GetCumulativeMetrics() const {
			return Metrics;
		}
		[[nodiscard]] ReplicationMetrics GetMetrics() const;

	  private:
		enum class PendingTransitionKind : std::uint8_t { Enter, Leave };
		struct PendingTransition {
			PendingTransitionKind Kind = PendingTransitionKind::Enter;
			std::uint64_t PendingSinceTick = 0;
			std::uint64_t Token = 0;
			bool Critical = false;
		};
		struct PendingQueueEntry {
			ObjectId Object;
			std::uint64_t Token = 0;
		};
		struct PreparedStructuralCommit {
			ReliableReplicationSequence Sequence;
			ReliableReplicationSequence NextSequence;
			std::optional<ChangeCursor> JournalCursor;
			std::vector<ObjectId> Entering;
			std::vector<ObjectId> Leaving;
			std::uint64_t PublishedObjects = 0;
			std::uint64_t UnpublishedObjects = 0;
			std::uint64_t DestroyedObjects = 0;
			std::uint64_t DeadlineMisses = 0;
			std::size_t TransitionCount = 0;
		};

		struct PeerState {
			ReplicationView View;
			ChangeCursor JournalCursor;
			ReliableReplicationSequence NextSequence{1};
			std::set<ObjectId> DesiredObjects;
			std::set<ObjectId> RequiredObjects;
			PeerRelevanceSelection LastSelection;
			ChangeCursor DesiredCatalogCursor;
			std::map<ObjectId, PendingTransition> PendingTransitions;
			std::deque<PendingQueueEntry> CriticalQueue;
			std::deque<PendingQueueEntry> OrdinaryQueue;
			std::map<ObjectId, std::vector<ObjectId>> LeavingDependents;
			std::optional<PreparedStructuralCommit> PreparedCommit;
			std::uint64_t NextPendingToken = 1;
			bool ExplicitSchedulerCommit = false;
			bool PolicyManaged = false;
		};
		std::shared_ptr<Instance> SourceRoot;
		InitialRelevancePolicy IsInitiallyRelevant;
		bool StructuralTemplateReuseEnabled = true;
		StructuralReplicationConfiguration Configuration;
		std::uint64_t StandaloneSchedulingTick = 0;
		std::uint64_t LatestSchedulingTick = 0;
		ObjectId WorldGeneration;
		std::map<ObjectId, std::shared_ptr<const StructuralMaterializationTemplate>> Catalog;
		std::set<ObjectId> RequestedTemplates;
		std::set<ObjectId> RetiredObjects;
		ChangeCursor CatalogCursor;
		std::map<ConnectionId, PeerState> Peers;
		std::size_t PendingTransitionCount = 0;
		ReplicationMetrics Metrics;

		bool RefreshCatalog(std::string &Error);
		bool BuildDependencyClosure(
			const PeerRelevanceSelection &Selection, std::set<ObjectId> &Closure, std::string &Error
		);
		bool RecordDesiredState(
			PeerState &Peer, const PeerRelevanceSelection &Selection, std::uint64_t SimulationTick, std::string &Error
		);
		void RefreshPendingMetrics(ReplicationMetrics &Snapshot) const;
		void CompactPendingQueues(PeerState &Peer);
		void ApplyPreparedCommit(PeerState &Peer, PreparedStructuralCommit Commit);
		PreparedPublishReplication MakePeerPublish(
			ObjectId Object,
			const std::set<ObjectId> &Known,
			ReplicationMetrics &CandidateMetrics,
			std::set<ObjectId> &CandidateRequestedTemplates,
			bool AllowSoftReferencePatches = true
		);
		PreparedPublishReplication MakePeerPublish(
			ObjectId Object,
			const ReplicationView &View,
			const std::set<ObjectId> &Entering,
			const std::set<ObjectId> &Leaving,
			ReplicationMetrics &CandidateMetrics,
			std::set<ObjectId> &CandidateRequestedTemplates
		);
		ReplicationIntent FinalizePeerPublish(PreparedPublishReplication Publish) const;
		ReplicationProduceResult ProduceRelevanceFrame(
			ConnectionId Connection,
			ReplicationMessageKind Kind,
			std::size_t MaximumTransitions,
			std::uint64_t SimulationTick,
			bool CriticalOnly = false,
			std::size_t MaximumFrameBytes = MaximumReplicationFrameBytes
		);
		ReplicationProduceResult AddPeer(
			ConnectionId Connection,
			ReplicationEpoch Epoch,
			const PeerRelevanceSelection &Selection,
			bool BoundOrdinaryTransitions
		);
		ReplicationScheduleResult RegisterPeer(
			ConnectionId Connection,
			ReplicationEpoch Epoch,
			const PeerRelevanceSelection &Selection,
			bool ExplicitSchedulerCommit
		);
	};
}
