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
	class InstanceProperty;
}

namespace gargantuan::network {
	inline constexpr std::size_t MaximumRelevanceTransitionsPerFrame = 4'096;
	inline constexpr std::size_t DefaultStructuralTransitionsPerPeerTick = 512;
	inline constexpr std::size_t DefaultStructuralTransitionsPerTick = 8'192;
	inline constexpr std::size_t DefaultStructuralPeerQuantum = 512;
	inline constexpr std::uint64_t DefaultStructuralTransitionDeadlineTicks = 600;
	inline constexpr std::size_t MaximumStructuralTransitionsPerTick = 65'536;
	inline constexpr std::size_t MaximumStructuralPendingTransitions = 1'048'576;
	inline constexpr std::size_t MaximumStructuralJournalRecordsPerCall = 2 * MaximumWireJournalRecords;
	inline constexpr std::size_t MaximumStructuralJournalRecordsPerTick = 65'536;
	inline constexpr std::size_t MaximumCatalogRetirementExaminationsPerTick = 4'096;
	inline constexpr std::size_t MaximumRetiredCatalogObjects = MaximumStructuralPendingTransitions;
	inline constexpr std::size_t MaximumPlanningWorkPerTick = 262'144;
	inline constexpr std::size_t MaximumPlanningRecordsPerPeer = 262'144;
	inline constexpr std::size_t MaximumPlanningRecords = 8'388'608;

	struct StructuralReplicationConfiguration {
		std::size_t MaximumTransitionsPerPeerTick = DefaultStructuralTransitionsPerPeerTick;
		std::size_t MaximumTransitionsPerTick = DefaultStructuralTransitionsPerTick;
		std::size_t PeerQuantum = DefaultStructuralPeerQuantum;
		std::size_t MaximumPendingTransitionsPerPeer = MaximumPeerDesiredObjects;
		std::size_t MaximumPendingTransitions = MaximumStructuralPendingTransitions;
		std::uint64_t TransitionDeadlineTicks = DefaultStructuralTransitionDeadlineTicks;
		std::size_t MaximumJournalRecordsPerPeerTick = 1'024;
		std::size_t MaximumJournalRecordsPerTick = 32'768;
		std::size_t PlanningWorkPerTick = 65'536;
		std::size_t PlanningPeerQuantum = 2'048;

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
		std::uint64_t DependencyPlanRebuilds = 0;
		std::uint64_t DependencyPlanCacheHits = 0;
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
		std::uint64_t AcceptedAncestryObjects = 0;
		std::uint64_t AcceptedAncestryLogicalBytes = 0;
		std::uint64_t CatalogRetiredObjects = 0;
		std::uint64_t CatalogRetiredHighWater = 0;
		std::uint64_t CatalogRetirementExaminations = 0;
		std::uint64_t CatalogRetirementMaximumTickExaminations = 0;
		std::uint64_t CatalogRetirementReleases = 0;
		std::uint64_t CatalogRetentionLogicalBytes = 0;
		std::uint64_t CatalogReferenceIndexBytes = 0;
		std::uint64_t PlanningWork = 0;
		std::uint64_t PlanningMaximumTickWork = 0;
		std::uint64_t PlanningServiceOpportunities = 0;
		std::uint64_t PlanningMaximumServiceGapTicks = 0;
		std::uint64_t PlanningMaximumPeerSlice = 0;
		std::uint64_t PlanningResumes = 0;
		std::uint64_t PlanningInvalidations = 0;
		std::uint64_t PlanningReadyBatches = 0;
		std::uint64_t PlanningRecords = 0;
		std::uint64_t PlanningRecordsHighWater = 0;
		std::uint64_t PlanningPeerRecordsHighWater = 0;
		std::uint64_t PlanningCpuNanoseconds = 0;
	};

