#pragma once

#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "PooledReliableServiceRecoveryContractFixture.hpp"
#include "WorkloadTimingEvidence.hpp"
#include "WorkloadActionEvidence.hpp"
#include "WorkloadRemoteEvidence.hpp"
#include "AggregateWorkloadEvidence.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "../src/network/GnsServiceDiagnostics.hpp"
#include "../src/host/server/FarmServiceTimingEvidence.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <limits>
#include <cstdlib>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingsockets_flat.h>

// Real GameSession/GNS qualification, deliberately separate from the transport
// capacity benchmark. All completions traverse production Remote dispatch.
namespace {

// One preceding client poll, retained at the first execution fault. No output,
// heap allocation, native worker observation or changed acceptance timestamp.
struct PooledPollAttribution {
	using Record = SteamNetworkingSocketsLib::GargantuanServiceTimingRecord;
	std::array<Record, 128> Records{};
	std::array<Record, 8> Longest{}; // Preserve early lock/dispatch waits after ring wrap.
	std::size_t Count = 0;
	std::uint64_t Step = 0, BeginNs = 0, EndNs = 0;
	bool Active = false, Frozen = false, Suppressed = false;
	SteamNetworkingSocketsLib::GargantuanServiceTimingSink Sink;
	PooledPollAttribution() {
		Sink.Context = this; Sink.CaptureCallsites = true;
		Sink.Record = [](void *Context, const Record &Value) noexcept {
			auto &Self = *static_cast<PooledPollAttribution *>(Context);
			if (!Self.Active) return;
			Self.Records[Self.Count++ % Self.Records.size()] = Value;
			const auto Index = Value.Detail >= 101 && Value.Detail <= 105 ? Value.Detail - 101 :
				(Value.Detail >= 201 && Value.Detail <= 203 ? Value.Detail - 201 + 5 : 8);
			if (Index < Self.Longest.size() && Value.ClockValid &&
				Value.EndQpc - Value.BeginQpc > Self.Longest[Index].EndQpc - Self.Longest[Index].BeginQpc)
				Self.Longest[Index] = Value;
		};
	}
	void Begin(std::uint64_t CurrentStep, std::uint64_t At) noexcept {
		if (Frozen) return;
		Count = 0; Longest = {}; Step = CurrentStep; BeginNs = At; EndNs = 0;
		Suppressed = SteamNetworkingSocketsLib::GargantuanServiceTimingObserver.load(std::memory_order_acquire) != nullptr;
		Active = !Suppressed; // Preserve an explicitly requested full observer.
	}
	void End(std::uint64_t At) noexcept { if (Active) { EndNs = At; Active = false; } }
	void Freeze() noexcept { Frozen = true; } // Current poll's nested scopes may unwind.
	void Reset() noexcept { Count = 0; Longest = {}; Active = Frozen = Suppressed = false; BeginNs = EndNs = Step = 0; }
	void Print(std::string_view Case) const {
		std::cout << "[Qualification:PollCallsites] case=" << Case << " step=" << Step
			<< " begin_ns=" << BeginNs << " end_ns=" << EndNs << " scopes=" << Count
			<< " overwritten=" << (Count > Records.size() ? Count - Records.size() : 0)
			<< " full_observer_active=" << Suppressed << " observer_cost_unmeasured=1 clock=qpc nested=1\n";
		for (auto Index = Count > Records.size() ? Count - Records.size() : 0; Index < Count; ++Index) {
			const auto &Value = Records[Index % Records.size()];
			std::cout << "[Qualification:PollCallsite] case=" << Case << " step=" << Step
				<< " site=" << Value.Detail << " handle=" << Value.Result << " thread=" << Value.ThreadId
				<< " valid=" << Value.ClockValid << " begin_qpc=" << Value.BeginQpc << " end_qpc=" << Value.EndQpc << '\n';
		}
		for (const auto &Value : Longest) if (Value.ClockValid)
			std::cout << "[Qualification:PollLongestCallsite] case=" << Case << " step=" << Step
				<< " site=" << Value.Detail << " handle=" << Value.Result << " thread=" << Value.ThreadId
				<< " begin_qpc=" << Value.BeginQpc << " end_qpc=" << Value.EndQpc << '\n';
	}
};

int DiagnosticDimension(const char *Name, int Default, int Minimum, int Maximum) {
	const auto *Text = std::getenv(Name);
	if (!Text) return Default;
	return std::clamp(std::atoi(Text), Minimum, Maximum);
}

// Whole-lifetime, opt-in scalar capture. This owner is declared before every
// transport; native timing is read only after transport destruction has joined.
struct PooledExecutionAttribution {
	enum class Stage : unsigned { Setup, Warmup, Demand, Drain, Report, CaseCleanup, FinalStop, NativeJoin };
	struct Fault {
		bool Observed = false;
		std::string_view Case;
		Stage Lifecycle = Stage::Setup;
		unsigned Phase = 0;
		std::uint64_t PhaseBeginNs = 0, Step = 0, PriorAt = 0, Now = 0, Tick = 0, DebtToken = 0, DebtBytes = 0;
		ConnectionId Connection;
		detail::PooledObservationPoint Point = detail::PooledObservationPoint::Other;
		detail::PooledExecutionState Before;
		detail::PooledReliableServiceFeedback::Result Result;
		bool Rejected = false, InvalidAfter = false, WorkReady = false, AcceptanceAvailable = true;
		test_detail::WorkloadClockAnchor Clock;
	} First;
	bool Enabled = false, LifecycleObserved = false, PostFaultLifecycleObserved = false;
	std::string_view CurrentCase = "setup", LifecycleCase;
	Stage CurrentStage = Stage::Setup, LifecycleStage = Stage::Setup;
	unsigned Phase = 0;
	std::uint64_t Step = 0, PhaseBeginNs = 0, LastAt = 0;
	std::uint64_t MaximumMain = 0, MaximumTurnover = 0, MaximumNativeReturn = 0;
	std::uint64_t LifecycleStep = 0, LifecyclePhaseBeginNs = 0;
	unsigned LifecyclePhase = 0;
	detail::PooledLifecycleRecord FirstLifecycle{};
	detail::PooledLifecycleRecord PostFaultLifecycle{};
	Stage PostFaultStage = Stage::Setup;
	std::uint64_t PostFaultStep = 0;
	test_detail::WorkloadClockAnchor LifecycleClock;
	detail::PooledServiceSink *Previous = nullptr;
	detail::PooledServiceSink Sink;
	std::unique_ptr<host::detail::FarmServiceTimingEvidence> Timing;
	explicit PooledExecutionAttribution(bool Capture, bool NativeTiming = true) : Enabled(Capture) {
		if (!Enabled) return;
		if (NativeTiming) Timing = std::make_unique<host::detail::FarmServiceTimingEvidence>(true, false, true);
		Previous = detail::ActivePooledService;
		Sink.Context = this; Sink.CaptureExecution = true;
		Sink.Record = [](void *Context, const detail::PooledServiceRecord &Value) noexcept {
			auto &Self = *static_cast<PooledExecutionAttribution *>(Context);
			Self.MaximumMain = std::max(Self.MaximumMain, Value.Result.MaximumMainOpportunityMicroseconds);
			Self.MaximumTurnover = std::max(Self.MaximumTurnover, Value.Result.MaximumTurnoverMicroseconds);
			Self.MaximumNativeReturn = std::max(Self.MaximumNativeReturn, Value.Result.MaximumNativeCapacityReturnMicroseconds);
			if (!Self.First.Observed && (Value.ExecutionRejected || Value.Result.ExecutionFailed || Value.InvalidAfter)) {
				auto &Saved = Self.First;
				Saved.Observed = true; Saved.Case = Self.CurrentCase; Saved.Lifecycle = Self.CurrentStage;
				Saved.Phase = Self.Phase; Saved.PhaseBeginNs = Self.PhaseBeginNs; Saved.Step = Self.Step;
				Saved.PriorAt = Self.LastAt; Saved.Now = Value.NowMicroseconds; Saved.Tick = Value.SimulationTick;
				Saved.Connection = Value.Connection; Saved.DebtToken = Value.DebtToken; Saved.DebtBytes = Value.DebtBytes;
				Saved.Point = Value.Point; Saved.Before = Value.ExecutionBefore; Saved.Result = Value.Result;
				Saved.Rejected = Value.ExecutionRejected; Saved.InvalidAfter = Value.InvalidAfter; Saved.WorkReady = Value.WorkReady;
				Saved.AcceptanceAvailable = Value.AcceptanceAvailable;
				Saved.Clock = test_detail::WorkloadClockAnchor::Capture();
				if (Self.Timing) Self.Timing->Freeze(Value.NowMicroseconds);
			}
			Self.LastAt = Value.NowMicroseconds;
			if (Self.Previous && Self.Previous->Record && (!Value.DiagnosticOnly || Self.Previous->CaptureExecution))
				Self.Previous->Record(Self.Previous->Context, Value);
		};
		Sink.AcceptedGrant = [](void *Context, ConnectionId Id, std::uint64_t Token, std::uint64_t Bytes, std::uint64_t At) noexcept {
			auto &Self = *static_cast<PooledExecutionAttribution *>(Context);
			if (Self.Previous && Self.Previous->AcceptedGrant) Self.Previous->AcceptedGrant(Self.Previous->Context, Id, Token, Bytes, At);
		};
		Sink.Lifecycle = [](void *Context, const detail::PooledLifecycleRecord &Value) noexcept {
			auto &Self = *static_cast<PooledExecutionAttribution *>(Context);
			if (!Self.LifecycleObserved) {
				Self.LifecycleObserved = true; Self.FirstLifecycle = Value; Self.LifecycleCase = Self.CurrentCase;
				Self.LifecycleStage = Self.CurrentStage; Self.LifecycleClock = test_detail::WorkloadClockAnchor::Capture();
				Self.LifecycleStep = Self.Step; Self.LifecyclePhase = Self.Phase; Self.LifecyclePhaseBeginNs = Self.PhaseBeginNs;
			}
			if (!Self.PostFaultLifecycleObserved && Self.First.Observed && Value.NowMicroseconds >= Self.First.Now) {
				Self.PostFaultLifecycleObserved = true; Self.PostFaultLifecycle = Value;
				Self.PostFaultStage = Self.CurrentStage; Self.PostFaultStep = Self.Step;
			}
			if (Self.Previous && Self.Previous->Lifecycle) Self.Previous->Lifecycle(Self.Previous->Context, Value);
		};
		detail::ActivePooledService = &Sink;
	}
	~PooledExecutionAttribution() {
		if (!Enabled) return;
		detail::ActivePooledService = Previous;
		const bool Joined = !Timing || detail::NativeDiagnosticQuiescent();
		const bool Cleared = Joined && (!Timing || Timing->Stop());
		std::cout << "[Qualification:ExecutionCapture] joined=" << Joined << " sink_removed=" << Cleared
			<< " incomplete=" << (!Joined || !Cleared) << '\n';
		Print();
		if (Timing && Joined && Cleared) Timing->Write(std::cout, "pooled-execution-attribution", true);
		else if (Timing) {
			// A cached native span can still Finish after bounded Stop retained its
			// domain. Retain the installed heap sink/history; never Visit or free it.
			(void)Timing.release();
		}
	}
	void Print() const {
		const auto &F = First;
		std::cout << "[Qualification:Performance] scope=aggregate-execution contract=" << CandidateReliableService().ContractIdentity()
			<< " main_us=" << MaximumMain << " main_target_us=100000 turnover_us=" << MaximumTurnover
			<< " turnover_target_us=150000 native_return_us=" << MaximumNativeReturn << " native_return_target_us=100000 fatal=0\n";
		std::cout << "[Qualification:ExecutionFirst] observed=" << F.Observed << " owner_measured=0 stack_measured=0";
		if (F.Observed) std::cout << " case=" << F.Case << " stage=" << static_cast<unsigned>(F.Lifecycle)
			<< " phase=" << F.Phase << " phase_begin_ns=" << F.PhaseBeginNs << " step=" << F.Step
			<< " prior_us=" << F.PriorAt << " now_us=" << F.Now << " tick=" << F.Tick
			<< " slot=" << F.Connection.Slot << " generation=" << F.Connection.Generation
			<< " point=" << static_cast<unsigned>(F.Point) << " rejected=" << F.Rejected << " invalid_after=" << F.InvalidAfter
			<< " acceptance_available=" << F.AcceptanceAvailable
			<< " main_failed=" << F.Result.MainOpportunityFailed << " turnover_failed=" << F.Result.TurnoverFailed
			<< " max_main_us=" << F.Result.MaximumMainOpportunityMicroseconds << " max_turnover_us=" << F.Result.MaximumTurnoverMicroseconds
			<< " before_captured=" << F.Before.Captured << " before_invalid=" << F.Before.Invalid << " armed=" << F.Before.Armed
			<< " required_since_us=" << F.Before.RequiredSince << " boundary_us=" << F.Before.Boundary << " work_ready=" << F.WorkReady
			<< " offer_token=" << F.Before.OfferToken << " offer_bytes=" << F.Before.OfferBytes << " offer_at_us=" << F.Before.OfferAt
			<< " turnover_token=" << F.Before.TurnoverToken << " turnover_at_us=" << F.Before.TurnoverAt << " permit_available=" << F.Before.PermitAvailable
			<< " debt_token=" << F.DebtToken << " debt_bytes=" << F.DebtBytes
			<< " qpc_before=" << F.Clock.QpcBefore << " qpc_after=" << F.Clock.QpcAfter << " frequency=" << F.Clock.QpcFrequency;
		if (F.Observed) std::cout << " pid=" << F.Clock.Process << " native_tid=" << F.Clock.Thread
			<< " clock_valid=" << F.Clock.NativeValid << " steady_ns=" << F.Clock.SteadyNs << " has_boundary=" << F.Before.HasBoundary;
		std::cout << '\n' << "[Qualification:ExecutionLifecycle] observed=" << LifecycleObserved;
		if (LifecycleObserved) std::cout << " case=" << LifecycleCase << " stage=" << static_cast<unsigned>(LifecycleStage)
			<< " step=" << LifecycleStep << " phase=" << LifecyclePhase << " phase_begin_ns=" << LifecyclePhaseBeginNs
			<< " point=" << static_cast<unsigned>(FirstLifecycle.Point) << " role=" << unsigned(FirstLifecycle.Role)
			<< " status_before=" << unsigned(FirstLifecycle.StatusBefore) << " reason=" << FirstLifecycle.Reason
			<< " now_us=" << FirstLifecycle.NowMicroseconds << " qpc_before=" << LifecycleClock.QpcBefore << " qpc_after=" << LifecycleClock.QpcAfter;
		std::cout << '\n';
		std::cout << "[Qualification:ExecutionPostFaultLifecycle] observed=" << PostFaultLifecycleObserved;
		if (PostFaultLifecycleObserved) std::cout << " stage=" << static_cast<unsigned>(PostFaultStage)
			<< " step=" << PostFaultStep << " point=" << static_cast<unsigned>(PostFaultLifecycle.Point)
			<< " role=" << unsigned(PostFaultLifecycle.Role) << " status_before=" << unsigned(PostFaultLifecycle.StatusBefore)
			<< " reason=" << PostFaultLifecycle.Reason << " now_us=" << PostFaultLifecycle.NowMicroseconds;
		std::cout << '\n';
	}
};

// Missing generations and terminal release cannot stand in for ACK retirement.
bool AggregatePooledQuiescent(const detail::PooledPostpassSnapshot &Snapshot,
	std::span<const ConnectionId> Expected) {
	if (Expected.empty() || Snapshot.Peers.size() != Expected.size() ||
		Snapshot.Admission.TerminalReleasedBytes || Snapshot.Admission.OutstandingBytes ||
		Snapshot.Admission.ActiveDrainGrants ||
		Snapshot.Admission.AcceptedBytes != Snapshot.Admission.VerifiedAttributedRetirement) return false;
	std::uint64_t Accepted = 0, Retired = 0;
	for (std::size_t Index = 0; Index < Expected.size(); ++Index) {
		if (!Expected[Index].IsValid()) return false;
		for (std::size_t Prior = 0; Prior < Index; ++Prior) if (Expected[Prior] == Expected[Index]) return false;
		const auto Found = std::find_if(Snapshot.Peers.begin(), Snapshot.Peers.end(),
			[&](const auto &Value) { return Value.Connection == Expected[Index]; });
		if (Found == Snapshot.Peers.end()) return false;
		const auto &Value = *Found; const auto &Execution = Value.Execution;
		if (!Value.Ready || !Value.FeedbackComplete || !Value.Healthy || !Execution.Captured || Execution.Invalid || Execution.Armed ||
			Execution.OfferToken || Execution.OfferBytes || Execution.TurnoverToken || Value.DebtToken || Value.Debt ||
			Value.OrdinaryDebt || Value.OrdinaryUnsent || Value.NativePending || Value.NativeUnacked || Value.NativeGrantToken ||
			Value.Accepted.All != Value.ReconciledAccepted.All || Value.Accepted.Structural != Value.ReconciledAccepted.Structural ||
			Value.Accepted.Gameplay != Value.ReconciledAccepted.Gameplay || Value.Accepted.All != Value.ReliableAcked ||
			Value.Accepted.Structural != Value.FirstSent || Value.FirstSent != Value.Acked || Value.Acked != Value.Retired) return false;
		if (Value.Accepted.Structural > std::numeric_limits<std::uint64_t>::max() - Accepted ||
			Value.Retired > std::numeric_limits<std::uint64_t>::max() - Retired) return false;
		Accepted += Value.Accepted.Structural; Retired += Value.Retired;
	}
	return Accepted == Snapshot.Admission.AcceptedBytes && Retired == Snapshot.Admission.VerifiedAttributedRetirement;
}

void RunAggregateReliableGameplay(Engine &ServerRuntime, Engine &PrimaryRuntime,
	GameSession &Server, GameSession &Primary, std::uint16_t Port, std::uint64_t &Tick,
	const std::shared_ptr<RemoteFunction> &Function, const std::shared_ptr<RemoteEvent> &Event) {
	using Clock = std::chrono::steady_clock;
	const auto DiagnosticStart = Clock::now();
	PooledExecutionAttribution Attribution(CandidateReliableService().IsPooled(), false);
	const int PressureCount = QualificationDiagnostic ? DiagnosticDimension("KI008_GROUPS", 6, 0, 6) : 6;
	const int PressureBytes = QualificationDiagnostic ? DiagnosticDimension("KI008_BYTES", 24576, 1, 24576) : 24576;
	const bool Gameplay = !QualificationDiagnostic || DiagnosticDimension("KI008_GAMEPLAY", 1, 0, 1) != 0;
	// Preserve the backend reason without enabling per-message tracing, which
	// materially changes the saturation timing of the structural diagnostic.
	detail::GnsServiceSink Diagnostic{nullptr, nullptr, [](void *, ConnectionId Id, const char *Reason) noexcept {
		std::fprintf(stderr, "[Qualification:BackendClose] peer=%u:%u reason=%s\n", Id.Slot, Id.Generation, Reason);
	}};
	struct DiagnosticScope {
		detail::GnsServiceSink *Previous;
		~DiagnosticScope() { detail::ActiveGnsService = Previous; }
	} Scope{detail::ActiveGnsService};
	detail::ActiveGnsService = &Diagnostic;
	if (QualificationDiagnostic) {
		SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(SteamNetworkingUtils(), k_ESteamNetworkingSocketsDebugOutputType_Msg,
			[](ESteamNetworkingSocketsDebugOutputType Level, const char *Text) {
				std::fprintf(stderr, "[KI008:Gns] ns=%lld thread=%zu level=%d %s\n",
					static_cast<long long>(Clock::now().time_since_epoch().count()), std::hash<std::thread::id>{}(std::this_thread::get_id()), static_cast<int>(Level), Text);
			});
		std::fprintf(stderr, "[KI008:Config] peers=%u groups=%d bytes=%d gameplay=%d ns=%lld thread=%zu\n", QualificationPeerCount, PressureCount, PressureBytes, Gameplay,
			static_cast<long long>(DiagnosticStart.time_since_epoch().count()), std::hash<std::thread::id>{}(std::this_thread::get_id()));
	}
	struct Peer {
		std::shared_ptr<GameNetworkingSocketsTransport> Transport;
		std::unique_ptr<GameSession> Session;
		std::unique_ptr<HeadlessRenderer> Renderer;
		std::unique_ptr<Engine> Runtime;
		~Peer() {
			if (Session) Session->Stop();
			if (Runtime) Runtime->Destroy();
		}
	};
	std::vector<std::unique_ptr<Peer>> Peers;
	struct AggregateStepSpan {
		std::uint64_t Tick = 0;
		std::uint32_t SlowestPeer = 0;
		double GapMs = 0, BeforeStepMs = 0, WorkMs = 0;
		double PriorObserverMs = 0, PriorSleepRequestedMs = 0, PriorSleepActualMs = 0;
		double PrimaryRuntimeMs = 0, ServerRuntimeMs = 0, ServerPollMs = 0, PrimaryPollMs = 0;
		double ServerSessionMs = 0, PrimarySessionMs = 0;
		double PeerPollMs = 0, PeerRuntimeMs = 0, PeerSessionMs = 0, SlowestPeerMs = 0;
		test_detail::AggregateStepOperations Operations;
		std::optional<double> ThreadCpuMs, ProcessCpuMs, PriorSleepProcessCpuMs;
	};
	struct AggregateBounds {
		std::uint64_t MinimumMargin = DefaultChangeJournalCapacity, RequiredHigh = 0, OldestSequence = 0, PendingHigh = 0;
		ConnectionId OldestOwner;
		bool CatalogOwner = false;
		double OldestAgeMs = 0, ServiceGapMs = 0;
		// Retain bounded diagnostics without printing inside a measured step.
		std::array<AggregateStepSpan, 4> SlowSteps{};
		std::size_t SlowStepCount = 0;
	} Bounds;
	struct JournalTime { std::uint64_t Sequence = 0; Clock::time_point Observed; };
	std::vector<JournalTime> JournalTimes(DefaultChangeJournalCapacity);
	std::uint64_t LastJournalTail = 0;
	auto LastStep = Clock::now();
	auto LastSleepEnd = LastStep;
	double PriorObserverMs = 0, PriorSleepRequestedMs = 0, PriorSleepActualMs = 0;
	std::optional<double> PriorSleepProcessCpuMs;
	bool Disconnected = false;
	bool AggregatePhaseActive = false;
	std::uint32_t AggregatePhase = 0;
	test_detail::WorkloadSleepLedger AggregateSleeps;
	std::vector<std::unique_ptr<test_detail::AggregatePeerEvidence>> AggregatePeers;
	std::unique_ptr<test_detail::AggregateOperationEvidence> AggregateOperations;
	auto CheckSession = [&](const GameSession &Session) {
		if (Session.GetStatus() != GameSessionStatus::Failed) return;
		if (!Disconnected) {
			const auto Metrics = Server.GetMetrics();
			std::cerr << "[Qualification:Disconnect] " << Session.GetFailure()
				<< " protocol_rejects=" << Metrics.ProtocolRejects
				<< " backlog_failures=" << Metrics.StructuralBacklogLimitFailures
				<< " journal_failures=" << Metrics.StructuralJournalLagFailures << std::endl;
			Check(false, "aggregate sessions remain connected through overload and recovery");
		}
		Disconnected = true;
	};
	auto Step = [&]() {
		Attribution.Step = Tick;
		const auto Next = Clock::now() + std::chrono::microseconds(16'667);
		const auto Started = Clock::now();
		const auto StartedCpu = test_detail::CaptureWorkloadCpu();
		AggregateStepSpan Span;
		Span.Tick = Tick;
		Span.Operations.Tick = Tick;
		Span.Operations.StartedNs = test_detail::WorkloadTimestamp(Started);
		Span.Operations.GapStartedNs = test_detail::WorkloadTimestamp(LastStep);
		Span.Operations.Before = StartedCpu;
		Span.BeforeStepMs = std::chrono::duration<double, std::milli>(Started - LastSleepEnd).count();
		Span.PriorObserverMs = PriorObserverMs;
		Span.PriorSleepRequestedMs = PriorSleepRequestedMs;
		Span.PriorSleepActualMs = PriorSleepActualMs;
		Span.PriorSleepProcessCpuMs = PriorSleepProcessCpuMs;
		auto Time = [&](double &Duration, auto &&Action, std::size_t Peer, std::size_t Subphase) {
			const auto BeforeCpu = AggregatePhaseActive ? test_detail::CaptureWorkloadCpu() : test_detail::WorkloadCpuSample{};
			const auto Begin = Clock::now();
			Action();
			const auto End = Clock::now();
			Duration += std::chrono::duration<double, std::milli>(End - Begin).count();
			if (AggregatePhaseActive) Span.Operations.Spans[Peer][Subphase] = {
				test_detail::WorkloadTimestamp(Begin), test_detail::WorkloadTimestamp(End),
				test_detail::WorkloadNativeThread(), BeforeCpu, test_detail::CaptureWorkloadCpu(),
				Peer < 32 ? AggregatePeers[Peer]->Slot : 0, Peer < 32 ? AggregatePeers[Peer]->Generation : 0};
		};
		Time(Span.PrimaryRuntimeMs, [&] { PrimaryRuntime.Step(); }, 0, 1);
		Time(Span.ServerRuntimeMs, [&] { ServerRuntime.Step(); }, 32, 1);
		Time(Span.ServerPollMs, [&] { (void)Server.Poll(); }, 32, 0);
		Time(Span.PrimaryPollMs, [&] { (void)Primary.Poll(); }, 0, 0);
		Time(Span.ServerSessionMs, [&] { Server.Step(Tick); }, 32, 2);
		Time(Span.PrimarySessionMs, [&] { Primary.Step(Tick); }, 0, 2);
		for (std::size_t PeerIndex = 0; PeerIndex < Peers.size(); ++PeerIndex) {
			auto &Peer = Peers[PeerIndex];
			const auto PeerStarted = Clock::now();
			Time(Span.PeerPollMs, [&] { (void)Peer->Session->Poll(); }, PeerIndex + 1, 0);
			if (!Peer->Runtime && Peer->Session->GetClientDataModel()) {
				const auto RuntimeStarted = Clock::now();
				Peer->Renderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
				Peer->Runtime = std::make_unique<Engine>(Peer->Session->GetClientDataModel(), Peer->Renderer.get(),
					std::function<void(std::string, std::string)>{},
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
				Peer->Runtime->ProcessService->Alive = true;
				Check(Peer->Session->AttachClientRuntime(*Peer->Runtime), "aggregate client attaches trusted runtime");
				Span.PeerRuntimeMs += std::chrono::duration<double, std::milli>(Clock::now() - RuntimeStarted).count();
			}
			if (Peer->Runtime) Time(Span.PeerRuntimeMs, [&] { Peer->Runtime->Step(); }, PeerIndex + 1, 1);
			Time(Span.PeerSessionMs, [&] { Peer->Session->Step(Tick); }, PeerIndex + 1, 2);
			const auto PeerMs = std::chrono::duration<double, std::milli>(Clock::now() - PeerStarted).count();
			if (PeerMs > Span.SlowestPeerMs) {
				Span.SlowestPeerMs = PeerMs;
				Span.SlowestPeer = static_cast<std::uint32_t>(PeerIndex + 1);
			}
		}
		++Tick;
		const auto ObservedAt = Clock::now();
		const auto ObservedCpu = test_detail::CaptureWorkloadCpu();
		Span.Operations.EndedNs = test_detail::WorkloadTimestamp(ObservedAt);
		Span.Operations.After = ObservedCpu;
		Span.WorkMs = std::chrono::duration<double, std::milli>(ObservedAt - Started).count();
		Span.GapMs = std::chrono::duration<double, std::milli>(ObservedAt - LastStep).count();
		Span.ThreadCpuMs = test_detail::WorkloadTimingEvidence::CpuDelta(StartedCpu.Thread100ns, ObservedCpu.Thread100ns,
			StartedCpu.ThreadValid, ObservedCpu.ThreadValid);
		Span.ProcessCpuMs = test_detail::WorkloadTimingEvidence::CpuDelta(StartedCpu.Process100ns, ObservedCpu.Process100ns,
			StartedCpu.ProcessValid, ObservedCpu.ProcessValid);
		Bounds.ServiceGapMs = std::max(Bounds.ServiceGapMs, Span.GapMs);
		if (AggregatePhaseActive) AggregateOperations->Record(Span.Operations, Span.GapMs);
		for (std::size_t Index = 0; Index < Bounds.SlowSteps.size(); ++Index) {
			if (Bounds.SlowSteps[Index].GapMs >= Span.GapMs) continue;
			for (std::size_t Move = Bounds.SlowSteps.size() - 1; Move > Index; --Move)
				Bounds.SlowSteps[Move] = Bounds.SlowSteps[Move - 1];
			Bounds.SlowSteps[Index] = Span;
			Bounds.SlowStepCount = std::min(Bounds.SlowStepCount + 1, Bounds.SlowSteps.size());
			break;
		}
		LastStep = ObservedAt;
		const auto ObserverStarted = Clock::now();
		const auto Metrics = Server.GetMetrics();
		Bounds.PendingHigh = std::max(Bounds.PendingHigh, Metrics.StructuralPendingEnters + Metrics.StructuralPendingLeaves);
		const auto Tail = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId());
		for (auto Sequence = std::max(LastJournalTail, Tail.NextSequence > DefaultChangeJournalCapacity ? Tail.NextSequence - DefaultChangeJournalCapacity : 1);
			Sequence < Tail.NextSequence; ++Sequence) JournalTimes[Sequence % JournalTimes.size()] = {Sequence, ObservedAt};
		LastJournalTail = Tail.NextSequence;
		const auto Oldest = ChangeJournal::Get().Read({Tail.Scope, 0}, 0).Cursor.NextSequence;
		for (const auto &Requirement : detail::GameSessionTestAccess::GetJournalRequirements(Server)) {
			const auto Required = Tail.NextSequence - Requirement.Cursor.NextSequence;
			const auto Margin = Required >= DefaultChangeJournalCapacity ? 0 : DefaultChangeJournalCapacity - Required;
			if (Margin < Bounds.MinimumMargin) {
				Bounds.MinimumMargin = Margin; Bounds.OldestSequence = Requirement.Cursor.NextSequence;
				Bounds.OldestOwner = Requirement.Connection; Bounds.CatalogOwner = Requirement.Catalog;
			}
			Bounds.RequiredHigh = std::max(Bounds.RequiredHigh, Required);
			const auto &Time = JournalTimes[Requirement.Cursor.NextSequence % JournalTimes.size()];
			if (Required && Time.Sequence == Requirement.Cursor.NextSequence)
				Bounds.OldestAgeMs = std::max(Bounds.OldestAgeMs, std::chrono::duration<double, std::milli>(ObservedAt - Time.Observed).count());
			Check(Requirement.Cursor.NextSequence >= Oldest, "aggregate live journal requirement remains retained");
		}
		CheckSession(Primary);
		for (const auto &Peer : Peers) CheckSession(*Peer->Session);
		PriorObserverMs = std::chrono::duration<double, std::milli>(Clock::now() - ObserverStarted).count();
		const auto BeforeSleep = Clock::now();
		const auto BeforeSleepCpu = test_detail::CaptureWorkloadCpu();
		PriorSleepRequestedMs = std::chrono::duration<double, std::milli>(std::max(Next, BeforeSleep) - BeforeSleep).count();
		AggregateSleeps.Begin(test_detail::WorkloadTimestamp(BeforeSleep),
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(Next, BeforeSleep) - BeforeSleep).count()),
			test_detail::WorkloadNativeThread());
		std::this_thread::sleep_until(Next);
		LastSleepEnd = Clock::now();
		AggregateSleeps.End(test_detail::WorkloadTimestamp(LastSleepEnd), test_detail::WorkloadNativeThread());
		const auto AfterSleepCpu = test_detail::CaptureWorkloadCpu();
		PriorSleepActualMs = std::chrono::duration<double, std::milli>(LastSleepEnd - BeforeSleep).count();
		PriorSleepProcessCpuMs = test_detail::WorkloadTimingEvidence::CpuDelta(BeforeSleepCpu.Process100ns, AfterSleepCpu.Process100ns,
			BeforeSleepCpu.ProcessValid, AfterSleepCpu.ProcessValid);
	};
	for (std::uint32_t Index = 1; Index < QualificationPeerCount; ++Index) {
		auto Value = std::make_unique<Peer>();
		Value->Transport = std::make_shared<GameNetworkingSocketsTransport>();
		auto Config = Configuration(GameSessionRole::Client, Port, false);
		Config.ClientNonce += Index;
		Value->Session = std::make_unique<GameSession>(Value->Transport, Config);
		Check(Value->Session->Start().Succeeded(), "aggregate peer starts");
		Peers.push_back(std::move(Value));
		const auto Deadline = Clock::now() + 20s;
		while (Server.GetMetrics().ReadyPeers != Index + 1 && Clock::now() < Deadline) Step();
		Check(Server.GetMetrics().ReadyPeers == Index + 1, "aggregate peer becomes Ready");
		if (Failures) return;
	}
	std::vector<RemoteManager *> Managers;
	std::vector<ConnectionId> Connections;
	auto Add = [&](Engine &Runtime, GameSession &Session) {
		auto Replica = std::dynamic_pointer_cast<RemoteFunction>(Runtime.DataModel->FindFirstChild("QualificationFunction", true));
		Check(Replica && Session.GetPrimaryConnection(), "aggregate Remote replica and connection exist");
		if (!Replica || !Session.GetPrimaryConnection()) return;
		Managers.push_back(Replica->GetRemoteManager());
		Connections.push_back(*Session.GetPrimaryConnection());
	};
	Add(PrimaryRuntime, Primary);
	for (auto &Peer : Peers) Add(*Peer->Runtime, *Peer->Session);
	if (Managers.size() != QualificationPeerCount) return;
	AggregateOperations = std::make_unique<test_detail::AggregateOperationEvidence>();
	for (std::uint32_t Index = 0; Index < QualificationPeerCount; ++Index)
		AggregatePeers.push_back(std::make_unique<test_detail::AggregatePeerEvidence>());
	auto *Receiver = Function->GetRemoteManager();
	Receiver->SetRequestHandler(Function->GetNetworkObjectId(),
		[](const RemoteInvocation &Invocation, RemoteManager::RequestReply Reply) {
			Check(Reply(Invocation.Arguments, {}), "aggregate response accepted");
		});
	Receiver->SetEventHandler(Event->GetNetworkObjectId(), [&](const RemoteInvocation &Invocation) {
		Check(Receiver->SendEvent(Invocation.Peer.Connection, Invocation.Remote, Invocation.Arguments).Accepted(),
			"aggregate echo accepted");
	});
	struct Sample {
		std::vector<double> Latencies;
		std::uint64_t Accepted = 0, Completed = 0, Errors = 0;
		std::size_t Pending = 0, PendingHigh = 0;
	};
	std::vector<Sample> Samples(QualificationPeerCount);
	struct FrozenPhase {
		bool Overload = false, DeadlineReached = false;
		bool RecoveryInput = false, HealthyControl = false, ExtraRecoveryRpc = false;
		int Concurrent = 0;
		std::uint32_t Phase = 0;
		std::size_t Active = 0;
		std::uint64_t Tick = 0;
		double DurationSeconds = 0, DrainMs = 0;
		test_detail::WorkloadClockAnchor BeginAnchor, EndAnchor;
		GameSessionMetrics Before, After;
		AggregateBounds Bounds;
		test_detail::AggregateOperationEvidence Operations;
		std::array<test_detail::AggregatePeerEvidence, 32> Peers;
		std::vector<Sample> Samples;
		std::vector<double> ConvergenceMs;
		std::vector<std::uint64_t> StructuralReceived;
	};
	// Three fixed phases, existing bounded record arrays; heap data only.
	// No formatted strings or references to reset phase state survive.
	std::array<std::unique_ptr<FrozenPhase>, 3> Frozen;
	const auto ServerConnections = detail::GameSessionTestAccess::GetConnections(Server);
	Check(ServerConnections.size() == QualificationPeerCount, "aggregate binds every server peer generation");
	if (ServerConnections.size() != QualificationPeerCount) return;
	std::vector<Engine *> Runtimes{&PrimaryRuntime};
	std::vector<GameSession *> Sessions{&Primary};
	for (auto &Peer : Peers) { Runtimes.push_back(Peer->Runtime.get()); Sessions.push_back(Peer->Session.get()); }
	static constexpr std::array<std::string_view, 3> AggregateCases{"aggregate-baseline", "aggregate-overload", "aggregate-recovery"};
	const auto AggregateProfile = CandidateReliableService().IsPooled() ? "POOLED_SERVICE" : "FULL_RESERVATION";
	for (const bool Overload : {false, true, false}) {
		Bounds = {};
		AggregateOperations->Reset();
		for (std::uint32_t Index = 0; Index < QualificationPeerCount; ++Index)
			AggregatePeers[Index]->Configure(AggregatePhase, Index, Connections[Index].Slot, Connections[Index].Generation);
		AggregatePhaseActive = true;
		const auto AggregateBeginAnchor = test_detail::WorkloadClockAnchor::Capture();
		Attribution.CurrentCase = AggregateCases[AggregatePhase]; Attribution.Phase = AggregatePhase;
		Attribution.PhaseBeginNs = AggregateBeginAnchor.SteadyNs;
		Attribution.CurrentStage = PooledExecutionAttribution::Stage::Warmup;
		LastStep = Clock::now();
		LastSleepEnd = LastStep;
		PriorObserverMs = PriorSleepRequestedMs = PriorSleepActualMs = 0;
		PriorSleepProcessCpuMs.reset();
		const auto Before = Server.GetMetrics();
		for (auto &Sample : Samples) Sample = {};
		const bool HealthyControl = PooledRecoveryEvidence::Current && PooledRecoveryEvidence::HealthyControl();
		const std::size_t Active = Overload && !HealthyControl ? QualificationPeerCount : std::min(8u, QualificationPeerCount);
		const int Concurrent = Overload && !HealthyControl ? 16 : 4;
		std::vector<std::shared_ptr<Part>> Pressure;
		if (Overload && QualificationAggregateStructural) {
			for (int Index = 0; Index < PressureCount; ++Index) {
				auto Object = std::make_shared<Part>();
				Object->SetName("AggregatePressure" + std::to_string(Index));
				Object->SetAnchored(true);
				Object->SetParent(ServerRuntime.DataModel);
				Pressure.push_back(std::move(Object));
			}
			for (int Frame = 0; Frame < 120; ++Frame) Step();
		}
		std::vector<std::uint64_t> StructuralBefore;
		for (const auto *Session : Sessions) StructuralBefore.push_back(Session->GetMetrics().ClientStructuralBytesReceived);
		const auto Started = Clock::now();
		Attribution.CurrentStage = PooledExecutionAttribution::Stage::Demand;
		for (int Frame = 0; Frame < 480 && !Disconnected; ++Frame) {
			for (std::size_t Index = 0; Index < Pressure.size(); ++Index)
				Pressure[Index]->SetName("Aggregate" + std::to_string(Index) + "-" + std::to_string(Frame) + std::string(PressureBytes, 'a'));
			if (Gameplay && (Frame % 40 == 0 || (Overload && Frame == 479))) {
				for (std::size_t Index = 0; Index < Active; ++Index) {
					auto &Sample = Samples[Index];
					if (Sample.Pending != 0) continue;
					for (int Call = 0; Call < Concurrent; ++Call) {
						std::vector<WireValue> Arguments{static_cast<int>(Index), std::string(3072 - 62, static_cast<char>('a' + Call))};
						auto &Diagnostic = *AggregatePeers[Index];
						const auto DiagnosticId = ++Diagnostic.RequestSequence;
						const auto DiagnosticIdentity = Diagnostic.CaptureIdentity();
						test_detail::WorkloadEndpointCounters StartCounters;
						StartCounters.BeforeNs = test_detail::WorkloadTimestamp(Clock::now());
						StartCounters.BeforeCpu = test_detail::CaptureWorkloadCpu();
						const auto Submitted = Clock::now();
						StartCounters.AfterCpu = test_detail::CaptureWorkloadCpu();
						StartCounters.AfterNs = test_detail::WorkloadTimestamp(Clock::now());
						const auto SubmitThread = test_detail::WorkloadNativeThread();
						StartCounters.Sleep = AggregateSleeps.Snapshot(test_detail::WorkloadTimestamp(Submitted), SubmitThread);
						++Sample.Pending;
						Diagnostic.Remote.Begin(DiagnosticId, true, false, Tick, test_detail::WorkloadTimestamp(Submitted), SubmitThread, StartCounters);
						const auto Sent = Managers[Index]->StartRequest(Connections[Index], Function->GetNetworkObjectId(), Arguments,
							[&, Index, Arguments, Submitted, DiagnosticId, DiagnosticIdentity](RemoteRequestResult Result) {
								auto &Completed = Samples[Index];
								--Completed.Pending;
								++Completed.Completed;
								if (Result.Outcome.Status != RemoteRequestTerminalStatus::Success || Result.Results != Arguments) ++Completed.Errors;
								test_detail::WorkloadEndpointCounters EndCounters;
								EndCounters.BeforeNs = test_detail::WorkloadTimestamp(Clock::now());
								EndCounters.BeforeCpu = test_detail::CaptureWorkloadCpu();
								const auto CompletedAt = Clock::now();
								EndCounters.AfterCpu = test_detail::CaptureWorkloadCpu();
								EndCounters.AfterNs = test_detail::WorkloadTimestamp(Clock::now());
								const auto EndThread = test_detail::WorkloadNativeThread();
								EndCounters.Sleep = AggregateSleeps.Snapshot(test_detail::WorkloadTimestamp(CompletedAt), EndThread);
								Completed.Latencies.push_back(std::chrono::duration<double, std::milli>(CompletedAt - Submitted).count());
								AggregatePeers[Index]->End(DiagnosticIdentity, DiagnosticId, Tick, test_detail::WorkloadTimestamp(CompletedAt), EndThread,
									static_cast<int>(Result.Outcome.Status), Result.Results == Arguments, EndCounters);
							});
						Diagnostic.Remote.Submitted(DiagnosticId, Sent.Accepted(), Tick, test_detail::WorkloadTimestamp(Clock::now()),
							test_detail::WorkloadNativeThread());
						if (Sent.Accepted()) ++Sample.Accepted;
						else { --Sample.Pending; ++Sample.Errors; }
						Sample.PendingHigh = std::max(Sample.PendingHigh, Sample.Pending);
					}
				}
			}
			Step();
		}
		const auto Ended = Clock::now();
		Attribution.CurrentStage = PooledExecutionAttribution::Stage::Drain;
		bool RecoveryInput = false;
		if (Overload && PooledRecoveryEvidence::Current) {
			auto *Diagnostic = PooledRecoveryEvidence::Current;
			std::uint64_t Carryover = 0;
			for (const auto &Sample : Samples) Carryover += Sample.Pending;
			Diagnostic->Begin(Server.GetMetrics().ReadyPeers, Carryover);
			// Diagnostic-only moderate addition: 8 x 4 x 3072 B, once in 500 ms.
			// Prior overload requests remain real and are explicitly reported above.
			std::array<bool, 8> Submitted{};
			while (!Disconnected && Clock::now() < Ended + std::chrono::milliseconds(500)) {
				for (std::size_t Index = 0; !HealthyControl && Index < std::min<std::size_t>(8, Managers.size()); ++Index) {
					if (Submitted[Index] || Samples[Index].Pending != 0) continue;
					Submitted[Index] = true;
					for (int Call = 0; Call < 4; ++Call) {
						std::vector<WireValue> Arguments{static_cast<int>(Index), std::string(3072 - 62, 'r')};
						const auto Sent = Managers[Index]->StartRequest(Connections[Index], Function->GetNetworkObjectId(), Arguments,
							[Diagnostic, Arguments](RemoteRequestResult Result) {
								Diagnostic->Resolved(Result.Outcome.Status == RemoteRequestTerminalStatus::Success && Result.Results == Arguments);
							});
						Diagnostic->Submitted(Sent.Accepted());
					}
				}
				Step();
			}
			Diagnostic->End();
			RecoveryInput = true;
		}
		const auto Deadline = Ended + 20s;
		std::vector<double> ConvergenceMs(QualificationPeerCount, -1);
		struct ClientCut {
			std::vector<std::shared_ptr<Instance>> Objects;
			std::vector<detail::ClientReplicaIdentity> Identities;
			bool Valid = false;
		};
		std::vector<ClientCut> Cuts(QualificationPeerCount);
		std::vector<ObjectId> FinalSources;
		std::vector<std::string> FinalNames;
		for (const auto &Object : Pressure) { FinalSources.push_back(Object->GetObjectId()); FinalNames.push_back(Object->GetName()); }
		auto CurrentCut = [&](std::size_t Index) {
			const auto &Cut = Cuts[Index];
			if (!Cut.Valid || Cut.Objects.size() != Pressure.size()) return false;
			for (std::size_t Object = 0; Object < Pressure.size(); ++Object)
				if (Pressure[Object]->GetDestroyed() || Pressure[Object]->IsDestroying() ||
					Pressure[Object]->GetObjectId() != FinalSources[Object] || Pressure[Object]->GetName() != FinalNames[Object] ||
					Cut.Objects[Object]->GetName() != FinalNames[Object]) return false;
			return Pressure.empty() || detail::GameSessionTestAccess::VerifyClientReplicaIdentities(
				*Sessions[Index], Cut.Objects, Cut.Identities);
		};
		auto DiscoverCut = [&](std::size_t Index) {
			auto &Cut = Cuts[Index];
			if (CurrentCut(Index)) return;
			Cut = {}; ConvergenceMs[Index] = -1;
			for (const auto &Name : FinalNames) {
				auto Object = Runtimes[Index]->DataModel->FindFirstChild(Name, true);
				if (!Object) return;
				Cut.Objects.push_back(std::move(Object));
			}
			if (!Pressure.empty()) {
				const auto Identities = detail::GameSessionTestAccess::ReadClientReplicaIdentities(*Sessions[Index], Cut.Objects);
				if (!Identities || Identities->size() != Pressure.size()) return;
				for (std::size_t Object = 0; Object < Pressure.size(); ++Object)
					if ((*Identities)[Object].Source != FinalSources[Object]) return;
				Cut.Identities = *Identities;
			}
			Cut.Valid = true;
			if (CurrentCut(Index)) ConvergenceMs[Index] = std::chrono::duration<double, std::milli>(Clock::now() - Ended).count();
			else Cut.Valid = false;
		};
		auto Pending = [&]() { return std::any_of(Samples.begin(), Samples.end(), [](const Sample &Value) { return Value.Pending != 0; }); };
		auto Quiescent = [&]() {
			if (!CandidateReliableService().IsPooled()) return true;
			const auto Snapshot = detail::GameSessionTestAccess::ReadPooledPostpass(Server);
			return Snapshot && AggregatePooledQuiescent(*Snapshot, ServerConnections);
		};
		bool Converged = false, Drained = false;
		std::size_t NextClient = 0;
		while (!Disconnected && Clock::now() < Deadline) {
			// One hierarchy/map discovery per serviced opportunity. The final cut
			// checks forward bindings, without another whole-map traversal.
			DiscoverCut(NextClient); NextClient = (NextClient + 1) % QualificationPeerCount;
			Converged = std::all_of(Cuts.begin(), Cuts.end(), [](const auto &Cut) { return Cut.Valid; });
			if (Converged)
				for (std::size_t Index = 0; Index < Cuts.size(); ++Index) if (!CurrentCut(Index)) {
					Cuts[Index].Valid = false; ConvergenceMs[Index] = -1; Converged = false;
				}
			Drained = !Pending() && Converged && Server.GetMetrics().JournalBacklogRecords == 0 && Quiescent() && Clock::now() < Deadline;
			if (Drained) break;
			Step();
		}
		Check(Drained && Converged, "aggregate accepted work ACK-retires with no timed ownership and all exact replicas converge");
		const auto AggregateEndAnchor = test_detail::WorkloadClockAnchor::Capture();
		const bool AggregateDeadlineReached = Clock::now() >= Deadline;
		AggregatePhaseActive = false;
		const auto After = Server.GetMetrics();
		const auto &Admission = After.ReliableAdmission;
		const auto AggregateDrainMs = std::chrono::duration<double, std::milli>(Clock::now() - Ended).count();
		Check(Admission.PeerCreditHighWater <= CandidateReliableService().PeerCreditCap() && Admission.GlobalCreditHighWater <= CandidateReliableService().GlobalCreditCap(),
			"aggregate byte credit stays within unchanged caps");
		Check(After.PlanningMaximumTickWork <= 65'536 && After.StructuralMaximumTransitionsSelectedPerTick <= 8'192,
			"aggregate planning and selection bounds remain unchanged");
		Check(After.JournalBacklogRecords == 0, "aggregate raw journal readers recover");
		for (std::size_t Index = 0; Index < Active; ++Index) {
			const auto &Sample = Samples[Index];
			Check(Sample.Errors == 0 && Sample.Completed == Sample.Accepted && (!Gameplay || Sample.Completed > 0),
				"every eligible aggregate peer completes accepted work without corruption");
			if (!Overload && !Sample.Latencies.empty()) {
				// Exact order statistics without sorting or formatting the live evidence.
				// At most 208 bounded requests per peer; the original thresholds hold.
				auto Latencies = Sample.Latencies;
				auto Percentile = [&](double Fraction) {
					const auto Rank = static_cast<std::size_t>(std::ceil(Fraction * Latencies.size())) - 1;
					std::nth_element(Latencies.begin(), Latencies.begin() + Rank, Latencies.end());
					return Latencies[Rank];
				};
				Performance(Percentile(.95) <= 150 && Percentile(.99) <= 250 && Percentile(1) <= 500,
					"aggregate qualified RPC meets unchanged gates");
			}
		}
		auto &Saved = *(Frozen[AggregatePhase] = std::make_unique<FrozenPhase>());
		Saved.Overload = Overload; Saved.Phase = AggregatePhase; Saved.Active = Active; Saved.Tick = Tick;
		Saved.RecoveryInput = RecoveryInput; Saved.HealthyControl = HealthyControl;
		Saved.Concurrent = Concurrent; Saved.ExtraRecoveryRpc = !HealthyControl;
		Saved.DurationSeconds = std::chrono::duration<double>(Ended - Started).count(); Saved.DrainMs = AggregateDrainMs;
		Saved.BeginAnchor = AggregateBeginAnchor; Saved.EndAnchor = AggregateEndAnchor; Saved.DeadlineReached = AggregateDeadlineReached;
		Saved.Before = Before; Saved.After = After; Saved.Bounds = Bounds; Saved.Operations = *AggregateOperations;
		Saved.Samples = Samples; Saved.ConvergenceMs = ConvergenceMs;
		for (std::size_t Index = 0; Index < AggregatePeers.size(); ++Index) {
			Saved.Peers[Index] = *AggregatePeers[Index];
			Saved.StructuralReceived.push_back(Sessions[Index]->GetMetrics().ClientStructuralBytesReceived - StructuralBefore[Index]);
		}
		// Closing gameplay callbacks is proved before a successful phase reset.
		// A failed phase keeps the original captures alive through Session::Stop.
		Check(!Pending(), "aggregate phase cannot reset live RPC callback captures");
		Attribution.CurrentStage = PooledExecutionAttribution::Stage::CaseCleanup;
		for (auto &Object : Pressure) { Object->Destroy(); Step(); }
		for (int Frame = 0; Frame < 120; ++Frame) Step();
		Check(!Pending() && Server.GetMetrics().JournalBacklogRecords == 0 && Quiescent(),
			"aggregate removal work ACK-retires and returns actual ownership before phase reset or Stop");
		if (Failures) break;
		++AggregatePhase;
	}
	Receiver->SetRequestHandler(Function->GetNetworkObjectId(), {});
	Receiver->SetEventHandler(Event->GetNetworkObjectId(), {});
	Attribution.CurrentStage = PooledExecutionAttribution::Stage::FinalStop;
	// Stop while callback captures still exist, including on a failed drain.
	Primary.Stop();
	for (auto &Peer : Peers) Peer->Session->Stop();
	const auto CleanupDeadline = Clock::now() + 5s;
	while (!detail::GameSessionTestAccess::GetConnections(Server).empty() && Clock::now() < CleanupDeadline) {
		Attribution.Step = Tick;
		(void)Server.Poll(); Server.Step(Tick++); std::this_thread::sleep_for(1ms);
	}
	const auto Requirements = detail::GameSessionTestAccess::GetJournalRequirements(Server);
	const auto PeerRequirements = std::count_if(Requirements.begin(), Requirements.end(), [](const auto &Value) { return !Value.Catalog; });
	const auto ConnectionsRemaining = detail::GameSessionTestAccess::GetConnections(Server).size();
	Check(PeerRequirements == 0 && !ConnectionsRemaining, "aggregate disconnect releases every peer journal owner");
	Server.Stop();
	Check(detail::GameSessionTestAccess::GetConnections(Server).empty() &&
		Server.GetMetrics().ReliableAdmissionPeerStates == 0, "aggregate Stop closes actual admission and session ownership");
	// All live session ownership has closed. Formatting, sorting and output are
	// cold work; the saved END anchors and phase values remain unchanged.
	Attribution.CurrentStage = PooledExecutionAttribution::Stage::Report;
	std::cout << "[Qualification:AggregateCleanup] peer_requirements=" << PeerRequirements
		<< " connections=" << ConnectionsRemaining << '\n';
	std::cout << "[Qualification:AggregateReportStorage] phase_capacity=" << Frozen.size()
		<< " static_phase_bytes=" << sizeof(FrozenPhase) << " heap_phase_bytes_bound=" << Frozen.size() * sizeof(FrozenPhase)
		<< " remote_records_per_peer=" << test_detail::AggregatePeerEvidence::RemoteEvidence::Capacity
		<< " operation_snapshots_per_phase=" << test_detail::AggregateOperationEvidence::SnapshotCapacity
		<< " report_after_stop=1\n";
	for (auto &Phase : Frozen) {
		if (!Phase) continue;
		auto &Saved = *Phase;
		const auto Overload = Saved.Overload; const auto AggregatePhase = Saved.Phase; const auto Tick = Saved.Tick;
		const auto Active = Saved.Active; auto &Samples = Saved.Samples; const auto &ConvergenceMs = Saved.ConvergenceMs;
		const auto &Bounds = Saved.Bounds; const auto &Before = Saved.Before; const auto &After = Saved.After;
		const auto &Admission = After.ReliableAdmission; const auto AggregateDrainMs = Saved.DrainMs;
		const auto &AggregateBeginAnchor = Saved.BeginAnchor; const auto &AggregateEndAnchor = Saved.EndAnchor;
		const auto AggregateDeadlineReached = Saved.DeadlineReached;
		std::array<const test_detail::AggregatePeerEvidence *, 32> AggregatePeers;
		for (std::size_t Index = 0; Index < AggregatePeers.size(); ++Index) AggregatePeers[Index] = &Saved.Peers[Index];
		const auto AggregateCase = AggregateCases[AggregatePhase];
		const auto AggregateContext = " phase_index=" + std::to_string(AggregatePhase);
		if (Saved.RecoveryInput)
			std::cout << "[Qualification:PooledRecoveryInput] ordinary_profile_control=" << Saved.HealthyControl
				<< " active=" << Saved.Active << " concurrent=" << Saved.Concurrent << " extra_recovery_rpc=" << Saved.ExtraRecoveryRpc << '\n';
		AggregateBeginAnchor.Print(std::cout, AggregateCase, AggregateProfile, "BEGIN", AggregateContext);
		AggregateEndAnchor.Print(std::cout, AggregateCase, AggregateProfile, "END", AggregateContext);
		Saved.Operations.Print(std::cout, AggregateCase, AggregateProfile, AggregatePhase);
		for (const auto &Diagnostic : AggregatePeers) {
			std::cout << "[Qualification:AggregatePeerEvidence] case=" << AggregateCase << " profile=" << AggregateProfile
				<< Diagnostic->Context() << " configured=" << Diagnostic->Configured << " invalid=" << Diagnostic->Invalid << '\n';
			Diagnostic->Remote.Print(std::cout, AggregateCase, AggregateProfile, Tick, AggregateEndAnchor.SteadyNs,
				AggregateDeadlineReached, Diagnostic->Context());
		}
		std::cout << "[Qualification:AggregateBounds] overload=" << Overload << " duration_s=" << Saved.DurationSeconds
			<< " drain_ms=" << AggregateDrainMs
			<< " structural_reserved=" << Admission.ReservedBytes - Before.ReliableAdmission.ReservedBytes
			<< " structural_accepted=" << Admission.AcceptedBytes - Before.ReliableAdmission.AcceptedBytes
			<< " credit_deferrals=" << Admission.CreditDeferrals - Before.ReliableAdmission.CreditDeferrals
			<< " backlog_deferrals=" << Admission.BacklogDeferrals - Before.ReliableAdmission.BacklogDeferrals
			<< " peer_credit_high=" << Admission.PeerCreditHighWater << " global_credit_high=" << Admission.GlobalCreditHighWater
			<< " peer_backlog_high=" << Admission.PeerBacklogHighWater << " global_backlog_high=" << Admission.GlobalBacklogHighWater
			<< " admission_wait_us=" << Admission.MaximumAdmissionWaitMicroseconds << " service_gap_ms=" << Bounds.ServiceGapMs
			<< " pending_high=" << Bounds.PendingHigh << " journal_required_high=" << Bounds.RequiredHigh
			<< " journal_minimum_margin=" << Bounds.MinimumMargin << " journal_oldest_sequence=" << Bounds.OldestSequence
			<< " journal_owner=" << (!Bounds.RequiredHigh ? "none" : Bounds.CatalogOwner ? "catalog" : "peer")
			<< " journal_owner_slot=" << Bounds.OldestOwner.Slot << " journal_owner_generation=" << Bounds.OldestOwner.Generation
			<< " journal_oldest_age_ms=" << Bounds.OldestAgeMs << " journal_recovery_backlog=" << After.JournalBacklogRecords << '\n';
		for (std::size_t Index = 0; Index < Bounds.SlowStepCount; ++Index) {
			const auto &Span = Bounds.SlowSteps[Index];
			std::cout << "[Qualification:AggregateStep] overload=" << Overload << " rank=" << Index << " tick=" << Span.Tick
				<< " gap_ms=" << Span.GapMs << " before_step_ms=" << Span.BeforeStepMs << " work_ms=" << Span.WorkMs
				<< " prior_observer_ms=" << Span.PriorObserverMs
				<< " prior_sleep_requested_ms=" << Span.PriorSleepRequestedMs << " prior_sleep_actual_ms=" << Span.PriorSleepActualMs
				<< " primary_runtime_ms=" << Span.PrimaryRuntimeMs << " server_runtime_ms=" << Span.ServerRuntimeMs
				<< " server_poll_ms=" << Span.ServerPollMs << " primary_poll_ms=" << Span.PrimaryPollMs
				<< " server_session_ms=" << Span.ServerSessionMs << " primary_session_ms=" << Span.PrimarySessionMs
				<< " peer_poll_ms=" << Span.PeerPollMs << " peer_runtime_ms=" << Span.PeerRuntimeMs
				<< " peer_session_ms=" << Span.PeerSessionMs << " slowest_peer=" << Span.SlowestPeer
				<< " slowest_peer_ms=" << Span.SlowestPeerMs << " thread_cpu_ms=";
			if (Span.ThreadCpuMs) std::cout << *Span.ThreadCpuMs; else std::cout << "NOT_MEASURED";
			std::cout << " process_cpu_ms=";
			if (Span.ProcessCpuMs) std::cout << *Span.ProcessCpuMs; else std::cout << "NOT_MEASURED";
			std::cout << " prior_sleep_process_cpu_ms=";
			if (Span.PriorSleepProcessCpuMs) std::cout << *Span.PriorSleepProcessCpuMs; else std::cout << "NOT_MEASURED";
			std::cout << '\n';
		}
		for (std::size_t Index = 0; Index < Active; ++Index) {
			auto &Sample = Samples[Index];
			std::sort(Sample.Latencies.begin(), Sample.Latencies.end());
			auto Percentile = [&](double Fraction) {
				return Sample.Latencies.empty() ? 0.0 : Sample.Latencies[static_cast<std::size_t>(std::ceil(Fraction * Sample.Latencies.size())) - 1];
			};
			std::cout << "[Qualification:Aggregate] overload=" << Overload << " connected=" << QualificationPeerCount
				<< " active=" << Active << " peer=" << Index << " accepted=" << Sample.Accepted << " completed=" << Sample.Completed
				<< " pending_high=" << Sample.PendingHigh << " errors=" << Sample.Errors << " p95_ms=" << Percentile(.95)
				<< " p99_ms=" << Percentile(.99) << " max_ms=" << Percentile(1)
				<< " structural_received_bytes=" << Saved.StructuralReceived[Index]
				<< " convergence_ms=" << ConvergenceMs[Index]
				<< " admitted_Bps=" << Sample.Accepted * 3104 / Saved.DurationSeconds << '\n';
		}
	}
	if (QualificationDiagnostic) SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(SteamNetworkingUtils(), k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
}

bool TestPooledExecutionCapture() {
	{
		std::array<ConnectionId, 32> Expected;
		detail::PooledPostpassSnapshot Complete;
		for (std::uint32_t Index = 0; Index < Expected.size(); ++Index) {
			Expected[Index] = {Index + 1, 1};
			detail::PooledPostpassPeer Peer;
			Peer.Connection = Expected[Index]; Peer.Ready = Peer.FeedbackComplete = Peer.Healthy = true;
			Peer.Execution.Captured = true;
			Peer.Accepted = Peer.ReconciledAccepted = {77, 77, 0};
			Peer.FirstSent = Peer.Acked = Peer.Retired = Peer.ReliableAcked = 77;
			Complete.Peers.push_back(Peer);
		}
		Complete.Admission.AcceptedBytes = Complete.Admission.VerifiedAttributedRetirement = 32 * 77;
		Check(AggregatePooledQuiescent(Complete, Expected), "exact 32-generation postpass ACK retirement permits the immutable phase cut");
		auto Reject = [&](auto &&Change, const char *Reason) {
			auto Incomplete = Complete; Change(Incomplete);
			Check(!AggregatePooledQuiescent(Incomplete, Expected), Reason);
		};
		Reject([](auto &S) { S.Peers.clear(); }, "empty peer evidence cannot prove live ownership closure");
		Reject([](auto &S) { S.Peers.pop_back(); }, "missing peer generation cannot prove quiescence");
		Reject([](auto &S) { ++S.Peers[0].Connection.Generation; }, "stale peer generation cannot bind a phase cut");
		Reject([](auto &S) { S.Peers[1] = S.Peers[0]; }, "duplicate peer evidence cannot replace another owner");
		Reject([](auto &S) { --S.Peers[0].FirstSent; }, "withheld native first-send bytes cannot look delivered");
		Reject([](auto &S) { --S.Peers[0].Acked; }, "positive native send alone cannot replace exact ACK");
		Reject([](auto &S) { --S.Peers[0].Retired; }, "ACK alone cannot replace production retirement");
		Reject([](auto &S) { ++S.Peers[0].Accepted.All; }, "unreconciled acceptance cannot borrow older native counters");
		Reject([](auto &S) { S.Peers[0].FeedbackComplete = false; }, "incomplete feedback cannot establish quiescence");
		Reject([](auto &S) { S.Peers[0].Healthy = false; }, "failed product health cannot establish quiescence");
		Reject([](auto &S) { S.Peers[0].NativePending = 1; }, "native pending work prevents a cold report cut");
		Reject([](auto &S) { S.Peers[0].NativeUnacked = 1; }, "native unacked work prevents a cold report cut");
		Reject([](auto &S) { S.Peers[0].OrdinaryDebt = 1; }, "ordinary debt prevents a cold report cut");
		Reject([](auto &S) { S.Peers[0].Debt = S.Admission.OutstandingBytes = 77; }, "structural debt cannot be hidden by callback completion");
		Reject([](auto &S) { S.Admission.ActiveDrainGrants = 1; }, "zero-byte retained permit still owns a slot");
		Reject([](auto &S) { S.Admission.TerminalReleasedBytes = 77; S.Admission.VerifiedAttributedRetirement -= 77; },
			"exact terminal conservation is not successful delivery");
		detail::PooledReliableServiceFeedback Ledger;
		Check(Ledger.BeginAdmissionOpportunity(1000, true), "actual ledger arms live work");
		Complete.Peers[0].Execution = detail::CapturePooledExecution(Ledger);
		Check(!AggregatePooledQuiescent(Complete, Expected), "armed Main work remains live even without byte debt");
		Check(Ledger.CompleteAdmissionOpportunity(2000, false), "actual admission pass clears no-work Main ownership");
		Ledger.PendingTurnover = detail::PooledReliableServiceFeedback::Turnover{1, 1000, true};
		Complete.Peers[0].Execution = detail::CapturePooledExecution(Ledger);
		Check(!AggregatePooledQuiescent(Complete, Expected), "ACK and an available permit do not erase pre-clear turnover metadata");
		Check(Ledger.CompleteAdmissionOpportunity(3000, false), "actual completed pass consumes returned turnover metadata");
		Complete.Peers[0].Execution = detail::CapturePooledExecution(Ledger);
		Check(AggregatePooledQuiescent(Complete, Expected), "only the postpass state certifies returned ownership");
		Check(!AggregatePooledQuiescent({}, {}), "empty expected cohort cannot certify quiescence");
	}
	{
		auto Live = std::make_unique<test_detail::AggregatePeerEvidence>();
		Live->Configure(0, 0, 1, 1);
		Live->Remote.Begin(1, true, false, 10, 1000, 1);
		Live->Remote.Submitted(1, true, 10, 1001, 1);
		Live->End(Live->CaptureIdentity(), 1, 11, 2000, 1, 0, true);
		const auto Frozen = std::make_unique<test_detail::AggregatePeerEvidence>(*Live);
		Live->Configure(1, 0, 1, 1);
		Check(Frozen->Phase == 0 && Frozen->Remote.Count == 1 && Frozen->Remote.Records[0].Terminal &&
			Frozen->Remote.Records[0].EndNs == 2000 && Live->Phase == 1 && Live->Remote.Count == 0,
			"heap phase evidence remains immutable after the next phase resets its live callback records");
		Live->Remote.Begin(1, true, false, 20, 3000, 1);
		Live->Remote.Submitted(1, true, 20, 3001, 1);
		const auto FailedCut = std::make_unique<test_detail::AggregatePeerEvidence>(*Live);
		Live->End(Live->CaptureIdentity(), 1, 21, 4000, 1, 1, false);
		Check(!FailedCut->Remote.Records[0].Terminal && Live->Remote.Records[0].Terminal,
			"Stop-time terminal callbacks cannot retrospectively complete frozen failed phase evidence");
	}
	{
		PooledPollAttribution Poll;
		PooledPollAttribution::Record Value{}; Value.Detail = 104;
		Value.ClockValid = true; Value.BeginQpc = 100; Value.EndQpc = 200;
		Poll.Begin(3, 100);
		for (unsigned Index = 0; Index < 140; ++Index) { Value.Count = Index; Poll.Sink.Record(&Poll, Value); }
		Poll.Freeze(); Poll.Sink.Record(&Poll, Value); Poll.End(200);
		Poll.Begin(4, 300); Poll.End(400);
		Check(Poll.Step == 3 && Poll.BeginNs == 100 && Poll.EndNs == 200 && Poll.Count == 141 && !Poll.Active,
			"first execution fault retains the preceding poll and nested unwind; cleanup cannot overwrite it");
		Poll.Reset();
		Check(!Poll.Longest[3].ClockValid && !Poll.Count && !Poll.BeginNs && !Poll.EndNs,
			"case reset cannot attribute an early demand fault to a warmup poll scope");
		using namespace SteamNetworkingSocketsLib;
		auto *PreviousPoll = GargantuanPollCallsiteObserver;
		{
			GargantuanPollCallsiteScope Outer(&Poll.Sink);
			{ GargantuanPollCallsiteScope Inner(nullptr); Check(!GargantuanPollCallsiteObserver, "nested poll observer detaches locally"); }
			Check(GargantuanPollCallsiteObserver == &Poll.Sink, "nested poll observer restores borrowed scope");
		}
		Check(GargantuanPollCallsiteObserver == PreviousPoll, "poll observer restores on scope exit");
	}
	auto *Original = detail::ActivePooledService;
	{ PooledExecutionAttribution Off(false, false); Check(detail::ActivePooledService == Original && !Off.Timing && !Off.First.Clock.NativeValid,
		"disabled execution capture owns no sink, timing storage or clock"); }
	unsigned Records = 0, Grants = 0, Lifecycles = 0;
	struct Forwarded { unsigned *Records, *Grants, *Lifecycles; } Forward{&Records, &Grants, &Lifecycles};
	detail::PooledServiceSink Previous{&Forward,
		[](void *C, const detail::PooledServiceRecord &) noexcept { ++*static_cast<Forwarded *>(C)->Records; },
		[](void *C, ConnectionId, std::uint64_t, std::uint64_t, std::uint64_t) noexcept { ++*static_cast<Forwarded *>(C)->Grants; },
		[](void *C, const detail::PooledLifecycleRecord &) noexcept { ++*static_cast<Forwarded *>(C)->Lifecycles; }};
	detail::ActivePooledService = &Previous;
	{
		PooledExecutionAttribution Capture(true, false);
		Capture.Sink.Lifecycle(&Capture, {detail::PooledLifecyclePoint::FailPeer, {1, 1}, 100, 0, 0, 1});
		detail::PooledReliableServiceFeedback Ledger;
		Check(Ledger.BeginAdmissionOpportunity(1000, true), "synthetic opportunity arms");
		const auto Before = detail::CapturePooledExecution(Ledger);
		Check(!Ledger.CompleteAdmissionOpportunity(101001, false), "synthetic exact Main violation");
		detail::PooledServiceRecord R;
		R.Connection = {1, 1}; R.NowMicroseconds = 101001; R.Point = detail::PooledObservationPoint::AdmissionComplete;
		R.ExecutionBefore = Before; R.ExecutionRejected = true;
		R.Result = Ledger.Observe(R.Connection, {}, {}, 101001, 0, 0);
		Capture.CurrentCase = "synthetic-main"; Capture.CurrentStage = PooledExecutionAttribution::Stage::Demand;
		Capture.Sink.Record(&Capture, R);
		Capture.CurrentCase = "cleanup"; Capture.CurrentStage = PooledExecutionAttribution::Stage::FinalStop;
		R.NowMicroseconds = 300000; Capture.Sink.Record(&Capture, R);
		Capture.Sink.AcceptedGrant(&Capture, R.Connection, 1, 77, 1000);
		Capture.Sink.Lifecycle(&Capture, {detail::PooledLifecyclePoint::FailPeer, R.Connection, 101002, 0, 0, 1});
		Check(Capture.First.Now == 101001 && Capture.First.Case == "synthetic-main" &&
			Capture.First.Before.RequiredSince == 1000 && Capture.First.Before.Armed &&
			Capture.First.Result.MainOpportunityFailed && !Capture.First.Result.TurnoverFailed &&
			Ledger.OpportunityRequiredSinceMicroseconds == 101001 &&
			Capture.FirstLifecycle.NowMicroseconds < Capture.First.Now && Capture.PostFaultLifecycleObserved &&
			Capture.PostFaultLifecycle.NowMicroseconds > Capture.First.Now,
			"earlier setup lifecycle cannot mask the first fault and its later teardown");
	}
	{
		PooledExecutionAttribution Capture(true, false);
		detail::PooledServiceRecord R; R.NowMicroseconds = 77; R.ExecutionRejected = true; R.InvalidAfter = true;
		R.ExecutionBefore.Invalid = R.ExecutionBefore.Captured = true;
		R.Point = detail::PooledObservationPoint::AdmissionBegin;
		Capture.Sink.Record(&Capture, R);
		Check(Capture.First.Observed && Capture.First.InvalidAfter && !Capture.First.Result.ExecutionFailed,
			"invalid Begin rejection is captured independently of execution bits");
	}
	{
		PooledExecutionAttribution Capture(true, false);
		detail::PooledReliableServiceFeedback Ledger;
		Check(Ledger.PublishOffer(1, 77, 1000) && Ledger.CompleteAdmissionOpportunity(50000, false) &&
			Ledger.CompleteAdmissionOpportunity(100000, false), "synthetic turnover retains frequent Main opportunities");
		detail::PooledServiceRecord R; R.Connection = {1, 1}; R.NowMicroseconds = 151001;
		R.ExecutionBefore = detail::CapturePooledExecution(Ledger);
		R.ExecutionRejected = !Ledger.BeginAdmissionOpportunity(R.NowMicroseconds, true);
		R.Point = detail::PooledObservationPoint::AdmissionBegin;
		R.Result = Ledger.Observe(R.Connection, {77, 77, 0}, {}, R.NowMicroseconds, 1, 77);
		Capture.Sink.Record(&Capture, R);
		Check(Capture.First.Result.TurnoverFailed && !Capture.First.Result.MainOpportunityFailed &&
			Capture.First.Before.OfferToken == 1 && Capture.First.Before.OfferBytes == 77 && Capture.First.Before.OfferAt == 1000,
			"first turnover failure retains exact activation without becoming a Main failure");
		Ledger.PendingTurnover = detail::PooledReliableServiceFeedback::Turnover{2, 1001, true};
		const auto Snapshot = detail::CapturePooledExecution(Ledger);
		Check(Snapshot.TurnoverToken == 2 && Snapshot.TurnoverAt == 1001 && Snapshot.PermitAvailable,
			"pending permit metadata is copied independently without changing ownership");
	}
	{
		PooledExecutionAttribution Capture(true, false);
		detail::PooledReliableServiceFeedback Ledger;
		Ledger.HasOpportunityBoundary = true; Ledger.LastOpportunityBoundaryMicroseconds = 1000;
		detail::PooledServiceRecord R; R.NowMicroseconds = 999; R.ExecutionBefore = detail::CapturePooledExecution(Ledger);
		R.ExecutionRejected = !Ledger.BeginAdmissionOpportunity(999, true); R.InvalidAfter = Ledger.Invalid;
		R.Result = Ledger.Observe({1, 1}, {}, {}, 999, 0, 0);
		Capture.Sink.Record(&Capture, R);
		Check(Capture.First.InvalidAfter && !Capture.First.Result.ExecutionFailed && Capture.First.Before.Boundary > Capture.First.Now,
			"clock regression remains a distinct invalid rejection with original boundary");
	}
	{
		PooledExecutionAttribution Capture(true, false);
		detail::PooledServiceRecord R; R.NowMicroseconds = 111; R.ExecutionRejected = true;
		R.AcceptanceAvailable = false; R.DiagnosticOnly = true;
		R.ExecutionBefore.Captured = R.ExecutionBefore.Armed = true; R.ExecutionBefore.RequiredSince = 1;
		Capture.Sink.Record(&Capture, R);
		Check(Capture.First.Observed && !Capture.First.AcceptanceAvailable && Capture.First.Before.RequiredSince == 1,
			"absent acceptance retains first rejected check without inventing accepted bytes or forwarding fake evidence");
	}
	Check(Records == 5 && Grants == 1 && Lifecycles == 2 && detail::ActivePooledService == &Previous,
		"all original callbacks forward exactly once and restore scope");
	detail::ActivePooledService = Original;
	std::cout << "[Qualification:ExecutionCapture] deterministic=PASS natural_failure=NOT_CLAIMED\n";
	return Failures == 0;
}

void RunReliableGameplayWorkload(
	Engine &ServerRuntime, Engine &ClientRuntime, GameSession &Server, GameSession &Client,
	GameNetworkingSocketsTransport &ClientTransport, std::uint64_t &Tick,
	GameNetworkingSocketsTransport &ServerTransport,
	const std::shared_ptr<RemoteFunction> &ServerFunction,
	const std::shared_ptr<RemoteEvent> &ServerEvent, PooledExecutionAttribution *Attribution = nullptr
) {
	using Clock = std::chrono::steady_clock;
	const auto Connection = Client.GetPrimaryConnection();
	auto ClientFunction = std::dynamic_pointer_cast<RemoteFunction>(
		ClientRuntime.DataModel->FindFirstChild("QualificationFunction", true));
	auto ClientEvent = std::dynamic_pointer_cast<RemoteEvent>(
		ClientRuntime.DataModel->FindFirstChild("QualificationEvent", true));
	Check(Connection && ClientFunction && ClientEvent, "qualification Remotes materialize through GameSession");
	if (!Connection || !ClientFunction || !ClientEvent) return;
	auto *Sender = ClientFunction->GetRemoteManager();
	auto *Receiver = ServerFunction->GetRemoteManager();
	Check(Sender && Receiver, "qualification uses session-owned Remote managers");
	if (!Sender || !Receiver) return;
	const auto ServerConnections = detail::GameSessionTestAccess::GetConnections(Server);
	const std::array<glm::vec3, 1> Focus{glm::vec3{}};
	Check(ServerConnections.size() == 1 && Server.SetTrustedReplicationFocus(ServerConnections.front(), Focus),
		"mixed qualification fixes trusted relevance at its structural workload");

	struct Observation {
		std::vector<double> Rpc, Event, Action;
		std::uint64_t OfferedBytes = 0, AcceptedBytes = 0, Rejected = 0, Errors = 0;
		std::size_t PendingHighWater = 0, BacklogHighWater = 0;
		std::uint64_t RetainedHigh = 0, RequiredHigh = 0, MinimumMargin = DefaultChangeJournalCapacity;
		std::uint64_t OldestRequired = 0, OldestPeer = 0, PreparedSamples = 0;
		bool OldestCatalog = false;
		double MaximumRequiredAgeMs = 0;
		std::size_t ServerBacklogHigh = 0;
		std::uint64_t RecoveryActionRejections = 0;
		std::uint64_t StepCount = 0;
		test_detail::WorkloadTimingEvidence CpuTiming;
		test_detail::WorkloadActionEvidence ActionTiming;
		// Case reset moves only this pointer; the bounded record array never
		// creates a second large temporary on the Windows fixture stack.
		std::unique_ptr<test_detail::WorkloadRemoteEvidence> RemoteTiming = std::make_unique<test_detail::WorkloadRemoteEvidence>();
		test_detail::WorkloadSleepLedger CompletedSleeps{};
		double StepIntervalMs = 0, ClientRuntimeMs = 0, ServerRuntimeMs = 0;
		double ServerPollMs = 0, ClientPollMs = 0, ServerSessionMs = 0, ClientSessionMs = 0;
		double ObserverMs = 0, SleepOvershootMs = 0;
	};
	struct PendingEvent { Clock::time_point Started; std::vector<WireValue> Arguments; };
	Observation Results;
	// One scalar witness per named case from the existing Main-owned sink.
	// Forward every original callback; no payload history or changed health clock.
	struct ExecutionWitness {
		PooledPollAttribution Poll;
		std::uint64_t *Step = nullptr;
		unsigned Phase = 0, FirstPhase = 0;
		bool Observed = false;
		std::uint64_t FirstAt = 0, FirstStep = 0, PriorAt = 0, PriorStep = 0;
		std::uint64_t LastAt = 0, LastStep = 0, DebtToken = 0, DebtBytes = 0;
		std::uint64_t MaximumMain = 0, MaximumTurnover = 0, MaximumNative = 0, MaximumOrdinary = 0;
		bool CompletionMiss = false, OrdinaryMiss = false, Invalid = false;
		ConnectionId Connection;
		detail::PooledReliableServiceFeedback::Result FirstResult;
		detail::PooledServiceSink *Previous = detail::ActivePooledService;
		detail::PooledServiceSink Sink;
		explicit ExecutionWitness(std::uint64_t &Value) : Step(&Value) {
			Sink.Context = this;
			Sink.CaptureExecution = Previous && Previous->CaptureExecution;
			Sink.Record = [](void *Context, const detail::PooledServiceRecord &Record) noexcept {
				auto &Self = *static_cast<ExecutionWitness *>(Context);
				Self.MaximumMain = std::max(Self.MaximumMain, Record.Result.MaximumMainOpportunityMicroseconds);
				Self.MaximumTurnover = std::max(Self.MaximumTurnover, Record.Result.MaximumTurnoverMicroseconds);
				Self.Invalid |= Record.InvalidAfter || !Record.Result.Valid;
				if (Record.Feedback) {
					const auto &S = *Record.Feedback;
					if (S.StructuralLastCompletedGrantCompletedAtMicroseconds >= S.StructuralLastCompletedGrantActivatedAtMicroseconds)
						Self.MaximumNative = std::max(Self.MaximumNative, S.StructuralLastCompletedGrantCompletedAtMicroseconds - S.StructuralLastCompletedGrantActivatedAtMicroseconds);
					Self.MaximumOrdinary = std::max(Self.MaximumOrdinary, S.QualifiedOrdinaryMaximumLatencyMicroseconds);
					Self.CompletionMiss |= S.StructuralCompletionServiceFailed;
					Self.OrdinaryMiss |= S.ProtectedOrdinaryEverFailed || Record.Result.QueuedOrdinaryEverFailed;
				}
				if (!Self.Observed && Record.Result.ExecutionFailed) {
					Self.Poll.Freeze();
					Self.Observed = true; Self.FirstAt = Record.NowMicroseconds; Self.FirstStep = *Self.Step;
					Self.PriorAt = Self.LastAt; Self.PriorStep = Self.LastStep; Self.FirstPhase = Self.Phase;
					Self.Connection = Record.Connection; Self.DebtToken = Record.DebtToken; Self.DebtBytes = Record.DebtBytes;
					Self.FirstResult = Record.Result;
				}
				Self.LastAt = Record.NowMicroseconds; Self.LastStep = *Self.Step;
				if (Self.Previous && Self.Previous->Record && (!Record.DiagnosticOnly || Self.Previous->CaptureExecution))
					Self.Previous->Record(Self.Previous->Context, Record);
			};
			Sink.AcceptedGrant = [](void *Context, ConnectionId Id, std::uint64_t Token,
				std::uint64_t Bytes, std::uint64_t At) noexcept {
				auto &Self = *static_cast<ExecutionWitness *>(Context);
				if (Self.Previous && Self.Previous->AcceptedGrant)
					Self.Previous->AcceptedGrant(Self.Previous->Context, Id, Token, Bytes, At);
			};
			if (Previous && Previous->Lifecycle) Sink.Lifecycle = [](void *Context, const detail::PooledLifecycleRecord &Value) noexcept {
				auto &Self = *static_cast<ExecutionWitness *>(Context);
				Self.Previous->Lifecycle(Self.Previous->Context, Value);
			};
			detail::ActivePooledService = &Sink;
		}
		~ExecutionWitness() { detail::ActivePooledService = Previous; }
		void Reset() noexcept { Observed = CompletionMiss = OrdinaryMiss = Invalid = false;
			LastAt = LastStep = MaximumMain = MaximumTurnover = MaximumNative = MaximumOrdinary = 0; Poll.Reset(); }
	} Execution(Results.StepCount);
	std::map<int, PendingEvent> Events;
	int Sequence = 0;
	std::size_t PendingRequests = 0;
	auto ActionService = ClientRuntime.DataModel->GetService("CharacterControlService");
	int ActionSequence = 0;
	std::optional<Clock::time_point> ActionStarted;
	bool RecoveryProbeActive = false;
	const auto ActionTimestamp = [](Clock::time_point Value) {
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Value.time_since_epoch()).count());
	};
	struct JournalTime { std::uint64_t Sequence = 0; Clock::time_point Observed; };
	std::vector<JournalTime> JournalTimes(DefaultChangeJournalCapacity);
	std::uint64_t LastObservedTail = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId()).NextSequence;
	auto IsActionSequence = [&](const std::optional<WireValue> &Value) {
		return Value && (*Value == WireValue(ActionSequence) || *Value == WireValue(static_cast<double>(ActionSequence)));
	};
	Receiver->SetRequestHandler(ServerFunction->GetNetworkObjectId(),
		[](const RemoteInvocation &Invocation, RemoteManager::RequestReply Reply) {
			Check(Reply(Invocation.Arguments, {}), "qualification RPC reply accepted exactly once");
		});
	Receiver->SetEventHandler(ServerEvent->GetNetworkObjectId(),
		[&](const RemoteInvocation &Invocation) {
			Check(Receiver->SendEvent(Invocation.Peer.Connection, Invocation.Remote, Invocation.Arguments).Accepted(),
				"qualification Event echo accepted");
		});
	Sender->SetEventHandler(ClientEvent->GetNetworkObjectId(), [&](const RemoteInvocation &Invocation) {
		if (Invocation.Arguments.empty() || !std::holds_alternative<int>(Invocation.Arguments[0])) {
			++Results.Errors;
			return;
		}
		const auto Found = Events.find(std::get<int>(Invocation.Arguments[0]));
		if (Found == Events.end()) { ++Results.Errors; return; }
		const bool PayloadMatched = Invocation.Arguments == Found->second.Arguments;
		if (!PayloadMatched) ++Results.Errors;
		const auto CompletionEvidence = test_detail::WorkloadObservedEndpoint::Capture(Results.CompletedSleeps);
		const auto CompletedAt = CompletionEvidence.At;
		Results.Event.push_back(std::chrono::duration<double, std::milli>(CompletedAt - Found->second.Started).count());
		Results.RemoteTiming->End(static_cast<std::uint64_t>(Found->first), Results.StepCount, ActionTimestamp(CompletedAt),
			CompletionEvidence.Thread, -1, PayloadMatched, CompletionEvidence.Counters);
		Events.erase(Found);
	});

	auto Send = [&](bool Rpc, std::size_t FrameBytes) {
		const int Id = ++Sequence;
		std::vector<WireValue> Arguments{Id, std::string(FrameBytes - 62, static_cast<char>('a' + Id % 26))};
		RemoteMessage Message;
		Message.Kind = Rpc ? RemoteMessageKind::Request : RemoteMessageKind::ReliableEvent;
		Message.Remote = Rpc ? ClientFunction->GetNetworkObjectId() : ClientEvent->GetNetworkObjectId();
		if (Rpc) { Message.Request = RemoteRequestId{1}; Message.Deadline = 10s; }
		Message.Arguments = Arguments;
		const auto Encoded = EncodeRemoteMessage(Message);
		Check(Encoded && Encoded->size() == FrameBytes, "qualified payload uses exact encoded frame bytes");
		Results.OfferedBytes += FrameBytes + ReliableServiceEnvelopeBytes;
		const auto StartEvidence = test_detail::WorkloadObservedEndpoint::Capture(Results.CompletedSleeps);
		const auto Started = StartEvidence.At;
		Results.RemoteTiming->Begin(static_cast<std::uint64_t>(Id), Rpc, RecoveryProbeActive, Results.StepCount,
			ActionTimestamp(Started), StartEvidence.Thread, StartEvidence.Counters);
		RemoteSendResult Sent;
		Clock::time_point SubmittedAt;
		if (Rpc) {
			++PendingRequests;
			Sent = Sender->StartRequest(*Connection, Message.Remote, Arguments,
				[&, Started, Arguments, Id](RemoteRequestResult Result) {
					--PendingRequests;
					const bool PayloadMatched = Result.Results == Arguments;
					if (Result.Outcome.Status != RemoteRequestTerminalStatus::Success || !PayloadMatched)
						++Results.Errors;
					const auto CompletionEvidence = test_detail::WorkloadObservedEndpoint::Capture(Results.CompletedSleeps);
					const auto CompletedAt = CompletionEvidence.At;
					Results.Rpc.push_back(std::chrono::duration<double, std::milli>(CompletedAt - Started).count());
					Results.RemoteTiming->End(static_cast<std::uint64_t>(Id), Results.StepCount, ActionTimestamp(CompletedAt),
						CompletionEvidence.Thread, static_cast<int>(Result.Outcome.Status), PayloadMatched, CompletionEvidence.Counters);
				});
			SubmittedAt = Clock::now();
			if (!Sent.Accepted()) --PendingRequests;
		} else {
			Sent = Sender->SendEvent(*Connection, Message.Remote, Arguments);
			SubmittedAt = Clock::now();
			if (Sent.Accepted()) Events.emplace(Id, PendingEvent{Started, std::move(Arguments)});
		}
		Results.RemoteTiming->Submitted(static_cast<std::uint64_t>(Id), Sent.Accepted(), Results.StepCount,
			ActionTimestamp(SubmittedAt), test_detail::WorkloadNativeThread());
		if (Sent.Accepted()) Results.AcceptedBytes += FrameBytes + ReliableServiceEnvelopeBytes;
		else ++Results.Rejected;
		Results.PendingHighWater = std::max(Results.PendingHighWater, PendingRequests);
	};
	std::optional<Clock::time_point> PreviousStepStarted;
	test_detail::WorkloadCpuSample PreviousStepCpu;
	double PreviousRequestedSleepMs = 0, PreviousActualSleepMs = 0;
	auto Step = [&]() {
		using test_detail::CaptureWorkloadCpu;
		using test_detail::WorkloadPhase;
		const auto StepCpu = CaptureWorkloadCpu();
		const auto StepStarted = Clock::now();
		if (PreviousStepStarted) {
			Results.StepIntervalMs = std::max(Results.StepIntervalMs,
				std::chrono::duration<double, std::milli>(StepStarted - *PreviousStepStarted).count());
			Results.CpuTiming.Record(WorkloadPhase::StepInterval, Results.StepCount,
				std::chrono::duration<double, std::milli>(StepStarted - *PreviousStepStarted).count(),
				PreviousStepCpu, StepCpu, PreviousRequestedSleepMs, PreviousActualSleepMs,
				ActionTimestamp(*PreviousStepStarted), ActionTimestamp(StepStarted));
		}
		PreviousStepStarted = StepStarted;
		PreviousStepCpu = StepCpu;
		++Results.StepCount;
		if (Attribution) Attribution->Step = Results.StepCount;
		const auto Next = StepStarted + std::chrono::microseconds(16'667);
		auto Measure = [&](double &Maximum, WorkloadPhase Phase, auto &&Operation) {
			const auto CpuBefore = CaptureWorkloadCpu();
			const auto Started = Clock::now();
			Execution.Phase = static_cast<unsigned>(Phase);
			if (Attribution) { Attribution->Phase = static_cast<unsigned>(Phase); Attribution->PhaseBeginNs = ActionTimestamp(Started); }
			Operation();
			const auto Ended = Clock::now();
			const auto CpuAfter = CaptureWorkloadCpu();
			const auto WallMs = std::chrono::duration<double, std::milli>(Ended - Started).count();
			Maximum = std::max(Maximum, WallMs);
			Results.CpuTiming.Record(Phase, Results.StepCount, WallMs, CpuBefore, CpuAfter,
				0, 0, ActionTimestamp(Started), ActionTimestamp(Ended));
		};
		Measure(Results.ClientRuntimeMs, WorkloadPhase::ClientRuntime, [&] { ClientRuntime.Step(); });
		Measure(Results.ServerRuntimeMs, WorkloadPhase::ServerRuntime, [&] { ServerRuntime.Step(); });
		Measure(Results.ServerPollMs, WorkloadPhase::ServerPoll, [&] { (void)Server.Poll(); });
		Measure(Results.ClientPollMs, WorkloadPhase::ClientPoll, [&] {
			auto &Poll = Execution.Poll;
			Poll.Begin(Results.StepCount, ActionTimestamp(Clock::now()));
			{
				SteamNetworkingSocketsLib::GargantuanPollCallsiteScope Scope(Poll.Active ? &Poll.Sink : nullptr);
				(void)Client.Poll();
			}
			Poll.End(ActionTimestamp(Clock::now()));
		});
		Measure(Results.ServerSessionMs, WorkloadPhase::ServerSession, [&] { Server.Step(Tick); });
		Measure(Results.ClientSessionMs, WorkloadPhase::ClientSession, [&] { Client.Step(Tick++); });
		const auto ObserverCpu = CaptureWorkloadCpu();
		const auto ObserverStarted = Clock::now();
		if (ActionStarted) {
			const auto Resolved = ActionService->GetAttributeValue("WorkloadResolved");
			const auto Rejected = ActionService->GetAttributeValue("WorkloadRejected");
			if (IsActionSequence(Resolved)) {
				const auto ResolvedAt = Clock::now();
				const auto ResolvedCpu = CaptureWorkloadCpu();
				Results.Action.push_back(std::chrono::duration<double, std::milli>(ResolvedAt - *ActionStarted).count());
				Results.ActionTiming.End(ActionSequence, Results.StepCount, ActionTimestamp(ResolvedAt), ResolvedCpu, false);
				if (ActionService->GetAttributeValue("WorkloadAccepted") != std::optional<WireValue>(true)) ++Results.Errors;
				ActionStarted.reset();
			} else if (IsActionSequence(Rejected)) {
				const auto RejectedAt = Clock::now();
				Results.ActionTiming.End(ActionSequence, Results.StepCount, ActionTimestamp(RejectedAt), CaptureWorkloadCpu(), true);
				const auto Authority = detail::GameSessionTestAccess::GetCharacterMetrics(Server);
				const auto Prediction = detail::GameSessionTestAccess::GetCharacterMetrics(Client);
				std::cout << "[Qualification:ActionRejected] sequence=" << ActionSequence
					<< " recovery_probe=" << RecoveryProbeActive
					<< " commands_received=" << Authority.CommandsReceived << " commands_accepted=" << Authority.CommandsAccepted
					<< " stale_commands=" << Authority.StaleCommandsRejected << " states_sent=" << Authority.AuthoritativeStatesSent
					<< " authority_protocol_rejects=" << Authority.ProtocolRejects
					<< " states_considered=" << Authority.StatesConsidered << " states_suppressed=" << Authority.StatesSuppressedUnchanged
					<< " active_relationships=" << Authority.PublicationActiveRelationships
					<< " prediction_overflows=" << Prediction.HistoryOverflows << " prediction_resets=" << Prediction.HardResets
					<< " stale_states=" << Prediction.StaleStatesDropped << " prediction_protocol_rejects=" << Prediction.ProtocolRejects << '\n';
				for (const auto &Player : ServerRuntime.Players->GetPlayers()) {
					if (const auto Character = Player->GetCharacter()) {
						const auto Position = std::dynamic_pointer_cast<KinematicCharacter>(*Character)->GetPosition();
						const auto Value = std::dynamic_pointer_cast<KinematicCharacter>(*Character);
						const auto Velocity = Value->GetVelocity();
						const auto Normal = Value->GetFloorNormal();
						std::cout << "[Qualification:Character] x=" << Position.x << " y=" << Position.y << " z=" << Position.z
							<< " vx=" << Velocity.x << " vy=" << Velocity.y << " vz=" << Velocity.z
							<< " nx=" << Normal.x << " ny=" << Normal.y << " nz=" << Normal.z
							<< " quaternion_length=" << glm::length(Value->GetCFrame().ToQuaternion()) << '\n';
					}
				}
				// A locally refused recovery probe was never accepted. Recovery
				// is a deadline, not a promise that the first early probe succeeds.
				if (RecoveryProbeActive) ++Results.RecoveryActionRejections;
				else ++Results.Errors;
				ActionStarted.reset();
			}
		}
		if (const auto Statistics = ClientTransport.GetStatistics(*Connection))
			Results.BacklogHighWater = std::max(Results.BacklogHighWater, Statistics->QueuedReliableBytes.value_or(0));
		if (const auto Statistics = ServerTransport.GetStatistics(ServerConnections.front()))
			Results.ServerBacklogHigh = std::max(Results.ServerBacklogHigh, Statistics->QueuedReliableBytes.value_or(0));
		const auto Tail = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId());
		const auto ObservedAt = Clock::now();
		for (auto Sequence = std::max(LastObservedTail, Tail.NextSequence > DefaultChangeJournalCapacity ?
			Tail.NextSequence - DefaultChangeJournalCapacity : 1); Sequence < Tail.NextSequence; ++Sequence)
			JournalTimes[Sequence % JournalTimes.size()] = {Sequence, ObservedAt};
		LastObservedTail = Tail.NextSequence;
		const auto Oldest = ChangeJournal::Get().Read({Tail.Scope, 0}, 0).Cursor.NextSequence;
		Results.RetainedHigh = std::max(Results.RetainedHigh, Tail.NextSequence - Oldest);
		for (const auto &Requirement : detail::GameSessionTestAccess::GetJournalRequirements(Server)) {
			const auto Required = Tail.NextSequence - Requirement.Cursor.NextSequence;
			const auto Margin = Required >= DefaultChangeJournalCapacity ? 0 : DefaultChangeJournalCapacity - Required;
			if (Margin < Results.MinimumMargin || (Required && Margin == Results.MinimumMargin && !Requirement.Catalog)) {
				Results.MinimumMargin = Margin;
				Results.OldestRequired = Requirement.Cursor.NextSequence;
				Results.OldestPeer = Requirement.Connection.Slot;
				Results.OldestCatalog = Requirement.Catalog;
			}
			Results.RequiredHigh = std::max(Results.RequiredHigh, Required);
			const auto &Time = JournalTimes[Requirement.Cursor.NextSequence % JournalTimes.size()];
			if (Required && Time.Sequence == Requirement.Cursor.NextSequence)
				Results.MaximumRequiredAgeMs = std::max(Results.MaximumRequiredAgeMs,
					std::chrono::duration<double, std::milli>(ObservedAt - Time.Observed).count());
			Results.PreparedSamples += Requirement.PreparedCommit;
			Check(Requirement.Cursor.NextSequence >= Oldest, "live journal requirement remains retained");
		}
		const auto BeforeSleep = Clock::now();
		Results.ObserverMs = std::max(Results.ObserverMs,
			std::chrono::duration<double, std::milli>(BeforeSleep - ObserverStarted).count());
		const auto SleepCpu = CaptureWorkloadCpu();
		Results.CpuTiming.Record(WorkloadPhase::Observer, Results.StepCount,
			std::chrono::duration<double, std::milli>(BeforeSleep - ObserverStarted).count(), ObserverCpu, SleepCpu,
			0, 0, ActionTimestamp(ObserverStarted), ActionTimestamp(BeforeSleep));
		PreviousRequestedSleepMs = std::chrono::duration<double, std::milli>(std::max(Next, BeforeSleep) - BeforeSleep).count();
		Results.CompletedSleeps.Begin(ActionTimestamp(BeforeSleep),
			ActionTimestamp(std::max(Next, BeforeSleep)) - ActionTimestamp(BeforeSleep), test_detail::WorkloadNativeThread());
		std::this_thread::sleep_until(Next);
		const auto AfterSleep = Clock::now();
		Results.CompletedSleeps.End(ActionTimestamp(AfterSleep), test_detail::WorkloadNativeThread());
		const auto AfterSleepCpu = CaptureWorkloadCpu();
		PreviousActualSleepMs = std::chrono::duration<double, std::milli>(AfterSleep - BeforeSleep).count();
		Results.CpuTiming.Record(WorkloadPhase::Sleep, Results.StepCount, PreviousActualSleepMs,
			SleepCpu, AfterSleepCpu, PreviousRequestedSleepMs, PreviousActualSleepMs,
			ActionTimestamp(BeforeSleep), ActionTimestamp(AfterSleep));
		// Attribute only time beyond the requested sleep or an already-late
		// entry. These wall times include preemption and do not prove CPU cost.
		Results.SleepOvershootMs = std::max(Results.SleepOvershootMs,
			std::chrono::duration<double, std::milli>(AfterSleep - std::max(Next, BeforeSleep)).count());
	};
	auto Percentile = [](std::vector<double> Values, double Fraction) {
		if (Values.empty()) return 0.0;
		std::sort(Values.begin(), Values.end());
		return Values[static_cast<std::size_t>(std::ceil(Fraction * Values.size())) - 1];
	};
	struct Case { const char *Name; std::size_t Frame; int Concurrency; int IntervalTicks; bool Mixed; bool Qualified; bool StructuralOverload = false; };
	const Case Cases[]{
		{"small", 128, 1, 8, false, true},
		{"upper", 16 * 1024, 1, 80, false, true},
		{"burst-concurrent", 3 * 1024, 4, 40, false, true},
		{"mixed", 3 * 1024, 4, 40, true, true},
		{"gameplay-overload", 16 * 1024, 16, 40, false, false},
		{"structural-overload", 128, 1, 8, false, false, true},
		{"mixed-overload", 16 * 1024, 16, 40, false, false, true},
		{"recovery", 128, 1, 8, false, true},
	};
	if (Attribution) Attribution->CurrentStage = PooledExecutionAttribution::Stage::Warmup;
	for (int Frame = 0; Frame < 120; ++Frame) Step();
	for (const auto &Case : Cases) {
		if (Attribution) { Attribution->CurrentCase = Case.Name; Attribution->CurrentStage = PooledExecutionAttribution::Stage::Demand; }
		Results = {};
		Execution.Reset();
		// Structural overload retains normal ordinary protection. Only the two
		// declared 16-KiB/concurrency-16 ordinary overload sources leave that class.
		const bool OrdinaryNormal = Case.Frame != 16 * 1024 || Case.Concurrency != 16;
		Check(detail::GameSessionTestAccess::SetOrdinarySourceNormal(Server, OrdinaryNormal) &&
			detail::GameSessionTestAccess::SetOrdinarySourceNormal(Client, OrdinaryNormal),
			"workload declares prospective ordinary scope without resetting accepted ownership or credits");
		PreviousStepStarted.reset();
		RecoveryProbeActive = false;
		const auto CaseClockAnchor = test_detail::WorkloadClockAnchor::Capture();
		const auto Started = Clock::now();
		const auto Before = Server.GetMetrics();
		const auto JournalStart = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId()).NextSequence;
		const auto BeforeReplication = detail::GameSessionTestAccess::GetReplicationMetrics(Server);
		const auto BeforeClient = Client.GetMetrics();
		const auto BeforeClientReplica = BeforeClient.ClientReplica;
		std::vector<std::shared_ptr<Part>> Parts;
		if (Case.StructuralOverload) {
			for (int Index = 0; Index < 32; ++Index) {
				auto Object = std::make_shared<Part>();
				Object->SetName("PressurePart" + std::to_string(Index));
				Object->SetAnchored(true);
				Object->SetParent(ServerRuntime.DataModel);
				Parts.push_back(std::move(Object));
			}
			for (int Frame = 0; Frame < 120; ++Frame) Step();
		}
		for (int Frame = 0; Frame < 480 && Client.GetStatus() == GameSessionStatus::Ready; ++Frame) {
			if (Frame % 60 == 0 && !ActionStarted) {
				const auto ActionCpu = test_detail::CaptureWorkloadCpu();
				ActionStarted = Clock::now();
				(void)ActionService->ApplyAttributeMutation("WorkloadRequest", WireValue(++ActionSequence));
				Results.ActionTiming.Begin(ActionSequence, Results.StepCount, ActionTimestamp(*ActionStarted), ActionCpu, false);
			}
			if (Frame % Case.IntervalTicks == 0 && PendingRequests == 0) {
				for (int Index = 0; Index < Case.Concurrency; ++Index) Send(true, Case.Frame);
				if (Case.Frame < 16 * 1024 && Events.empty()) Send(false, Case.Frame);
			}
			if (Case.Frame == 16 * 1024 && Frame % Case.IntervalTicks == Case.IntervalTicks / 2 && Events.empty())
				Send(false, Case.Frame);
			if (Case.Mixed && Frame % 4 == 0) {
				auto Object = std::make_shared<Part>();
				Object->SetName("QualificationPart" + std::to_string(Frame));
				Object->SetAnchored(true);
				Object->SetParent(ServerRuntime.DataModel);
				Parts.push_back(std::move(Object));
			}
			if (Case.StructuralOverload) {
				for (int Index = 0; Index < 16; ++Index)
					Parts[static_cast<std::size_t>((Frame % 2) * 16 + Index)]->SetName(
						"Pressure" + std::to_string(Index) + "-" + std::to_string(Frame) + std::string(24 * 1024, 'p'));
			}
			if (!Case.Qualified && Frame == 479)
				for (int Index = 0; Index < Case.Concurrency; ++Index) Send(true, Case.Frame);
			Step();
		}
		const auto DemandEnded = Clock::now();
		if (Attribution) Attribution->CurrentStage = PooledExecutionAttribution::Stage::Drain;
		Check(detail::GameSessionTestAccess::SetOrdinarySourceNormal(Server, true) &&
			detail::GameSessionTestAccess::SetOrdinarySourceNormal(Client, true),
			"new recovery probes restore normal scope while prior accepted memberships remain frozen");
		const auto DrainDeadline = DemandEnded + 20s;
		const auto DemandEndMetrics = Server.GetMetrics();
		// Qualification-only bounded inventory. Count closed Name segments in
		// order, and one current Name per object in the barrier-free suffix.
		// Singleton frame encoding is a conservative complete-message byte bound;
		// unrelated retained records are charged at the unchanged complete G cap.
		std::uint64_t RetainedNames = 0, RetainedOther = 0, RetainedWorkBytes = 0;
		for (const auto &Requirement : detail::GameSessionTestAccess::GetJournalRequirements(Server)) {
			if (Requirement.Catalog) continue;
			const auto Read = ChangeJournal::Get().Read(Requirement.Cursor, DefaultChangeJournalCapacity);
			Check(Read.Status == ChangeReadStatus::Available, "cessation inventory remains in the authoritative journal window");
			std::set<ObjectId> CurrentNames;
			for (const auto &Record : Read.Records) {
				const auto *Name = std::get_if<PropertyUpdatedChange>(&Record.Payload);
				if (!Name || !Name->Replicated || Name->DeclaringClassSchemaId || Name->PropertyName != "Name" ||
					!std::holds_alternative<std::string>(Name->Value)) {
					++RetainedOther; RetainedWorkBytes += MaximumReliableServiceGroupBytes; continue;
				}
				WireValue Value = Name->Value;
				if (Record.Sequence >= Requirement.NameCoalescingBegin) {
					if (!CurrentNames.insert(Record.Object).second) continue;
					const auto Live = ObjectRegistry::Get().Lookup(Record.Object);
					Check(Live && !Live->GetDestroyed(), "retained current Name has live authoritative identity");
					if (Live) Value = Live->GetName();
				}
				ReplicationFrame Frame{ReplicationProtocolVersion, ReplicationMessageKind::Incremental,
					ReplicationEpoch(1), ReliableReplicationSequence(1)};
				Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Record.Object, "Name", std::move(Value)}});
				const auto Encoded = EncodeReplicationFrame(Frame);
				Check(Encoded.has_value(), "retained Name singleton has an exact complete-message encoding");
				if (Encoded) RetainedWorkBytes += Encoded->size() + ReliableServiceEnvelopeBytes;
				++RetainedNames;
			}
		}
		const auto ConvergenceAllowance = CandidateReliableService().IsPooled()
			? std::chrono::microseconds(test::pooled_recovery_contract::ConvergenceBoundUs(RetainedWorkBytes, RetainedWorkBytes))
			: std::chrono::duration_cast<std::chrono::microseconds>(20s);
		const auto ConvergenceDeadline = DemandEnded + ConvergenceAllowance;
		auto Converged = [&]() {
			return std::all_of(Parts.begin(), Parts.end(), [&](const auto &Object) {
				return ClientRuntime.DataModel->FindFirstChild(Object->GetName(), true) != nullptr;
			}) && Server.GetMetrics().JournalBacklogRecords == 0 && Server.GetMetrics().MaterializationBacklog == 0;
		};
		double ServiceRecoveryMs = -1, ConvergenceMs = -1;
		double RecoveryRpcMs = 0, RecoveryEventMs = 0, RecoveryActionMs = 0;
		std::uint64_t JournalAt20Seconds = 0;
		bool DeadlineObserved = false, RecoveryProbeSent = false;
		auto NextRecoveryProbe = DemandEnded;
		std::size_t RecoveryAttempts = 0;
		std::size_t RecoveryRpcIndex = 0, RecoveryEventIndex = 0, RecoveryActionIndex = 0;
		auto ExactPooledComplete = [&]() {
			if (!CandidateReliableService().IsPooled()) return true;
			const auto Snapshot = detail::GameSessionTestAccess::ReadPooledPostpass(Server);
			return Snapshot && AggregatePooledQuiescent(*Snapshot, ServerConnections);
		};
		while (Client.GetStatus() == GameSessionStatus::Ready && Clock::now() < ConvergenceDeadline) {
			const auto Now = Clock::now();
			const auto Current = Server.GetMetrics();
			if (ConvergenceMs < 0 && Converged()) ConvergenceMs = std::chrono::duration<double, std::milli>(Now - DemandEnded).count();
			if (!RecoveryProbeSent && Now >= NextRecoveryProbe && !PendingRequests && Events.empty() && !ActionStarted) {
				RecoveryRpcIndex = Results.Rpc.size(); RecoveryEventIndex = Results.Event.size(); RecoveryActionIndex = Results.Action.size();
				RecoveryProbeSent = true;
				RecoveryProbeActive = true;
				NextRecoveryProbe = Now + 1s;
				++RecoveryAttempts;
				Send(true, 128); Send(false, 128);
				const auto ActionCpu = test_detail::CaptureWorkloadCpu();
				ActionStarted = Clock::now();
				(void)ActionService->ApplyAttributeMutation("WorkloadRequest", WireValue(++ActionSequence));
				Results.ActionTiming.Begin(ActionSequence, Results.StepCount, ActionTimestamp(*ActionStarted), ActionCpu, true);
			}
			const auto ServerQueue = ServerTransport.GetStatistics(ServerConnections.front());
			const auto ClientQueue = ClientTransport.GetStatistics(*Connection);
			if (ServiceRecoveryMs < 0 && RecoveryProbeSent && !PendingRequests && Events.empty() && !ActionStarted &&
				Results.Rpc.size() > RecoveryRpcIndex && Results.Event.size() > RecoveryEventIndex && Results.Action.size() > RecoveryActionIndex &&
				ExactPooledComplete() &&
				!Current.ReliableAdmission.OutstandingBytes && !Current.ReliableAdmission.ActiveDrainGrants &&
				ServerQueue && ServerQueue->QueuedReliableBytes == std::optional<std::size_t>(0) &&
				ClientQueue && ClientQueue->QueuedReliableBytes == std::optional<std::size_t>(0)) {
				ServiceRecoveryMs = std::chrono::duration<double, std::milli>(Now - DemandEnded).count();
				RecoveryRpcMs = Results.Rpc[RecoveryRpcIndex]; RecoveryEventMs = Results.Event[RecoveryEventIndex];
				RecoveryActionMs = Results.Action[RecoveryActionIndex];
			}
			// A completed callback precedes transport ACK reconciliation. Keep its
			// evidence live while that ACK drains, until the next scheduled probe.
			if (ServiceRecoveryMs < 0 && RecoveryProbeSent && Now >= NextRecoveryProbe &&
				!PendingRequests && Events.empty() && !ActionStarted) {
				RecoveryProbeSent = false;
				RecoveryProbeActive = false;
			}
			if (!DeadlineObserved && Now >= DrainDeadline) { DeadlineObserved = true; JournalAt20Seconds = Current.JournalBacklogRecords; }
			if (ServiceRecoveryMs >= 0 && ConvergenceMs >= 0 && (!Case.StructuralOverload || DeadlineObserved)) break;
			if (Now >= DrainDeadline && ServiceRecoveryMs < 0) break;
			Step();
		}
		Check(Client.GetStatus() == GameSessionStatus::Ready, "single-peer workload remains connected");
		Check(ServiceRecoveryMs >= 0 && !PendingRequests && Events.empty() && !ActionStarted && ExactPooledComplete(),
			"bounded capture proves exact ACK retirement, cleared queues and completed recovery probes");
		Check(ConvergenceMs >= 0, "bounded capture proves complete structural convergence");
		Performance(ServiceRecoveryMs >= 0 && ServiceRecoveryMs <= 20'000 && RecoveryRpcMs <= 150 && RecoveryEventMs <= 250 && RecoveryActionMs <= 250,
			"recovery_20000ms_rpc150ms_event250ms_action250ms");
		Performance(ConvergenceMs >= 0 && ConvergenceMs <= std::chrono::duration<double, std::milli>(ConvergenceAllowance).count(), "workload_derived_convergence");
		const double DrainMs = std::chrono::duration<double, std::milli>(Clock::now() - DemandEnded).count();
		const auto After = Server.GetMetrics();
		const auto AfterReplication = detail::GameSessionTestAccess::GetReplicationMetrics(Server);
		const auto AfterClientReplica = Client.GetMetrics().ClientReplica;
		std::cout << "[Qualification:Recovery] case=" << Case.Name << " service_recovery_ms=" << ServiceRecoveryMs
			<< " convergence_ms=" << ConvergenceMs << " convergence_bound_ms=" << std::chrono::duration<double, std::milli>(ConvergenceAllowance).count()
			<< " journal_at_cessation=" << DemandEndMetrics.JournalBacklogRecords << " journal_at_20s=" << (DeadlineObserved ? std::to_string(JournalAt20Seconds) : "not-measured")
			<< " retained_name_upper=" << RetainedNames << " retained_other_upper=" << RetainedOther << " retained_complete_bytes_upper=" << RetainedWorkBytes
			<< " emitted_transitions=" << AfterClientReplica.OperationsApplied - BeforeClientReplica.OperationsApplied
			<< " emitted_frames=" << AfterClientReplica.FramesApplied - BeforeClientReplica.FramesApplied
			<< " coalesced_records=" << AfterReplication.OperationsCoalesced - BeforeReplication.OperationsCoalesced
			<< " recovery_rpc_ms=" << RecoveryRpcMs << " recovery_event_ms=" << RecoveryEventMs << " recovery_action_ms=" << RecoveryActionMs
			<< " recovery_attempts=" << RecoveryAttempts << " recovery_action_rejections=" << Results.RecoveryActionRejections
			<< " debt_high=" << After.ReliableAdmission.OutstandingHighWater << " grants_high=" << After.ReliableAdmission.DrainGrantsHighWater
			<< " admission_wait_us=" << After.ReliableAdmission.MaximumAdmissionWaitMicroseconds
			<< " planning_gap_ticks=" << After.PlanningMaximumServiceGapTicks
			<< " materialization_cpu_ns=" << After.MaterializationCpuNanoseconds - Before.MaterializationCpuNanoseconds
			<< " client_decode_ns=" << Client.GetMetrics().ClientStructuralDecodeNanoseconds - BeforeClient.ClientStructuralDecodeNanoseconds
			<< " client_apply_ns=" << Client.GetMetrics().ClientStructuralApplyNanoseconds - BeforeClient.ClientStructuralApplyNanoseconds
			<< " client_remote_gap_ns=" << Client.GetMetrics().ClientRemoteMaximumServiceGapNanoseconds << '\n';
		const auto JournalEnd = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId()).NextSequence;
		const double Seconds = std::chrono::duration<double>(DemandEnded - Started).count();
		const double P95 = Percentile(Results.Rpc, .95), P99 = Percentile(Results.Rpc, .99);
		const double Maximum = Percentile(Results.Rpc, 1), EventMaximum = Percentile(Results.Event, 1);
		std::cout << "[Qualification:ReliableGameplay] case=" << Case.Name << " duration_s=" << Seconds
			<< " frame_bytes=" << Case.Frame << " remote_offered_Bps=" << Results.OfferedBytes / Seconds
			<< " remote_accepted_bytes=" << Results.AcceptedBytes << " rejected=" << Results.Rejected
			<< " rpc_count=" << Results.Rpc.size() << " rpc_p95_ms=" << P95 << " rpc_p99_ms=" << P99
			<< " rpc_max_ms=" << Maximum << " event_count=" << Results.Event.size() << " event_max_ms=" << EventMaximum
			<< " action_count=" << Results.Action.size() << " action_max_ms=" << Percentile(Results.Action, 1)
			<< " errors=" << Results.Errors << " pending_high=" << Results.PendingHighWater
			<< " client_backlog_high=" << Results.BacklogHighWater << " drain_ms=" << DrainMs
			<< " server_backlog_high=" << Results.ServerBacklogHigh
			<< " structural_accepted_bytes=" << After.ReliableAdmission.AcceptedBytes - Before.ReliableAdmission.AcceptedBytes
			<< " qualification_grants=" << After.ReliableAdmission.QualificationGrants - Before.ReliableAdmission.QualificationGrants
			<< " grant_deferrals=" << After.ReliableAdmission.GrantDeferrals - Before.ReliableAdmission.GrantDeferrals
			<< " credit_deferrals=" << After.ReliableAdmission.CreditDeferrals - Before.ReliableAdmission.CreditDeferrals
			<< " peer_credit_high=" << After.ReliableAdmission.PeerCreditHighWater
			<< " global_credit_high=" << After.ReliableAdmission.GlobalCreditHighWater
			<< " journal_produced=" << JournalEnd - JournalStart << " journal_Bps_records=" << (JournalEnd - JournalStart) / Seconds
			<< " journal_lag_high=" << After.StructuralMaximumJournalLagRecords
			<< " journal_retained_high=" << Results.RetainedHigh << " journal_required_high=" << Results.RequiredHigh
			<< " journal_minimum_margin=" << Results.MinimumMargin << " journal_oldest_required=" << Results.OldestRequired
			<< " journal_owner=" << (!Results.RequiredHigh ? "none" : Results.OldestCatalog ? "catalog" : "peer") << " journal_owner_slot=" << Results.OldestPeer
			<< " prepared_samples=" << Results.PreparedSamples << '\n';
		std::cout << "[Qualification:Retention] case=" << Case.Name << " maximum_observed_required_age_ms="
			<< Results.MaximumRequiredAgeMs << " journal_recovery_backlog=" << After.JournalBacklogRecords
			<< " planning_records_high=" << After.PlanningRecordsHighWater
			<< " pending_enters=" << After.StructuralPendingEnters << " pending_leaves=" << After.StructuralPendingLeaves
			<< " decode_ns=" << Client.GetMetrics().ClientStructuralDecodeNanoseconds
			<< " apply_ns=" << Client.GetMetrics().ClientStructuralApplyNanoseconds << '\n';
		if (Attribution) Attribution->CurrentStage = PooledExecutionAttribution::Stage::Report;
		std::cout << "[Qualification:WorkloadTiming] case=" << Case.Name << " steps=" << Results.StepCount
			<< " step_interval_max_ms=" << Results.StepIntervalMs
			<< " client_runtime_max_ms=" << Results.ClientRuntimeMs << " server_runtime_max_ms=" << Results.ServerRuntimeMs
			<< " server_poll_max_ms=" << Results.ServerPollMs << " client_poll_max_ms=" << Results.ClientPollMs
			<< " server_session_max_ms=" << Results.ServerSessionMs << " client_session_max_ms=" << Results.ClientSessionMs
			<< " observer_max_ms=" << Results.ObserverMs << " sleep_overshoot_max_ms=" << Results.SleepOvershootMs << '\n';
		std::cout << "[Qualification:ExecutionWitness] case=" << Case.Name << " observed=" << Execution.Observed;
		if (Execution.Observed) std::cout << " first_at_us=" << Execution.FirstAt << " first_step=" << Execution.FirstStep
			<< " phase=" << Execution.FirstPhase << " prior_at_us=" << Execution.PriorAt << " prior_step=" << Execution.PriorStep
			<< " connection_slot=" << Execution.Connection.Slot << " connection_generation=" << Execution.Connection.Generation
			<< " main_failed=" << Execution.FirstResult.MainOpportunityFailed << " turnover_failed=" << Execution.FirstResult.TurnoverFailed
			<< " max_main_us=" << Execution.FirstResult.MaximumMainOpportunityMicroseconds
			<< " max_turnover_us=" << Execution.FirstResult.MaximumTurnoverMicroseconds
			<< " debt_token=" << Execution.DebtToken << " debt_bytes=" << Execution.DebtBytes;
		std::cout << '\n';
		if (Execution.Observed) Execution.Poll.Print(Case.Name);
		std::cout << "[Qualification:Performance] case=" << Case.Name << " contract=" << CandidateReliableService().ContractIdentity()
			<< " main_us=" << Execution.MaximumMain << " main_target_us=100000 turnover_us=" << Execution.MaximumTurnover
			<< " turnover_target_us=150000 native_body_us=" << Execution.MaximumNative
			<< " native_target_byte_us=" << NativeGrantCompletionCurve::CompletionBudgetByteMicroseconds
			<< " native_target_rate=" << NativeGrantCompletionCurve::RateBytesPerSecond
			<< " native_target_miss=" << Execution.CompletionMiss << " ordinary_us=" << Execution.MaximumOrdinary
			<< " ordinary_target_us=50000 ordinary_target_miss=" << Execution.OrdinaryMiss << " fatal=0\n";
		Check(!Execution.Invalid, "pooled timing policy cannot hide malformed accounting evidence");
		const auto Profile = CandidateReliableService().IsPooled() ? "POOLED_SERVICE" : "FULL_RESERVATION";
		Results.CpuTiming.Print(std::cout, Case.Name, Profile);
		CaseClockAnchor.Print(std::cout, Case.Name, Profile, "BEGIN");
		test_detail::WorkloadClockAnchor::Capture().Print(std::cout, Case.Name, Profile, "END");
		const auto ActionObservedAt = Clock::now();
		Results.ActionTiming.Print(std::cout, Case.Name, Results.StepCount, ActionTimestamp(ActionObservedAt),
			test_detail::CaptureWorkloadCpu(), ActionObservedAt >= DrainDeadline,
			CandidateReliableService().IsPooled() ? "POOLED_SERVICE" : "FULL_RESERVATION");
		Results.RemoteTiming->Print(std::cout, Case.Name, Profile, Results.StepCount, ActionTimestamp(ActionObservedAt),
			ActionObservedAt >= DrainDeadline);
		Check(After.ReliableAdmission.PeerCreditHighWater <= CandidateReliableService().PeerCreditCap() &&
			After.ReliableAdmission.GlobalCreditHighWater <= CandidateReliableService().GlobalCreditCap(),
			"finite peer and global credit stay within unchanged caps");
		Check(After.PlanningMaximumTickWork <= 65'536 && After.StructuralMaximumTransitionsSelectedPerTick <= 8'192,
			"qualification preserves planning and exact structural selection bounds");
		Check(!Results.Rpc.empty() && !Results.Event.empty(), "workload measures both RPC and Event completions");
		Check(Results.Errors == 0, "accepted workload has no corruption or terminal RPC error");
		if (Case.Qualified) {
			Check(Results.Rejected == 0, "qualified workload has no rejected calls");
			Performance(P95 <= 150 && P99 <= 250 && Maximum <= 500, "rpc_p95_150ms_p99_250ms_max500ms");
			Performance(EventMaximum <= 250, "event_250ms");
			Check(!Results.Action.empty(), "qualified workload completes an authoritative action");
			Performance(!Results.Action.empty() && Percentile(Results.Action, 1) <= 250, "action_250ms");
		}
		if (Attribution) Attribution->CurrentStage = PooledExecutionAttribution::Stage::CaseCleanup;
		for (const auto &Object : Parts) {
			Check(ClientRuntime.DataModel->FindFirstChild(Object->GetName(), true) != nullptr,
				"mixed structural workload reaches client application");
			Object->Destroy();
		}
		for (int Frame = 0; Frame < 60; ++Frame) Step();
		if (Client.GetStatus() != GameSessionStatus::Ready || PendingRequests || !Events.empty() || (Case.Qualified && Failures)) break;
	}
	// Do not leave callbacks referencing fixture-local observations behind.
	Receiver->SetRequestHandler(ServerFunction->GetNetworkObjectId(), {});
	Receiver->SetEventHandler(ServerEvent->GetNetworkObjectId(), {});
	if (Client.GetStatus() == GameSessionStatus::Ready) Sender->SetEventHandler(ClientEvent->GetNetworkObjectId(), {});
	if (Attribution) Attribution->CurrentStage = PooledExecutionAttribution::Stage::FinalStop;
	Client.Stop();
}

}
