#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace gargantuan::runtime_detail {

// Internal, opt-in diagnostics. Captures own fixed storage on Main and do not
// alter runtime scheduling, authority, or worker execution. Nested durations
// are inclusive. ExclusiveNanoseconds subtracts nested scopes in the same
// capture and can be summed within that capture (not across nested captures).
enum class WorkPhase : std::size_t {
	ContentStep, DetachedPreparation, ContentCommit, EngineActivation,
	EngineStep, Physics, Animation, RootMotion, RenderPublication,
	SessionPoll, SessionStep, SpatialUpdate, Relevance, DesiredState,
	CatalogRefresh, StructuralSelection, IncrementalPreparation, PeerViewCopy,
	StructuralEncode, StructuralCommit, GraphSynchronization,
	CharacterSimulation, CharacterPublication, RemotePump, SchedulerFlush,
	TransportSend, ClientDecode, ClientApply,
	PhysicsUpdate, PhysicsRigidStep, PhysicsEvents, PhysicsSoftColliders, PhysicsSoftStep,
	RelevanceQuery, RelevanceSelection, ActionService, ActionRootMotion,
	DependencyClosure, PendingDiscovery, LeaveDependencyIndex, StructuralFixupPlanning, StructuralGroupSelection,
	PhysicsBodyCreate, PhysicsBodyDestroy, PhysicsConstraintCreate, PhysicsConstraintDestroy, PhysicsBackendStep,
	GraphMetrics, PhysicsPairsProfile, PhysicsCollideProfile, PhysicsSolveProfile, PhysicsSensorsProfile,
	CatalogRetirement, RelevantRebuild, RemoteMaterialization, StructuralClearFixups, StructuralRestoreFixups,
	DependencySeedPreparation, DependencyEdgeWalk, FixupDesiredScan, FixupReferenceCosts, FixupAcceptedScan, FixupGroupCosts,
	StructuralPlanning, RelevancePeer, RelevanceHysteresis, RelevanceCommit,
	PlanningValidation, PlanningResume, PlanningCleanup, PlanningInstall,
	StructuralFrameBuild, StructuralValidationEncode, StructuralSubmissionEncode,
	StructuralIntent, StructuralSubmit, StructuralAcceptance, KnownCommit, AcceptedMetadataCommit,
	Count
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(WorkPhase::Count)> WorkPhaseNames{
	"ContentStep", "DetachedPreparation", "ContentCommit", "EngineActivation",
	"EngineStep", "Physics", "Animation", "RootMotion", "RenderPublication",
	"SessionPoll", "SessionStep", "SpatialUpdate", "Relevance", "DesiredState",
	"CatalogRefresh", "StructuralSelection", "IncrementalPreparation", "PeerViewCopy",
	"StructuralEncode", "StructuralCommit", "GraphSynchronization",
	"CharacterSimulation", "CharacterPublication", "RemotePump", "SchedulerFlush",
	"TransportSend", "ClientDecode", "ClientApply",
	"PhysicsUpdate", "PhysicsRigidStep", "PhysicsEvents", "PhysicsSoftColliders", "PhysicsSoftStep",
	"RelevanceQuery", "RelevanceSelection", "ActionService", "ActionRootMotion",
	"DependencyClosure", "PendingDiscovery", "LeaveDependencyIndex", "StructuralFixupPlanning", "StructuralGroupSelection",
	"PhysicsBodyCreate", "PhysicsBodyDestroy", "PhysicsConstraintCreate", "PhysicsConstraintDestroy", "PhysicsBackendStep",
	"GraphMetrics", "PhysicsPairsProfile", "PhysicsCollideProfile", "PhysicsSolveProfile", "PhysicsSensorsProfile",
	"CatalogRetirement", "RelevantRebuild", "RemoteMaterialization", "StructuralClearFixups", "StructuralRestoreFixups",
	"DependencySeedPreparation", "DependencyEdgeWalk", "FixupDesiredScan", "FixupReferenceCosts", "FixupAcceptedScan", "FixupGroupCosts",
	"StructuralPlanning", "RelevancePeer", "RelevanceHysteresis", "RelevanceCommit",
	"PlanningValidation", "PlanningResume", "PlanningCleanup", "PlanningInstall",
	"StructuralFrameBuild", "StructuralValidationEncode", "StructuralSubmissionEncode",
	"StructuralIntent", "StructuralSubmit", "StructuralAcceptance", "KnownCommit", "AcceptedMetadataCommit"
};

struct WorkDuration {
	std::uint64_t Nanoseconds = 0;
	std::uint64_t Calls = 0;
	std::uint64_t FirstStartedNanoseconds = 0;
	std::uint64_t LastEndedNanoseconds = 0;
	std::uint64_t Units = 0;
	std::uint64_t Allocations = 0;
	std::uint64_t ExclusiveNanoseconds = 0;
	std::uint64_t AllocatedBytes = 0;
	std::uint64_t Retained = 0;
	std::uint64_t Rejected = 0;
};
// Trusted producer annotations are diagnostics only: never serialized and never
// consulted for scheduler selection, ordering, acceptance or authority.
enum class WorkProducer : std::size_t {
	Other, Structural, Bootstrap, SessionControl, CharacterReliable, Gchr,
	OwnerAction, RemoteEventReliable, RemoteEventUnreliable, RpcRequest, RpcResponse, RemoteControl, CharacterInput, Count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(WorkProducer::Count)> WorkProducerNames{
	"Other", "Structural", "Bootstrap", "SessionControl", "CharacterReliable", "Gchr",
	"OwnerAction", "RemoteEventReliable", "RemoteEventUnreliable", "RpcRequest", "RpcResponse", "RemoteControl", "CharacterInput"
};
struct WorkProducerSample {
	std::uint64_t Enqueued = 0, EnqueuedBytes = 0, Serviced = 0, ServicedBytes = 0;
	std::uint64_t Opportunities = 0, BudgetDeferrals = 0, CapacityDeferrals = 0;
	std::uint64_t QueueDepthMaximum = 0, ServiceAgeMaximumNanoseconds = 0, ServiceGapMaximumNanoseconds = 0;
};
// Fixed opt-in counters: counts describe actual loop visits, not inferred work.
enum class WorkCounter : std::size_t {
	RelevancePeers, RelevanceRoots, RelevanceMembers, DependencySeeds, DependencyEdges,
	DependencyDuplicates, DependencyStale, DiscoveryPeers, PendingExamined, DesiredExamined,
	KnownExamined, AlreadyKnown, AlreadyPending, CandidateAttempts, CandidateInserted,
	CandidateReplanned, CandidateCancelled, GraphPeers, GraphCharacters, GraphRemotes,
	GraphChanges, GraphKnownChecks, GraphIrrelevant, RemoteRegistryExamined,
	JournalLagBeforeServiceMaximum, JournalLagAfterServiceMaximum, JournalCatalogSequence,
	RetirementObjectsExamined, RetirementPeersExamined, RetirementTemplatesReleased,
	PlanHits, PlanMissSelection, PlanMissTopology, PlanMissEmpty, CatalogBatches,
	CatalogPayloadOnlyBatches, CatalogTopologyBatches, RelevantRebuilt, PeerViewCopiedIdentities,
	FixupReferrers, FixupProperties, EncodeRetries,
	ClientCopiedIdentities, ClientIndexedIdentities, ClientPreflightIdentities, ClientReceiverVisits,
	ClientRemovalVisits,
	FixupPlanningCalls, FixupReferenceValues, FixupHardNilValues, FixupScalarValues,
	ClearFixupReferrers, ClearFixupProperties, RestoreFixupReferrers, RestoreFixupProperties,
	FixupsEmitted, DependencyGroupsBuilt, DependencyGroupsDeferred,
	AcceptedReferenceObjectsExamined, AcceptedReferencesExamined, RemovalReferencesPlanned,
	FixupParentChanges, FixupEmptyReferenceObjects, FixupEnterCostCandidates, FixupPendingEnterCosts,
	AcceptedEmptyReferenceObjects, RestoreMatchedReferences,
	PlanningLookupAdvances, PlanningLookupSearches, PlanningLookupMaximumAdvances,
	PlanningCharged, PlanningResumes, PlanningCompletedGroups, PlanningReadyBatches, PlanningRecordsHighWater, PlanningServiceGap,
	RelevanceUnchangedRoots, RelevanceUnchangedSelection, RelevanceOldRootVisits, RelevanceCandidateVisits,
	RelevanceNoopQueryCandidates,
	Count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(WorkCounter::Count)> WorkCounterNames{
	"RelevancePeers", "RelevanceRoots", "RelevanceMembers", "DependencySeeds", "DependencyEdges",
	"DependencyDuplicates", "DependencyStale", "DiscoveryPeers", "PendingExamined", "DesiredExamined",
	"KnownExamined", "AlreadyKnown", "AlreadyPending", "CandidateAttempts", "CandidateInserted",
	"CandidateReplanned", "CandidateCancelled", "GraphPeers", "GraphCharacters", "GraphRemotes",
	"GraphChanges", "GraphKnownChecks", "GraphIrrelevant", "RemoteRegistryExamined",
	"JournalLagBeforeServiceMaximum", "JournalLagAfterServiceMaximum", "JournalCatalogSequence",
	"RetirementObjectsExamined", "RetirementPeersExamined", "RetirementTemplatesReleased",
	"PlanHits", "PlanMissSelection", "PlanMissTopology", "PlanMissEmpty", "CatalogBatches",
	"CatalogPayloadOnlyBatches", "CatalogTopologyBatches", "RelevantRebuilt", "PeerViewCopiedIdentities",
	"FixupReferrers", "FixupProperties", "EncodeRetries",
	"ClientCopiedIdentities", "ClientIndexedIdentities", "ClientPreflightIdentities", "ClientReceiverVisits",
	"ClientRemovalVisits",
	"FixupPlanningCalls", "FixupReferenceValues", "FixupHardNilValues", "FixupScalarValues",
	"ClearFixupReferrers", "ClearFixupProperties", "RestoreFixupReferrers", "RestoreFixupProperties",
	"FixupsEmitted", "DependencyGroupsBuilt", "DependencyGroupsDeferred",
	"AcceptedReferenceObjectsExamined", "AcceptedReferencesExamined", "RemovalReferencesPlanned",
	"FixupParentChanges", "FixupEmptyReferenceObjects", "FixupEnterCostCandidates", "FixupPendingEnterCosts",
	"AcceptedEmptyReferenceObjects", "RestoreMatchedReferences",
	"PlanningLookupAdvances", "PlanningLookupSearches", "PlanningLookupMaximumAdvances",
	"PlanningCharged", "PlanningResumes", "PlanningCompletedGroups", "PlanningReadyBatches", "PlanningRecordsHighWater", "PlanningServiceGap",
	"RelevanceUnchangedRoots", "RelevanceUnchangedSelection", "RelevanceOldRootVisits", "RelevanceCandidateVisits",
	"RelevanceNoopQueryCandidates"
};
struct WorkSample : std::array<WorkDuration, WorkPhaseNames.size()> {
	std::array<WorkProducerSample, WorkProducerNames.size()> Producers{};
	std::array<std::uint64_t, WorkCounterNames.size()> Counters{};
};
inline thread_local WorkSample *ActiveWorkSample = nullptr;
inline thread_local WorkProducer ActiveWorkProducer = WorkProducer::Other;
class WorkProducerScope final {
  public:
	explicit WorkProducerScope(WorkProducer Producer) noexcept : Previous(ActiveWorkProducer) {
		ActiveWorkProducer = Producer;
	}
	~WorkProducerScope() { ActiveWorkProducer = Previous; }
	WorkProducerScope(const WorkProducerScope &) = delete;
	WorkProducerScope &operator=(const WorkProducerScope &) = delete;
  private:
	WorkProducer Previous;
};
inline WorkProducerSample *CurrentProducerSample(WorkProducer Producer) noexcept {
	return ActiveWorkSample ? &ActiveWorkSample->Producers[static_cast<std::size_t>(Producer)] : nullptr;
}
inline std::uint64_t WorkTimestamp() noexcept {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
using WorkAllocationReader = std::uint64_t (*)() noexcept;
inline thread_local WorkAllocationReader ActiveWorkAllocationReader = nullptr;
inline thread_local WorkAllocationReader ActiveWorkAllocationBytesReader = nullptr;
class WorkScope;
inline thread_local WorkScope *ActiveWorkScope = nullptr;

inline void AddWorkCounter(std::uint64_t &Value, std::uint64_t Count) noexcept {
	Value += std::min(Count, std::numeric_limits<std::uint64_t>::max() - Value);
}

inline void CountWork(WorkCounter Counter, std::uint64_t Count = 1) noexcept {
	if (ActiveWorkSample) AddWorkCounter(ActiveWorkSample->Counters[static_cast<std::size_t>(Counter)], Count);
}
inline void MaximumWork(WorkCounter Counter, std::uint64_t Value) noexcept {
	if (!ActiveWorkSample) return;
	auto &Maximum = ActiveWorkSample->Counters[static_cast<std::size_t>(Counter)];
	Maximum = std::max(Maximum, Value);
}

// Backend-reported subprofiles are inclusive diagnostic values, not additional
// timed scopes. Keep their exclusive time zero to avoid charging twice.
inline void RecordReportedWork(WorkPhase Phase, std::uint64_t Nanoseconds) noexcept {
	if (!ActiveWorkSample) return;
	auto &Value = (*ActiveWorkSample)[static_cast<std::size_t>(Phase)];
	AddWorkCounter(Value.Nanoseconds, Nanoseconds);
	AddWorkCounter(Value.Calls, 1);
}

inline void RecordWorkDisposition(WorkPhase Phase, bool Retained, std::size_t Count = 1) noexcept {
	if (!ActiveWorkSample) return;
	auto &Value = (*ActiveWorkSample)[static_cast<std::size_t>(Phase)];
	AddWorkCounter(Retained ? Value.Retained : Value.Rejected, Count);
}

inline void RecordWorkUnits(WorkPhase Phase, std::size_t Count = 1) noexcept {
	if (!ActiveWorkSample) return;
	auto &Units = (*ActiveWorkSample)[static_cast<std::size_t>(Phase)].Units;
	Units += std::min<std::uint64_t>(Count, std::numeric_limits<std::uint64_t>::max() - Units);
}

class WorkCapture final {
  public:
	explicit WorkCapture(WorkSample *Sample, WorkAllocationReader Allocations = nullptr,
		WorkAllocationReader AllocationBytes = nullptr) noexcept
		: Previous(ActiveWorkSample), PreviousAllocationReader(ActiveWorkAllocationReader),
		  PreviousBytesReader(ActiveWorkAllocationBytesReader), PreviousScope(ActiveWorkScope) {
		ActiveWorkSample = Sample;
		ActiveWorkAllocationReader = Allocations;
		ActiveWorkAllocationBytesReader = AllocationBytes;
		ActiveWorkScope = nullptr;
	}
	~WorkCapture() {
		ActiveWorkSample = Previous;
		ActiveWorkAllocationReader = PreviousAllocationReader;
		ActiveWorkAllocationBytesReader = PreviousBytesReader;
		ActiveWorkScope = PreviousScope;
	}
	WorkCapture(const WorkCapture &) = delete;
	WorkCapture &operator=(const WorkCapture &) = delete;
  private:
	WorkSample *Previous;
	WorkAllocationReader PreviousAllocationReader;
	WorkAllocationReader PreviousBytesReader;
	WorkScope *PreviousScope;
};

class WorkScope final {
  public:
	explicit WorkScope(WorkPhase Phase) noexcept
		: Target(ActiveWorkSample ? &(*ActiveWorkSample)[static_cast<std::size_t>(Phase)] : nullptr) {
		if (Target) {
			Parent = ActiveWorkScope;
			ActiveWorkScope = this;
			Started = std::chrono::steady_clock::now();
			AllocationReader = ActiveWorkAllocationReader;
			if (AllocationReader) StartedAllocations = AllocationReader();
			BytesReader = ActiveWorkAllocationBytesReader;
			if (BytesReader) StartedBytes = BytesReader();
		}
	}
	~WorkScope() {
		if (!Target) return;
		const auto Ended = std::chrono::steady_clock::now();
		const auto Elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			Ended - Started).count());
		ActiveWorkScope = Parent;
		if (Parent) AddWorkCounter(Parent->ChildNanoseconds, Elapsed);
		AddWorkCounter(Target->ExclusiveNanoseconds, Elapsed - std::min(Elapsed, ChildNanoseconds));
		const auto StartedNanoseconds = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Started.time_since_epoch()).count());
		if (Target->Calls == 0 || StartedNanoseconds < Target->FirstStartedNanoseconds)
			Target->FirstStartedNanoseconds = StartedNanoseconds;
		Target->LastEndedNanoseconds = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Ended.time_since_epoch()).count());
		Target->Nanoseconds += std::min(Elapsed, std::numeric_limits<std::uint64_t>::max() - Target->Nanoseconds);
		if (Target->Calls != std::numeric_limits<std::uint64_t>::max()) ++Target->Calls;
		if (AllocationReader) {
			const auto Current = AllocationReader();
			const auto Count = Current - std::min(Current, StartedAllocations);
			Target->Allocations += std::min(Count, std::numeric_limits<std::uint64_t>::max() - Target->Allocations);
		}
		if (BytesReader) {
			const auto Current = BytesReader();
			AddWorkCounter(Target->AllocatedBytes, Current - std::min(Current, StartedBytes));
		}
	}
	WorkScope(const WorkScope &) = delete;
	WorkScope &operator=(const WorkScope &) = delete;
  private:
	WorkDuration *Target;
	std::chrono::steady_clock::time_point Started{};
	WorkAllocationReader AllocationReader = nullptr;
	std::uint64_t StartedAllocations = 0;
	WorkScope *Parent = nullptr;
	std::uint64_t ChildNanoseconds = 0;
	WorkAllocationReader BytesReader = nullptr;
	std::uint64_t StartedBytes = 0;
};

template <class Operation> decltype(auto) MeasureWork(WorkPhase Phase, Operation &&Run) {
	WorkScope Scope(Phase);
	return std::forward<Operation>(Run)();
}
}