	struct ReplicationProduceResult {
		std::optional<ReplicationFrame> Frame;
		std::string Error;
		std::size_t SelectedTransitions = 0;
		// Includes copied/coalesced/skipped records and byte-limit retries, not just emitted operations.
		std::size_t JournalRecordsExamined = 0;
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
		// Internal runtime continuation path. Legacy synchronous fixture helpers
		// remain available as an independent semantic oracle.
		[[nodiscard]] ReplicationScheduleResult RegisterPeerPlanned(ConnectionId Connection, ReplicationEpoch Epoch,
			std::shared_ptr<const PeerRelevanceSelection> Selection);
		[[nodiscard]] ReplicationScheduleResult RequestPlanning(ConnectionId Connection,
			std::shared_ptr<const PeerRelevanceSelection> Selection, std::uint64_t SimulationTick);
		void ProcessPlanning(std::uint64_t SimulationTick);
		[[nodiscard]] bool IsPlanningReady(ConnectionId Connection) const;
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
			std::size_t MaximumFrameBytes = MaximumReplicationFrameBytes,
			std::size_t MaximumJournalRecords = MaximumStructuralJournalRecordsPerCall);
		[[nodiscard]] std::uint64_t GetJournalLag(ConnectionId Connection) const;
		[[nodiscard]] ReplicationProduceResult SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant);
		bool RemovePeer(ConnectionId Connection);
		// Internal Main-thread maintenance. Repeated/older tick values cannot refill
		// the shared retirement budget; no per-peer or per-refresh allowance exists.
		void ProcessCatalogRetirement(std::uint64_t SimulationTick);
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
		struct PlanningContinuation;
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
		struct AcceptedParentState {
			ObjectId Parent; // Invalid denotes no parent, not an unknown identity.
			std::uint64_t JournalEnd = 0; // Parent-only coalescing watermark.
			// Ownership only, not Known or a materialization epoch. Prepared parents
			// retain it before acceptance; accepted parents retain it until removal.
			std::shared_ptr<const ObjectId> CatalogRetention;
			// Exact non-nil native references represented by accepted GRPL work.
			// Frozen schema pointers, never Instance pointers or authoritative edges.
			std::vector<std::pair<const InstanceProperty *, ObjectId>> References;
		};
		struct CatalogEntry {
			std::shared_ptr<const StructuralMaterializationTemplate> Template;
			// Stable across publication revisions, never across full ObjectIds.
			std::shared_ptr<const ObjectId> Retention;
			// Borrowed nodes of this entry's immutable publication, selected using
			// canonical native reference metadata. Replaced with the whole entry.
			std::vector<const std::map<std::string, WireValue>::value_type *> ReferenceProperties;
			CatalogEntry(std::shared_ptr<const StructuralMaterializationTemplate> Value,
				std::shared_ptr<const ObjectId> RetentionValue = {});
			std::size_t GetReferenceIndexBytes() const {
				return sizeof(ReferenceProperties) + ReferenceProperties.capacity() * sizeof(decltype(ReferenceProperties)::value_type);
			}
			const StructuralMaterializationTemplate *operator->() const { return Template.get(); }
		};
		using AcceptedParentMap = std::map<ObjectId, AcceptedParentState>;
		struct PreparedStructuralCommit {
			ReliableReplicationSequence Sequence;
			ReliableReplicationSequence NextSequence;
			std::optional<ChangeCursor> JournalCursor;
			std::uint64_t PublicationJournalEnd = 0;
			std::vector<ObjectId> Entering;
			std::vector<ObjectId> Leaving;
			std::uint64_t PublishedObjects = 0;
			std::uint64_t UnpublishedObjects = 0;
			std::uint64_t DestroyedObjects = 0;
			std::uint64_t DeadlineMisses = 0;
			std::size_t TransitionCount = 0;
			AcceptedParentMap Parents;
		};

		struct PeerState {
			ReplicationView View;
			ChangeCursor JournalCursor;
			ReliableReplicationSequence NextSequence{1};
			// Exclusive source cursor captured by each accepted complete Enter.
			// Only lagging current materializations need an entry; never a payload.
			std::map<ObjectId, std::uint64_t> PublicationJournalEnds;
			// Derived metadata for actual Known identities, changed only with the
			// accepted frame. Current catalog ancestry is not accepted ancestry.
			AcceptedParentMap AcceptedParents;
			std::size_t AcceptedReferenceBytes = 0;
			std::set<ObjectId> DesiredObjects;
			std::set<ObjectId> RequiredObjects;
			PeerRelevanceSelection LastSelection;
			ChangeCursor DesiredDependencyCursor;
			std::map<ObjectId, PendingTransition> PendingTransitions;
			std::deque<PendingQueueEntry> CriticalQueue;
			std::deque<PendingQueueEntry> OrdinaryQueue;
			std::map<ObjectId, std::vector<ObjectId>> LeavingDependents;
			std::optional<PreparedStructuralCommit> PreparedCommit;
			std::uint64_t NextPendingToken = 1;
			bool ExplicitSchedulerCommit = false;
			bool PolicyManaged = false;
			std::shared_ptr<const PeerRelevanceSelection> PlanningSelection;
			std::shared_ptr<const PeerRelevanceSelection> ResolvedSelection;
			std::shared_ptr<PlanningContinuation> Planning;
			std::uint64_t AcceptedRevision = 0;
			std::uint64_t LastPlanningServiceTick = 0;
			std::size_t PlanningFrameLimit = 0;
			std::size_t PlanningInputRecords = 0;
			bool Planned = false;
			bool PlanningAwaitingSelection = false;
			std::string PlanningError;
		};
		std::shared_ptr<Instance> SourceRoot;
		InitialRelevancePolicy IsInitiallyRelevant;
		bool StructuralTemplateReuseEnabled = true;
		StructuralReplicationConfiguration Configuration;
		std::uint64_t StandaloneSchedulingTick = 0;
		std::uint64_t LatestSchedulingTick = 0;
		ObjectId WorldGeneration;
		std::map<ObjectId, CatalogEntry> Catalog;
		std::set<ObjectId> RequestedTemplates;
		std::set<ObjectId> RetiredObjects;
		ObjectId RetirementAfter;
		std::uint64_t RetirementTick = 0;
		std::size_t RetirementRemaining = MaximumCatalogRetirementExaminationsPerTick;
		ChangeCursor CatalogCursor;
		// Latest committed catalog boundary that changed live identities, ancestry,
		// or hard-reference edges. Ordinary publication revisions are independent.
		ChangeCursor DependencyCursor;
		ChangeCursor PlanningCursor;
		std::uint64_t PlanningTick = 0;
		std::size_t PlanningRemaining = 0;
		std::size_t PlanningRecordCount = 0;
		ConnectionId PlanningAfter;
		std::set<ConnectionId> PlanningPeers;
		std::map<ConnectionId, std::shared_ptr<PlanningContinuation>> DetachedPlanning;
		std::map<ConnectionId, PeerState> Peers;
		std::size_t PendingTransitionCount = 0;
		ReplicationMetrics Metrics;
		ReplicationProduceResult ProducePlannedFrame(ConnectionId Connection, ReplicationMessageKind Kind,
			std::size_t MaximumTransitions, std::uint64_t SimulationTick, std::size_t MaximumFrameBytes);

		bool RefreshCatalog(std::string &Error);
		void BeginRetirementTick(std::uint64_t SimulationTick);
		void ReclaimRetiredTemplates();
		bool BuildDependencyClosure(
			const PeerRelevanceSelection &Selection, std::set<ObjectId> &Closure, std::string &Error
		);
		bool RecordDesiredState(
			PeerState &Peer, const PeerRelevanceSelection &Selection, std::uint64_t SimulationTick, std::string &Error
		);
		void RefreshPendingMetrics(ReplicationMetrics &Snapshot) const;
		void CompactPendingQueues(PeerState &Peer);
		void ApplyPreparedCommit(PeerState &Peer, PreparedStructuralCommit Commit);
		AcceptedParentMap CaptureAcceptedParents(const PeerState &Peer, const ReplicationFrame &Frame, std::uint64_t JournalEnd);
		static void ApplyAcceptedParents(PeerState &Peer, AcceptedParentMap Parents);
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
