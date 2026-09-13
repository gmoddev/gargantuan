#pragma once

#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "../src/network/GnsServiceDiagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
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
	struct AggregateBounds {
		std::uint64_t MinimumMargin = DefaultChangeJournalCapacity, RequiredHigh = 0, OldestSequence = 0, PendingHigh = 0;
		ConnectionId OldestOwner;
		bool CatalogOwner = false;
		double OldestAgeMs = 0, ServiceGapMs = 0;
	} Bounds;
	struct JournalTime { std::uint64_t Sequence = 0; Clock::time_point Observed; };
	std::vector<JournalTime> JournalTimes(DefaultChangeJournalCapacity);
	std::uint64_t LastJournalTail = 0;
	auto LastStep = Clock::now();
	bool Disconnected = false;
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
		PrimaryRuntime.Step();
		ServerRuntime.Step();
		(void)Server.Poll();
		(void)Primary.Poll();
		Server.Step(Tick);
		Primary.Step(Tick);
		for (auto &Peer : Peers) {
			(void)Peer->Session->Poll();
			if (!Peer->Runtime && Peer->Session->GetClientDataModel()) {
				Peer->Renderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
				Peer->Runtime = std::make_unique<Engine>(Peer->Session->GetClientDataModel(), Peer->Renderer.get(),
					std::function<void(std::string, std::string)>{},
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
				Peer->Runtime->ProcessService->Alive = true;
				Check(Peer->Session->AttachClientRuntime(*Peer->Runtime), "aggregate client attaches trusted runtime");
			}
			if (Peer->Runtime) Peer->Runtime->Step();
			Peer->Session->Step(Tick);
		}
		++Tick;
		const auto ObservedAt = Clock::now();
		Bounds.ServiceGapMs = std::max(Bounds.ServiceGapMs, std::chrono::duration<double, std::milli>(ObservedAt - LastStep).count());
		LastStep = ObservedAt;
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
		std::this_thread::sleep_until(Next);
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
	for (const bool Overload : {false, true, false}) {
		Bounds = {};
		LastStep = Clock::now();
		const auto Before = Server.GetMetrics();
		for (auto &Sample : Samples) Sample = {};
		const std::size_t Active = Overload ? QualificationPeerCount : std::min(8u, QualificationPeerCount);
		const int Concurrent = Overload ? 16 : 4;
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
						const auto Submitted = Clock::now();
						++Sample.Pending;
						const auto Sent = Managers[Index]->StartRequest(Connections[Index], Function->GetNetworkObjectId(), Arguments,
							[&, Index, Arguments, Submitted](RemoteRequestResult Result) {
								auto &Completed = Samples[Index];
								--Completed.Pending;
								++Completed.Completed;
								if (Result.Outcome.Status != RemoteRequestTerminalStatus::Success || Result.Results != Arguments) ++Completed.Errors;
								Completed.Latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - Submitted).count());
							});
						if (Sent.Accepted()) ++Sample.Accepted;
						else { --Sample.Pending; ++Sample.Errors; }
						Sample.PendingHigh = std::max(Sample.PendingHigh, Sample.Pending);
					}
				}
			}
			Step();
		}
		const auto Ended = Clock::now();
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
		const auto After = Server.GetMetrics();
		const auto &Admission = After.ReliableAdmission;
		std::cout << "[Qualification:AggregateBounds] overload=" << Overload << " duration_s=" << std::chrono::duration<double>(Ended - Started).count()
			<< " drain_ms=" << std::chrono::duration<double, std::milli>(Clock::now() - Ended).count()
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
		Check(Admission.PeerCreditHighWater <= CandidateReliableService().PeerBurst && Admission.GlobalCreditHighWater <= CandidateReliableService().GlobalBurst,
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

void RunReliableGameplayWorkload(
	Engine &ServerRuntime, Engine &ClientRuntime, GameSession &Server, GameSession &Client,
	GameNetworkingSocketsTransport &ClientTransport, std::uint64_t &Tick,
	GameNetworkingSocketsTransport &ServerTransport,
	const std::shared_ptr<RemoteFunction> &ServerFunction,
	const std::shared_ptr<RemoteEvent> &ServerEvent
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
	};
	struct PendingEvent { Clock::time_point Started; std::vector<WireValue> Arguments; };
	Observation Results;
	std::map<int, PendingEvent> Events;
	int Sequence = 0;
	std::size_t PendingRequests = 0;
	auto ActionService = ClientRuntime.DataModel->GetService("CharacterControlService");
	int ActionSequence = 0;
	std::optional<Clock::time_point> ActionStarted;
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
		if (Invocation.Arguments != Found->second.Arguments) ++Results.Errors;
		Results.Event.push_back(std::chrono::duration<double, std::milli>(Clock::now() - Found->second.Started).count());
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
		const auto Started = Clock::now();
		RemoteSendResult Sent;
		if (Rpc) {
			++PendingRequests;
			Sent = Sender->StartRequest(*Connection, Message.Remote, Arguments,
				[&, Started, Arguments](RemoteRequestResult Result) {
					--PendingRequests;
					if (Result.Outcome.Status != RemoteRequestTerminalStatus::Success || Result.Results != Arguments)
						++Results.Errors;
					Results.Rpc.push_back(std::chrono::duration<double, std::milli>(Clock::now() - Started).count());
				});
			if (!Sent.Accepted()) --PendingRequests;
		} else {
			Sent = Sender->SendEvent(*Connection, Message.Remote, Arguments);
			if (Sent.Accepted()) Events.emplace(Id, PendingEvent{Started, std::move(Arguments)});
		}
		if (Sent.Accepted()) Results.AcceptedBytes += FrameBytes + ReliableServiceEnvelopeBytes;
		else ++Results.Rejected;
		Results.PendingHighWater = std::max(Results.PendingHighWater, PendingRequests);
	};
	auto Step = [&]() {
		const auto Next = Clock::now() + std::chrono::microseconds(16'667);
		ClientRuntime.Step();
		ServerRuntime.Step();
		(void)Server.Poll();
		(void)Client.Poll();
		Server.Step(Tick);
		Client.Step(Tick++);
		if (ActionStarted) {
			const auto Resolved = ActionService->GetAttributeValue("WorkloadResolved");
			const auto Rejected = ActionService->GetAttributeValue("WorkloadRejected");
			if (IsActionSequence(Resolved)) {
				Results.Action.push_back(std::chrono::duration<double, std::milli>(Clock::now() - *ActionStarted).count());
				if (ActionService->GetAttributeValue("WorkloadAccepted") != std::optional<WireValue>(true)) ++Results.Errors;
				ActionStarted.reset();
			} else if (IsActionSequence(Rejected)) {
				const auto Authority = detail::GameSessionTestAccess::GetCharacterMetrics(Server);
				const auto Prediction = detail::GameSessionTestAccess::GetCharacterMetrics(Client);
				std::cout << "[Qualification:ActionRejected] sequence=" << ActionSequence
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
				++Results.Errors;
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
		std::this_thread::sleep_until(Next);
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
	for (int Frame = 0; Frame < 120; ++Frame) Step();
	for (const auto &Case : Cases) {
		Results = {};
		const auto Started = Clock::now();
		const auto Before = Server.GetMetrics();
		const auto JournalStart = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId()).NextSequence;
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
				ActionStarted = Clock::now();
				(void)ActionService->ApplyAttributeMutation("WorkloadRequest", WireValue(++ActionSequence));
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
		const auto DrainDeadline = DemandEnded + 20s;
		auto Converged = [&]() {
			return std::all_of(Parts.begin(), Parts.end(), [&](const auto &Object) {
				return ClientRuntime.DataModel->FindFirstChild(Object->GetName(), true) != nullptr;
			}) && Server.GetMetrics().JournalBacklogRecords == 0 && Server.GetMetrics().MaterializationBacklog == 0;
		};
		while (Client.GetStatus() == GameSessionStatus::Ready &&
			(PendingRequests || !Events.empty() || ActionStarted || !Converged()) && Clock::now() < DrainDeadline) Step();
		Check(Client.GetStatus() == GameSessionStatus::Ready, "single-peer workload remains connected");
		Check(PendingRequests == 0 && Events.empty() && !ActionStarted && Converged(),
			"accepted workload drains and structural state converges within fixed recovery deadline");
		const double DrainMs = std::chrono::duration<double, std::milli>(Clock::now() - DemandEnded).count();
		const auto After = Server.GetMetrics();
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
		Check(After.ReliableAdmission.PeerCreditHighWater <= CandidateReliableService().PeerBurst &&
			After.ReliableAdmission.GlobalCreditHighWater <= CandidateReliableService().GlobalBurst,
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
	Client.Stop();
}

}
