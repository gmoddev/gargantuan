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
#include <cstdlib>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingsockets_flat.h>

// Real GameSession/GNS qualification, deliberately separate from the transport
// capacity benchmark. All completions traverse production Remote dispatch.
namespace {

int DiagnosticDimension(const char *Name, int Default, int Minimum, int Maximum) {
	const auto *Text = std::getenv(Name);
	if (!Text) return Default;
	return std::clamp(std::atoi(Text), Minimum, Maximum);
}

void RunAggregateReliableGameplay(Engine &ServerRuntime, Engine &PrimaryRuntime,
	GameSession &Server, GameSession &Primary, std::uint16_t Port, std::uint64_t &Tick,
	const std::shared_ptr<RemoteFunction> &Function, const std::shared_ptr<RemoteEvent> &Event) {
	using Clock = std::chrono::steady_clock;
	const auto DiagnosticStart = Clock::now();
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
			std::cout << "[Qualification:PooledRecoveryInput] ordinary_profile_control=" << HealthyControl
				<< " active=" << Active << " concurrent=" << Concurrent << " extra_recovery_rpc=" << !HealthyControl << '\n';
		}
		const auto Deadline = Ended + 20s;
		std::vector<double> ConvergenceMs(QualificationPeerCount, -1);
		auto Converged = [&]() {
			bool All = true;
			for (std::size_t Index = 0; Index < Runtimes.size(); ++Index) {
				const bool Current = std::all_of(Pressure.begin(), Pressure.end(), [&](const auto &Object) {
					return Runtimes[Index]->DataModel->FindFirstChild(Object->GetName(), true) != nullptr;
				});
				if (Current && ConvergenceMs[Index] < 0)
					ConvergenceMs[Index] = std::chrono::duration<double, std::milli>(Clock::now() - Ended).count();
				All = All && Current;
			}
			return All;
		};
		auto Pending = [&]() { return std::any_of(Samples.begin(), Samples.end(), [](const Sample &Value) { return Value.Pending != 0; }); };
		while (!Disconnected && (Pending() || !Converged() || Server.GetMetrics().JournalBacklogRecords != 0) && Clock::now() < Deadline) Step();
		Check(!Pending() && Converged(), "aggregate accepted requests drain and all peers converge");
		const auto AggregateEndAnchor = test_detail::WorkloadClockAnchor::Capture();
		const bool AggregateDeadlineReached = Clock::now() >= Deadline;
		AggregatePhaseActive = false;
		const auto After = Server.GetMetrics();
		const auto &Admission = After.ReliableAdmission;
		const auto AggregateDrainMs = std::chrono::duration<double, std::milli>(Clock::now() - Ended).count();
		const auto AggregateCase = AggregateCases[AggregatePhase];
		const auto AggregateContext = " phase_index=" + std::to_string(AggregatePhase);
		AggregateBeginAnchor.Print(std::cout, AggregateCase, AggregateProfile, "BEGIN", AggregateContext);
		AggregateEndAnchor.Print(std::cout, AggregateCase, AggregateProfile, "END", AggregateContext);
		AggregateOperations->Print(std::cout, AggregateCase, AggregateProfile, AggregatePhase);
		for (const auto &Diagnostic : AggregatePeers) {
			std::cout << "[Qualification:AggregatePeerEvidence] case=" << AggregateCase << " profile=" << AggregateProfile
				<< Diagnostic->Context() << " configured=" << Diagnostic->Configured << " invalid=" << Diagnostic->Invalid << '\n';
			Diagnostic->Remote.Print(std::cout, AggregateCase, AggregateProfile, Tick, AggregateEndAnchor.SteadyNs,
				AggregateDeadlineReached, Diagnostic->Context());
		}
		std::cout << "[Qualification:AggregateBounds] overload=" << Overload << " duration_s=" << std::chrono::duration<double>(Ended - Started).count()
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
		Check(Admission.PeerCreditHighWater <= CandidateReliableService().PeerCreditCap() && Admission.GlobalCreditHighWater <= CandidateReliableService().GlobalCreditCap(),
			"aggregate byte credit stays within unchanged caps");
		Check(After.PlanningMaximumTickWork <= 65'536 && After.StructuralMaximumTransitionsSelectedPerTick <= 8'192,
			"aggregate planning and selection bounds remain unchanged");
		Check(After.JournalBacklogRecords == 0, "aggregate raw journal readers recover");
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
				<< " structural_received_bytes=" << Sessions[Index]->GetMetrics().ClientStructuralBytesReceived - StructuralBefore[Index]
				<< " convergence_ms=" << ConvergenceMs[Index]
				<< " admitted_Bps=" << Sample.Accepted * 3104 / std::chrono::duration<double>(Ended - Started).count() << '\n';
			Check(Sample.Errors == 0 && Sample.Completed == Sample.Accepted && (!Gameplay || Sample.Completed > 0),
				"every eligible aggregate peer completes accepted work without corruption");
			if (!Overload) Check(Percentile(.95) <= 150 && Percentile(.99) <= 250 && Percentile(1) <= 500,
				"aggregate qualified RPC meets unchanged gates");
		}
		for (auto &Object : Pressure) Object->Destroy();
		for (int Frame = 0; Frame < 120; ++Frame) Step();
		if (Failures) break;
		++AggregatePhase;
	}
	Receiver->SetRequestHandler(Function->GetNetworkObjectId(), {});
	Receiver->SetEventHandler(Event->GetNetworkObjectId(), {});
	// Stop while callback captures still exist, including on a failed drain.
	Primary.Stop();
	for (auto &Peer : Peers) Peer->Session->Stop();
	const auto CleanupDeadline = Clock::now() + 5s;
	while (!detail::GameSessionTestAccess::GetConnections(Server).empty() && Clock::now() < CleanupDeadline) {
		(void)Server.Poll(); Server.Step(Tick++); std::this_thread::sleep_for(1ms);
	}
	const auto Requirements = detail::GameSessionTestAccess::GetJournalRequirements(Server);
	const auto PeerRequirements = std::count_if(Requirements.begin(), Requirements.end(), [](const auto &Value) { return !Value.Catalog; });
	std::cout << "[Qualification:AggregateCleanup] peer_requirements=" << PeerRequirements
		<< " connections=" << detail::GameSessionTestAccess::GetConnections(Server).size() << '\n';
	Check(PeerRequirements == 0 && detail::GameSessionTestAccess::GetConnections(Server).empty(), "aggregate disconnect releases every peer journal owner");
	if (QualificationDiagnostic) SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(SteamNetworkingUtils(), k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
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

bool TestPooledExecutionCapture() {
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
		std::uint64_t *Step = nullptr;
		unsigned Phase = 0, FirstPhase = 0;
		bool Observed = false;
		std::uint64_t FirstAt = 0, FirstStep = 0, PriorAt = 0, PriorStep = 0;
		std::uint64_t LastAt = 0, LastStep = 0, DebtToken = 0, DebtBytes = 0;
		ConnectionId Connection;
		detail::PooledReliableServiceFeedback::Result FirstResult;
		detail::PooledServiceSink *Previous = detail::ActivePooledService;
		detail::PooledServiceSink Sink;
		explicit ExecutionWitness(std::uint64_t &Value) : Step(&Value) {
			Sink.Context = this;
			Sink.CaptureExecution = Previous && Previous->CaptureExecution;
			Sink.Record = [](void *Context, const detail::PooledServiceRecord &Record) noexcept {
				auto &Self = *static_cast<ExecutionWitness *>(Context);
				if (!Self.Observed && Record.Result.ExecutionFailed) {
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
		void Reset() noexcept { Observed = false; LastAt = LastStep = 0; }
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
		Measure(Results.ClientPollMs, WorkloadPhase::ClientPoll, [&] { (void)Client.Poll(); });
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
				Results.Rpc[RecoveryRpcIndex] <= 150 && Results.Event[RecoveryEventIndex] <= 250 && Results.Action[RecoveryActionIndex] <= 250 &&
				!Current.ReliableAdmission.OutstandingBytes && !Current.ReliableAdmission.ActiveDrainGrants &&
				ServerQueue && ServerQueue->QueuedReliableBytes == std::optional<std::size_t>(0) &&
				ClientQueue && ClientQueue->QueuedReliableBytes == std::optional<std::size_t>(0)) {
				ServiceRecoveryMs = std::chrono::duration<double, std::milli>(Now - DemandEnded).count();
				RecoveryRpcMs = Results.Rpc[RecoveryRpcIndex]; RecoveryEventMs = Results.Event[RecoveryEventIndex];
				RecoveryActionMs = Results.Action[RecoveryActionIndex];
			}
			if (ServiceRecoveryMs < 0 && RecoveryProbeSent && !PendingRequests && Events.empty() && !ActionStarted) {
				RecoveryProbeSent = false;
				RecoveryProbeActive = false;
			}
			if (!DeadlineObserved && Now >= DrainDeadline) { DeadlineObserved = true; JournalAt20Seconds = Current.JournalBacklogRecords; }
			if (ServiceRecoveryMs >= 0 && ConvergenceMs >= 0 && (!Case.StructuralOverload || DeadlineObserved)) break;
			if (Now >= DrainDeadline && ServiceRecoveryMs < 0) break;
			Step();
		}
		Check(Client.GetStatus() == GameSessionStatus::Ready, "single-peer workload remains connected");
		Check(ServiceRecoveryMs >= 0 && ServiceRecoveryMs <= 20'000 && RecoveryRpcMs <= 150 && RecoveryEventMs <= 250 && RecoveryActionMs <= 250,
			"service recovers within 20 seconds with cleared debt/grants/queues and qualified gameplay probes");
		Check(ConvergenceMs >= 0 && ConvergenceMs <= std::chrono::duration<double, std::milli>(ConvergenceAllowance).count(),
			"structural convergence meets the independent workload-derived bound");
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
			Check(P95 <= 150 && P99 <= 250 && Maximum <= 500, "qualified RPC meets unchanged p95/p99/max gates");
			Check(EventMaximum <= 250, "qualified Event echo meets unchanged RTT gate");
			Check(!Results.Action.empty() && Percentile(Results.Action, 1) <= 250, "qualified action result meets unchanged gate");
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
