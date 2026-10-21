// Manual Phase 1 only: four actual remote clients, bounded queued structural demand.
// No sustained 64 MiB/s, full32, provider, action, or deployment qualification claim.
#include "PhysicalGnsFundingProbeTrace.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <crtdbg.h>
#endif

namespace physical_probe {
using namespace gargantuan;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr unsigned OperationsPerGroup = 8;
constexpr unsigned ObjectCount = OperationsPerGroup * DemandGroupCount;
void Require(bool Good, const char *Why) { if (!Good) throw std::runtime_error(Why); }
std::uint64_t Number(const char *Text) {
	std::uint64_t Value = 0; const std::string_view Input(Text);
	const auto Parsed = std::from_chars(Input.data(), Input.data() + Input.size(), Value);
	Require(Parsed.ec == std::errc{} && Parsed.ptr == Input.data() + Input.size(), "invalid unsigned argument"); return Value;
}
std::string Name(unsigned Revision, unsigned Index, std::size_t Size) {
	std::string Value = "Revision" + std::to_string(100 + Revision) + "Part" + std::to_string(Index) + "-";
	Value.resize(Size, static_cast<char>('a' + Revision % 26)); return Value;
}

std::array<std::size_t, ObjectCount> NameSizes() {
	std::array<std::size_t, ObjectCount> Sizes; Sizes.fill(GroupBytes / OperationsPerGroup);
	ReplicationFrame Frame; Frame.Epoch = ReplicationEpoch(1); Frame.Sequence = ReliableReplicationSequence(1);
	for (unsigned I = 0; I < OperationsPerGroup; ++I)
		Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{ObjectId{I + 1, 1}, "Name", Name(1, I, Sizes[I])}});
	const auto Encoded = EncodeReplicationFrame(Frame); Require(bool(Encoded), "Name calibration codec failed");
	const auto Excess = Encoded->size() + ReliableServiceEnvelopeBytes - GroupBytes;
	Require(Excess < Sizes[0] / 2, "unexpected Name frame overhead"); Sizes[0] -= Excess;
	std::get<PropertyReplicationUpdate>(Frame.Operations[0].Intent).Value = Name(1, 0, Sizes[0]);
	const auto Exact = EncodeReplicationFrame(Frame);
	Require(Exact && Exact->size() + ReliableServiceEnvelopeBytes == GroupBytes, "Name frame is not exactly G");
	for (unsigned GroupIndex = 1; GroupIndex < DemandGroupCount; ++GroupIndex)
		for (unsigned Offset = 0; Offset < OperationsPerGroup; ++Offset)
			Sizes[GroupIndex * OperationsPerGroup + Offset] = Sizes[Offset];
	return Sizes; // Every wave prepares one complete maximum-size structural group.
}

std::array<std::string, ObjectCount> DemandNames(const std::array<std::size_t, ObjectCount> &Sizes, unsigned Revision) {
	std::array<std::string, ObjectCount> Values;
	for (unsigned I = 0; I < ObjectCount; ++I) Values[I] = Name(Revision, I, Sizes[I]);
	return Values;
}
double Milliseconds(Clock::duration Value) { return std::chrono::duration<double, std::milli>(Value).count(); }
double Percentile(std::vector<double> Values, unsigned P) {
	if (Values.empty()) return -1; std::sort(Values.begin(), Values.end());
	return Values[(Values.size() * P + 99) / 100 - 1];
}
struct Probe {
	bool Server, Producer, ReadinessSmoke, Finished = false, DoneSent = false, Cleaned = false, WasReady = false, ReadinessPassed = false;
	std::uint64_t Nonce, Tick = 0, Errors = 0;
	unsigned ExpectedClients, ReadinessObserved = 0, Wave = 0, Applied = 0, Sequence = 0;
	Trace Samples;
	HeadlessRenderer Renderer{Vector2(32, 32)};
	std::unique_ptr<Engine> Runtime;
	std::shared_ptr<GameNetworkingSocketsTransport> Transport;
	std::unique_ptr<GameSession> Session;
	std::shared_ptr<RemoteFunction> Function;
	std::shared_ptr<RemoteEvent> Event;
	std::array<std::shared_ptr<Instance>, ObjectCount> Objects;
	std::array<std::size_t, ObjectCount> Sizes = NameSizes();
	std::array<std::string, ObjectCount> ExpectedNames = DemandNames(Sizes, 2);
	RemoteManager *Manager = nullptr;
	struct Participant { std::string Nonce; bool Producer = false, Done = false, GameplayDone = false; unsigned Applied = 0; };
	std::map<ConnectionId, Participant> Participants;
	std::optional<Clock::time_point> RpcStart, EventStart;
	std::vector<double> RpcTimes, EventTimes;
	std::vector<WireValue> Echo;
	std::string FailureClass;
	bool Registered = false, RegisterPending = false, GameplayDoneSent = false, MaterializationCheckPending = false;
	Clock::time_point Started = Clock::now(), LastStep = Started, NextSend = Started, WaveStarted = Started;
	Clock::time_point ReadyAt{}, FinishAt{}, NextMaterializationCheck{};
	double MaximumStepMs = 0;
	glm::vec3 InitialPosition{};
	bool InputSent = false, Moved = false;
	Probe(bool IsServer, bool IsProducer, std::uint64_t ClientNonce, TransportEndpoint Endpoint, bool IsReadinessSmoke, unsigned ClientCount)
		: Server(IsServer), Producer(IsProducer), ReadinessSmoke(IsReadinessSmoke), Nonce(ClientNonce), ExpectedClients(ClientCount) {
		if (Server) Samples.Prepare();
		RpcTimes.reserve(512); EventTimes.reserve(512);
		const auto Profile = ReliableServiceProfile::PooledService();
		GameNetworkingSocketsTransportConfiguration Config;
		if (Server) { Config.MaximumConnections = Profile.MaximumConnections; Config.SendRate = static_cast<std::uint32_t>(Profile.BackendSendRate()); }
		Transport = std::make_shared<GameNetworkingSocketsTransport>(Config);
		if (Server) {
			auto World = std::make_shared<DataModel>();
			auto Floor = std::make_shared<Part>(); Floor->SetName("ProbeFloor"); Floor->SetAnchored(true);
			Floor->SetSize({1024, 1, 1024}); Floor->SetPosition({0, -1, 0}); Floor->SetParent(World->GetService("Workspace"));
			for (unsigned I = 0; I < Objects.size(); ++I) {
				auto Value = std::make_shared<Folder>(); Value->SetName("ProbePart" + std::to_string(I)); Value->SetParent(World); Objects[I] = Value;
			}
			Function = std::make_shared<RemoteFunction>(); Function->SetName("ProbeFunction"); Function->SetParent(World);
			Event = std::make_shared<RemoteEvent>(); Event->SetName("ProbeEvent"); Event->SetParent(World);
			Runtime = MakeRuntime(World, RuntimeMode::NetworkServer);
		}
		GameSessionConfiguration SessionConfig{.Role = Server ? GameSessionRole::Server : GameSessionRole::Client,
			.Endpoint = std::move(Endpoint), .Limits = GameSessionConfiguration::DefaultLimits(), .ClientNonce = Nonce,
			.AllowInsecureDevelopmentNetwork = true};
		if (Server) SessionConfig.ReliableService = Profile;
		Session = std::make_unique<GameSession>(Transport, SessionConfig, Runtime.get());
	}
	std::unique_ptr<Engine> MakeRuntime(std::shared_ptr<DataModel> World, RuntimeMode Mode) {
		auto Value = std::make_unique<Engine>(World, &Renderer, nullptr, EngineProviderConfiguration{.AudioEnabled = false, .Mode = Mode});
		Value->ProcessService->Alive = true; return Value;
	}
	~Probe() { if (!Cleaned) { try { Cleanup(); } catch (...) {} } }
	void Cleanup() {
		if (Cleaned) return;
		Session->Stop(); const auto M = Session->GetMetrics();
		const auto &A = M.ReliableAdmission;
		const bool Good = detail::GameSessionTestAccess::GetConnections(*Session).empty() &&
			detail::GameSessionTestAccess::GetJournalRequirements(*Session).empty() && !M.ReliableAdmissionPeerStates && !A.ActiveDrainGrants && !A.OutstandingBytes &&
			A.AcceptedBytes == A.VerifiedAttributedRetirement + A.TerminalReleasedBytes;
		if (Runtime) { Runtime->Destroy(); Runtime.reset(); }
		if (Server) Samples.Restore(); Cleaned = true;
		std::cout << "[Probe:Cleanup] good=" << Good << " created=" << A.AcceptedBytes << " retired=" << A.VerifiedAttributedRetirement
			<< " terminal=" << A.TerminalReleasedBytes << " outstanding=" << A.OutstandingBytes << " grants=" << A.ActiveDrainGrants << '\n';
		Require(Good, "cleanup/conservation failed");
	}
	void InstallHandlers() {
		if (Manager || !Runtime) return;
		if (!Server) {
			Function = std::dynamic_pointer_cast<RemoteFunction>(Runtime->DataModel->FindFirstChild("ProbeFunction", true));
			Event = std::dynamic_pointer_cast<RemoteEvent>(Runtime->DataModel->FindFirstChild("ProbeEvent", true));
			if (!Function || !Event) return;
			for (unsigned I = 0; I < Objects.size(); ++I) {
				Objects[I] = Runtime->DataModel->FindFirstChild("ProbePart" + std::to_string(I), true); if (!Objects[I]) return;
			}
		}
		Manager = Function->GetRemoteManager(); if (!Manager) return;
		Require(Manager->SetEventHandler(Event->GetNetworkObjectId(), [this](const RemoteInvocation &V) { OnEvent(V); }), "event handler registration failed");
		if (Server) Require(Manager->SetRequestHandler(Function->GetNetworkObjectId(),
			[this](const RemoteInvocation &V, RemoteManager::RequestReply Reply) {
				const auto &A = V.Arguments;
				if (A.size() == 3 && A[0] == WireValue(std::string("register")) && std::holds_alternative<std::string>(A[1]) && std::holds_alternative<bool>(A[2])) {
					const auto &N = std::get<std::string>(A[1]);
					if (Participants.size() >= PeerCount || Participants.contains(V.Peer.Connection) || N.size() > 20) { ++Errors; return; }
					for (const auto &[Id, P] : Participants) if (P.Nonce == N) { ++Errors; return; }
					Participants.emplace(V.Peer.Connection, Participant{N, std::get<bool>(A[2])});
				} else if (!Participants.contains(V.Peer.Connection) || !Participants.at(V.Peer.Connection).Producer ||
					A.size() != 2 || A[0] != WireValue(std::string("rpc"))) { ++Errors; return; }
				if (!Reply(A, {})) ++Errors;
			}), "request handler registration failed");
	}
	void OnEvent(const RemoteInvocation &V) {
		const auto &A = V.Arguments;
		if (A.size() != 2 || !std::holds_alternative<std::string>(A[0]) || !std::holds_alternative<int>(A[1])) { ++Errors; return; }
		const auto &Kind = std::get<std::string>(A[0]); const int Value = std::get<int>(A[1]);
		if (Server) {
			const auto Found = Participants.find(V.Peer.Connection); if (Found == Participants.end()) { ++Errors; return; }
			auto &P = Found->second;
			if (Kind == "event" && P.Producer) { if (!Manager->SendEvent(V.Peer.Connection, V.Remote, A).Accepted()) ++Errors; }
			else if (Kind == "applied" && Value == int(Wave) && Value == int(P.Applied + 1)) P.Applied = Value;
			else if (Kind == "gameplay-done" && P.Producer && Value == 1 && !P.GameplayDone) P.GameplayDone = true;
			else if (Kind == "done" && Finished && Value == 1 && !P.Done) P.Done = true;
			else ++Errors;
		} else if (Kind == "wave" && Value == int(Wave + 1) && Value <= int(WaveCount)) {
			Wave = unsigned(Value); ExpectedNames = DemandNames(Sizes, Wave + 1);
		}
		else if (Kind == "finish" && Value == int(WaveCount) && Applied == WaveCount) { Finished = true; FinishAt = Clock::now(); }
		else if (Kind == "event" && EventStart && A == Echo) {
			const auto Ms = Milliseconds(Clock::now() - *EventStart); EventTimes.push_back(Ms); EventStart.reset(); if (Ms > 250) ++Errors;
		} else ++Errors;
	}
	void Broadcast(const std::string &Kind, int Value) {
		for (const auto &[Id, P] : Participants)
			Require(Manager->SendEvent(Id, Event->GetNetworkObjectId(), {Kind, Value}).Accepted(), "orchestration event rejected");
	}
	bool GameplayPassed() const {
		return !Errors && !RpcStart && !EventStart && (!Producer || (RpcTimes.size() >= 60 && EventTimes.size() >= 60 &&
			Percentile(RpcTimes, 95) <= 150 && Percentile(RpcTimes, 99) <= 250 && Percentile(RpcTimes, 100) <= 500 && Percentile(EventTimes, 100) <= 250));
	}
	void ClientWork() {
		if (!Manager || Session->GetStatus() != GameSessionStatus::Ready) return;
		const auto Id = Session->GetPrimaryConnection(); Require(bool(Id), "client connection missing");
		if (!Registered && !RegisterPending) {
			RegisterPending = true;
			const std::vector<WireValue> Args{std::string("register"), std::to_string(Nonce), Producer};
			Require(Manager->StartRequest(*Id, Function->GetNetworkObjectId(), Args, [this, Args](RemoteRequestResult R) {
				RegisterPending = false; Registered = R.Outcome.Status == RemoteRequestTerminalStatus::Success && R.Results == Args;
				if (!Registered) ++Errors;
			}).Accepted(), "registration refused");
		}
		if (!Registered) return;
		const auto Now = Clock::now();
		if (const auto Player = Runtime->Players->GetLocalPlayer(); Player && (*Player)->GetCharacter()) {
			const auto Position = (*Player)->GetCharacter().value()->GetPosition();
			if (!InputSent) {
				InitialPosition = Position; InputSent = true;
				(void)Runtime->ProcessEvent(KeyEvent{.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W, .State = ButtonState::Pressed});
			}
			if (glm::length(glm::vec2(Position.x - InitialPosition.x, Position.z - InitialPosition.z)) > 0.25f) {
				Moved = true;
				(void)Runtime->ProcessEvent(KeyEvent{.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W, .State = ButtonState::Released});
			}
		}
		if (Wave > Applied && Now >= NextMaterializationCheck) {
			bool Matches = true;
			for (unsigned I = 0; I < Objects.size(); ++I) Matches = Matches && Objects[I]->GetName() == ExpectedNames[I];
			NextMaterializationCheck = Now + 50ms;
			if (Matches) { Applied = Wave; Require(Manager->SendEvent(*Id, Event->GetNetworkObjectId(), {std::string("applied"), int(Applied)}).Accepted(), "application report rejected"); }
		}
		if (RpcStart) Require(Now - *RpcStart <= 500ms, "RPC local deadline exceeded");
		if (EventStart) Require(Now - *EventStart <= 250ms, "Event local deadline exceeded");
		if (Producer && !Finished && RpcTimes.size() < 80 && Now >= NextSend && !RpcStart && !EventStart) {
			Require(RpcTimes.size() < 512 && EventTimes.size() < 512, "gameplay sample bound exceeded");
			const int N = int(++Sequence); RpcStart = EventStart = Now; NextSend = Now + 100ms;
			const std::vector<WireValue> Args{std::string("rpc"), N}; Echo = {std::string("event"), N};
			Require(Manager->StartRequest(*Id, Function->GetNetworkObjectId(), Args, [this, Args](RemoteRequestResult R) {
				if (!RpcStart) { ++Errors; return; }
				const auto Ms = Milliseconds(Clock::now() - *RpcStart); RpcTimes.push_back(Ms); RpcStart.reset();
				if (R.Outcome.Status != RemoteRequestTerminalStatus::Success || R.Results != Args || Ms > 500) ++Errors;
			}).Accepted(), "ordinary RPC refused");
			Require(Manager->SendEvent(*Id, Event->GetNetworkObjectId(), Echo).Accepted(), "ordinary Event refused");
		}
		if (Producer && !GameplayDoneSent && GameplayPassed()) {
			Require(Manager->SendEvent(*Id, Event->GetNetworkObjectId(), {std::string("gameplay-done"), 1}).Accepted(), "gameplay completion report rejected");
			GameplayDoneSent = true;
		}
		if (Finished && !DoneSent && !RpcStart && !EventStart) {
			Require(GameplayPassed() && Moved && Session->GetMetrics().ClientCharacterMessagesHandled, "client service/control gate failed");
			Require(Manager->SendEvent(*Id, Event->GetNetworkObjectId(), {std::string("done"), 1}).Accepted(), "completion report refused"); DoneSent = true;
		}
	}
	bool ServerWork() {
		const auto Now = Clock::now(); const auto M = Session->GetMetrics();
		if (ReadinessSmoke) {
			const auto ActiveConnections = detail::GameSessionTestAccess::GetConnections(*Session).size();
			Require(M.ReadyPeers <= ExpectedClients && ActiveConnections <= ExpectedClients, "unexpected readiness-smoke client count");
			if (M.ReadyPeers == ExpectedClients && ActiveConnections == ExpectedClients) {
				if (ReadyAt == Clock::time_point{}) ReadyAt = Now;
				ReadinessObserved = M.ReadyPeers;
				if (Now - ReadyAt >= 1000ms) { ReadinessPassed = true; return true; }
			} else {
				Require(ReadyAt == Clock::time_point{} && Now - Started < 15s, "readiness-smoke client timeout/disconnect");
			}
			return false;
		}
		if (Wave) Require(detail::GameSessionTestAccess::GetConnections(*Session).size() == PeerCount, "live client disconnected");
		Require(M.ReadyPeers <= PeerCount && Participants.size() <= PeerCount, "unexpected client count");
		if (M.ReadyPeers != PeerCount || Participants.size() != PeerCount) { Require(!Wave && Now - Started < 15s, "client ready timeout/disconnect"); return false; }
		unsigned Producers = 0; for (const auto &[Id, P] : Participants) Producers += P.Producer;
		Require(Producers == 1, "exactly one gameplay producer required");
		if (ReadyAt == Clock::time_point{}) ReadyAt = Now;
		if (Finished) {
			Require(Now - FinishAt < 3s, "client completion timeout");
			return std::all_of(Participants.begin(), Participants.end(), [](const auto &P) { return P.second.Done; }) && Now - FinishAt >= 500ms;
		}
		const bool Drained = !M.ReliableAdmission.OutstandingBytes && !M.ReliableAdmission.ActiveDrainGrants;
		const bool JournalClear = std::all_of(Participants.begin(), Participants.end(), [this](const auto &Entry) {
			const auto It = std::ranges::find_if(Samples.Peers, [&Entry](const auto &Peer) { return Peer.Id == Entry.first; });
			return It != Samples.Peers.end() && It->HasPrevious && !It->Previous.Sample.StructuralJournalLag;
		});
		if (Wave) Require(Now - WaveStarted < 6s, "bounded structural wave/debt timeout");
		const bool AppliedAll = std::all_of(Participants.begin(), Participants.end(), [this](const auto &P) { return P.second.Applied == Wave; });
		const bool GameplayDone = std::all_of(Participants.begin(), Participants.end(), [](const auto &P) { return !P.second.Producer || P.second.GameplayDone; });
		// Arm the next complete journal group as soon as the previous one has
		// reached every client. Credit and ACK-gated gaps do not accrue F1 drain
		// deficit; the next accepted grant receives its own finite obligation.
		if (Now - ReadyAt < 1500ms || !AppliedAll || !JournalClear || (!Wave && !Drained)) return false;
		if (Wave < WaveCount) {
			++Wave; Samples.Demand = 1; WaveStarted = Now; Broadcast("wave", Wave);
			ExpectedNames = DemandNames(Sizes, Wave + 1);
			for (unsigned I = 0; I < Objects.size(); ++I) Objects[I]->SetName(ExpectedNames[I]);
			return false;
		}
		if (!GameplayDone || !Drained) return false;
		Samples.Analyze();
		Require(Samples.Passed(), "F1 four-peer finite-grant drain campaign incomplete");
		Require(detail::GameSessionTestAccess::GetCharacterMetrics(*Session).CommandsAccepted > 0, "no authoritative input accepted");
		Require(!M.ReliableAdmission.TerminalReleasedBytes, "terminal release cannot count as drain");
		Finished = true; FinishAt = Now; Broadcast("finish", WaveCount);
		return false;
	}
	void Run() {
		if (Server) Samples.Install(); Require(Session->Start().Succeeded(), "session start failed");
		bool Complete = false;
		while (!Complete && Clock::now() - Started < 70s) {
			const auto Begin = Clock::now(); MaximumStepMs = std::max(MaximumStepMs, Milliseconds(Begin - LastStep)); LastStep = Begin;
			(void)Session->Poll();
			if (!Server && !Runtime && Session->GetClientDataModel()) {
				Runtime = MakeRuntime(Session->GetClientDataModel(), RuntimeMode::NetworkClient);
				Require(Session->AttachClientRuntime(*Runtime), "client runtime attach failed");
			}
			if (Runtime) Runtime->Step();
			Session->Step(++Tick); InstallHandlers();
			WasReady = WasReady || Session->GetStatus() == GameSessionStatus::Ready;
			if (Session->GetStatus() == GameSessionStatus::Failed) {
				if (ReadinessSmoke && !Server && WasReady && Session->GetFailure() == "Game server connection closed") { ReadinessPassed = true; Complete = true; break; }
				if (!Server && DoneSent && GameplayPassed()) { Complete = true; break; }
				FailureClass = "H-client-session-failure";
				throw std::runtime_error("unexpected session failure: " + Session->GetFailure());
			}
			Require(!Errors && !Samples.Overflow && !Samples.Invalid && !Samples.FloorFailure, "observed service/feedback failure");
			if (Server) Complete = ServerWork();
			else { ClientWork(); Complete = DoneSent && Clock::now() - FinishAt >= 1s; }
			std::this_thread::sleep_until(Begin + (Server ? 4ms : 5ms));
		}
		Require(Complete && !Errors, "bounded run incomplete");
	}
	void Report() {
		if (ReadinessSmoke) std::cout << "[Probe:Readiness] result=" << (ReadinessPassed ? "pass" : "fail")
			<< " role=" << (Server ? "server" : "client") << " ready=" << (Server ? ReadinessObserved : static_cast<unsigned>(WasReady))
			<< " expected=" << (Server ? ExpectedClients : 1) << " clean_remote_shutdown=" << (!Server && ReadinessPassed) << '\n';
		std::cout << "[Probe:Summary] role=" << (Server ? "server" : "client") << " nonce=" << Nonce << " producer=" << Producer
			<< " waves=" << Wave << " applied=" << Applied << " moved=" << Moved << " rpc_n=" << RpcTimes.size() << " event_n=" << EventTimes.size()
			<< " rpc_p95_ms=" << Percentile(RpcTimes, 95) << " rpc_p99_ms=" << Percentile(RpcTimes, 99) << " rpc_max_ms=" << Percentile(RpcTimes, 100)
			<< " event_max_ms=" << Percentile(EventTimes, 100) << " step_max_ms=" << MaximumStepMs << " errors=" << Errors
			<< " samples=" << Samples.Rows.size() << " overflow=" << Samples.Overflow << " invalid=" << Samples.Invalid << " floor_failure=" << Samples.FloorFailure << '\n';
		if (Server) {
			Samples.Analyze();
			const auto Profile = ReliableServiceProfile::PooledService();
			Samples.Dump("physical-gns-server.csv");
			for (const auto &[Id, P] : Participants) std::cout << "[Probe:Peer] slot=" << Id.Slot << " generation=" << Id.Generation << " nonce=" << P.Nonce << " producer=" << P.Producer << '\n';
			std::uint64_t DemandRows = 0, FourGrantDebtRows = 0;
			for (const auto &R : Samples.Rows) {
				if (R.Sample.StructuralJournalLag) ++DemandRows;
				if (R.Sample.StructuralJournalLag && R.Sample.DebtToken && R.Sample.Feedback &&
					R.Sample.Feedback->ActiveAttributedRetirementToken == R.Sample.DebtToken &&
					R.Sample.Admission.ActiveDrainGrants == PeerCount) ++FourGrantDebtRows;
			}
			const auto Cause = Participants.size() < PeerCount ? "H-client-readiness-or-disconnect" :
				Samples.FloorFailure ? "C-grant-drain-or-delivery" : Samples.Invalid ? "D-feedback-or-attribution" :
				!Samples.PoolQualifiedUs ? "B-four-grant-firstsend-overlap" : "A-campaign-convergence-or-turnover";
			std::cout << "[Probe:ServiceCurve] contract=F1 peer_rate_Bps=" << Profile.Pooled.PeerDrainFloor
				<< " pool_rate_Bps=" << Profile.Pooled.StructuralPool << " quantum_B=" << PooledReliableServiceProfile::ServiceQuantumBytes
				<< " startup_us=5000 run_us=1000"
				<< " peer_finite_intercept_byte_us=" << FiniteLatencyByteMicroseconds
				<< " peer_running_bound_byte_us=" << RunningDeficitBound
				<< " pool_running_bound_byte_us=" << PooledReliableServiceProfile::FourGrantPoolRunningBoundByteMicroseconds
				<< " pool_common_run_us=" << Samples.PoolQualifiedUs
				<< " pool_episodes=" << Samples.PoolEpisodes
				<< " pool_curve=derived-from-four-native-grant-curves"
				<< " demand_rows=" << DemandRows << " four_grant_debt_rows=" << FourGrantDebtRows
				<< " verdict=" << (Samples.Passed() ? "PASS" : "FAIL")
				<< " failure_class=" << (Samples.Passed() ? "none" : FailureClass.empty() ? Cause : FailureClass) << '\n';
			for (unsigned I = 0; I < PeerCount; ++I) {
				const auto &P = Samples.Peers[I];
				std::cout << "[Probe:PeerService] slot=" << P.Id.Slot << " grants=" << P.GrantCount
					<< " qualified_grants=" << P.QualifiedGrants
					<< " completed_grants=" << std::count_if(P.Grants.begin(), P.Grants.begin() + P.GrantCount,
						[](const Trace::Grant &G) { return G.CompletedAtMicroseconds != 0; })
					<< " structural_first=" << P.LastFirst - P.BaselineFirst
					<< " structural_ack=" << P.LastAck - P.BaselineAck
					<< " running_us=" << P.LastActiveUs - P.BaselineActiveUs
					<< " max_run_deficit_byte_us=" << P.MaximumDeficit
					<< " retry=" << P.Retry << '\n';
			}
			const auto M = Session->GetMetrics().ReliableAdmission;
			std::cout << "[Probe:Admission] accepted=" << M.AcceptedBytes << " retired=" << M.VerifiedAttributedRetirement
				<< " terminal=" << M.TerminalReleasedBytes << " outstanding=" << M.OutstandingBytes
				<< " grants=" << M.ActiveDrainGrants << " grants_high_water=" << M.DrainGrantsHighWater
				<< " credit_deferrals=" << M.CreditDeferrals << " grant_deferrals=" << M.GrantDeferrals
				<< " fairness_deferrals=" << M.FairnessDeferrals << " feedback_deferrals=" << M.FeedbackDeferrals
				<< " pending_high_water=" << M.OutstandingHighWater << '\n';
		} else {
			std::ofstream Out("physical-gns-client-" + std::to_string(Nonce) + ".csv"); Out.exceptions(std::ios::failbit | std::ios::badbit);
			Out << "kind,sequence,rtt_ms\n";
			for (std::size_t I = 0; I < RpcTimes.size(); ++I) Out << "rpc," << I + 1 << ',' << RpcTimes[I] << '\n';
			for (std::size_t I = 0; I < EventTimes.size(); ++I) Out << "event," << I + 1 << ',' << EventTimes[I] << '\n';
		}
	}
};
void SelfTest() {
	const auto Sizes = NameSizes();
	for (unsigned WaveIndex = 1; WaveIndex <= WaveCount; ++WaveIndex) {
		ReplicationFrame Frame; Frame.Epoch = ReplicationEpoch(1); Frame.Sequence = ReliableReplicationSequence(WaveIndex);
		for (unsigned GroupIndex = 0; GroupIndex < DemandGroupCount; ++GroupIndex) {
			Frame.Operations.clear();
			for (unsigned Offset = 0; Offset < OperationsPerGroup; ++Offset) {
				const auto Index = GroupIndex * OperationsPerGroup + Offset;
				Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{ObjectId{Index + 1, 1}, "Name", Name(WaveIndex + 1, Index, Sizes[Index])}});
			}
			const auto Encoded = EncodeReplicationFrame(Frame);
			if (!Encoded || Encoded->size() + ReliableServiceEnvelopeBytes != GroupBytes)
				std::cerr << "[Probe:SelfTest] wave=" << WaveIndex << " group=" << GroupIndex
					<< " bytes=" << (Encoded ? Encoded->size() + ReliableServiceEnvelopeBytes : 0) << " expected=" << GroupBytes << '\n';
			Require(Encoded && Encoded->size() + ReliableServiceEnvelopeBytes == GroupBytes, "barriered workload group is not exactly G");
		}
		Require(Name(WaveIndex + 1, ObjectCount - 1, Sizes.back()) !=
			Name(WaveIndex, ObjectCount - 1, Sizes.back()), "next group coalesced across waves");
	}
	Require(Number("39450") == 39450 && Number("18446744073709551615") == UINT64_MAX, "unsigned parsing failed");
	for (const auto *Bad : {"", "-1", "+1", "1x", "18446744073709551616"}) {
		bool Rejected = false; try { (void)Number(Bad); } catch (const std::exception &) { Rejected = true; }
		Require(Rejected, "invalid numeric argument accepted");
	}
	// Socket-free campaign records exercise independent finite grants, real
	// four-grant first-send overlap, zero-ACK startup, native failure and staleness.
	auto FeedCurve = [](Trace &T, unsigned Step, unsigned Wave, unsigned Point,
		bool Slow = false, bool Stale = false) {
		const std::uint64_t Start = 1'000'000 + std::uint64_t(Wave - 1) * 1'100'000;
		const std::uint64_t Time = Start + (Point == 0 ? 1000 : Point == 1 ? 6000 : 31'000);
		for (unsigned Peer = 0; Peer < PeerCount; ++Peer) {
			const ConnectionId Id{Peer + 1, 1};
			const std::uint64_t Token = Wave * PeerCount + Peer + 1;
			const std::uint64_t Prior = std::uint64_t(Wave - 1) * GroupBytes;
			const bool Active = Point != 2;
			detail::PooledServiceRecord R{.Connection = Id, .SimulationTick = Step,
				.NowMicroseconds = Time, .DebtToken = Active ? Token : 0,
				.DebtBytes = Active ? GroupBytes : 0,
				.StructuralJournalLag = Active ? 512ULL : 0ULL};
			R.Feedback = detail::ReliableServiceFeedback{
				.Connection = Id,
				.ObservedAtMicroseconds = Stale ? Time - FreshnessUs - 1 : Time,
				.StructuralPayloadBytesFirstSent = Prior + (Point == 0 ? 0 : Point == 1 ? 100'000 : GroupBytes),
				.StructuralPayloadBytesAcked = Prior + (Point == 2 ? GroupBytes : 0),
				.StructuralQualifiedActiveMicroseconds = std::uint64_t(Wave - 1) * 25'000 +
					(Point == 2 ? 25'000 : 0),
				.StructuralMaximumDeficitByteMicroseconds = Slow ? RunningDeficitBound + 1 : 10'000'000'000ULL,
				.StructuralActiveGrantBytes = Active ? GroupBytes : 0,
				.StructuralActiveGrantFirstSentBytes = Point == 1 ? std::uint64_t{100'000} : 0,
				.StructuralActiveGrantStartedAtMicroseconds = Active ? Start : 0,
				.StructuralServiceFailed = Slow,
				.PendingReliableStreamBytes = Active ? GroupBytes - (Point == 1 ? 100'000 : 0) : 0,
				.State = ConnectionState::Connected,
				.StructuralGrantFirstSendAtMicroseconds = Point == 1 ? Start + 6000 : 0,
				.StructuralGrantCompletedAtMicroseconds = Point == 2 ? Start + 31'000 : 0,
				.StructuralCompletedGrantSequence = Point == 2 ? Wave : Wave - 1,
				.StructuralLastCompletedGrantToken = Point == 2 ? Token : 0,
				.StructuralLastCompletedGrantBytes = Point == 2 ? GroupBytes : 0,
				.StructuralLastCompletedGrantActivatedAtMicroseconds = Point == 2 ? Start : 0,
				.StructuralLastCompletedGrantFirstSendAtMicroseconds = Point == 2 ? Start + 6000 : 0,
				.StructuralLastCompletedGrantCompletedAtMicroseconds = Point == 2 ? Start + 31'000 : 0,
				.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds =
					Point == 2 ? (Slow ? RunningDeficitBound + 1 : 10'000'000'000ULL) : 0,
				.StructuralLastCompletedGrantFailed = Point == 2 && Slow};
			R.Result.Valid = true; R.Result.Available = !Stale; R.Result.Qualified = !Slow && !Stale;
			R.Admission.ActiveDrainGrants = Active ? PeerCount : 0;
			if (!Active) { R.Result.RetiredToken = Token; R.Result.RetiredBytes = GroupBytes; }
			Trace::Record(&T, R);
		}
	};
	Trace Good; Good.Demand = 1;
	for (unsigned W = 1; W <= 3; ++W)
		for (unsigned P = 0; P < 3; ++P)
			FeedCurve(Good, (W - 1) * 3 + P + 1, W, P);
	Good.Analyze();
	if (!Good.Passed()) {
		std::cerr << "[Probe:SelfTest] F1 trace overflow=" << Good.Overflow
			<< " invalid=" << Good.Invalid << " floor_failure=" << Good.FloorFailure
			<< " pool_us=" << Good.PoolQualifiedUs << " episodes=" << Good.PoolEpisodes << '\n';
		for (const auto &Peer : Good.Peers)
			std::cerr << "[Probe:SelfTest] peer=" << Peer.Id.Slot << " grants=" << Peer.GrantCount
				<< " qualified=" << Peer.QualifiedGrants << " first=" << Peer.LastFirst
				<< " ack=" << Peer.LastAck << " running_us=" << Peer.LastActiveUs
				<< " max_deficit=" << Peer.MaximumDeficit << '\n';
		for (const auto &Peer : Good.Peers)
			for (std::size_t Index = 0; Index < Peer.GrantCount; ++Index) {
				const auto &Grant = Peer.Grants[Index];
				std::cerr << "[Probe:SelfTest] grant peer=" << Peer.Id.Slot << " token=" << Grant.Token
					<< " bytes=" << Grant.Bytes << " start=" << Grant.StartedAtMicroseconds
					<< " first=" << Grant.FirstSendAtMicroseconds
					<< " complete=" << Grant.CompletedAtMicroseconds
					<< " retired=" << Grant.Retired << '\n';
			}
	}
	Require(Good.Passed() && Good.PoolEpisodes == 3 && Good.PoolQualifiedUs == 75'000,
		"F1 grant turnover and common first-send overlap failed");
	Trace Slow; Slow.Demand = 1; FeedCurve(Slow, 1, 1, 0); FeedCurve(Slow, 2, 1, 1, true);
	Require(Slow.FloorFailure && !Slow.Passed(), "native grant running deficit escaped failure");
	Trace Stale; Stale.Demand = 1; FeedCurve(Stale, 1, 1, 0); FeedCurve(Stale, 2, 1, 1, false, true);
	Require(Stale.Invalid && !Stale.Passed(), "stale campaign feedback escaped failure");
	Trace NoOverlap; NoOverlap.Demand = 1;
	for (unsigned W = 1; W <= 3; ++W)
		for (unsigned P = 0; P < 3; ++P)
			FeedCurve(NoOverlap, (W - 1) * 3 + P + 1, W, P);
	for (std::size_t G = 0; G < NoOverlap.Peers[3].GrantCount; ++G)
		NoOverlap.Peers[3].Grants[G].FirstSendAtMicroseconds += 20'000;
	for (unsigned Peer = 0; Peer < 3; ++Peer)
		for (std::size_t G = 0; G < NoOverlap.Peers[Peer].GrantCount; ++G)
			NoOverlap.Peers[Peer].Grants[G].CompletedAtMicroseconds -= 6000;
	NoOverlap.Analyze();
	Require(!NoOverlap.PoolQualifiedUs && !NoOverlap.Passed(), "ACK/debt overlap counted as pool drain");
	Good.Rows.resize(Trace::Capacity); FeedCurve(Good, 999, 3, 2);
	Require(Good.Overflow, "trace overflow did not fail closed");
	std::cout << "[Probe:SelfTest] pass=1 sockets=0 contract=F1 cases=5\n";
}
}
int main(int Count, char **Args) {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	using namespace physical_probe;
	try {
		if (Count == 2 && std::string_view(Args[1]) == "--self-test") { SelfTest(); return 0; }
		const bool Server = Count > 1 && std::string_view(Args[1]) == "server";
		const bool ReadinessSmoke = Count > 1 && std::string_view(Args[Count - 1]) == "--readiness-smoke";
		Require((Server && ((Count == 5 && !ReadinessSmoke) || (Count == 6 && ReadinessSmoke))) ||
			(!Server && ((Count == 6 && !ReadinessSmoke) || (Count == 7 && ReadinessSmoke)) && std::string_view(Args[1]) == "client"),
			"usage: probe server bindAddress port expectedClients [--readiness-smoke] | probe client serverAddress port nonce producerFlag [--readiness-smoke]");
		const auto Port = Number(Args[3]); Require(Port && Port <= 65535, "invalid port");
		const auto N = Number(Args[4]); Require(Server ? (ReadinessSmoke ? (N == 1 || N == PeerCount) : N == PeerCount) : N > 0,
			"Phase1 requires four clients; readiness smoke requires one or four clients and nonzero client nonces");
		const auto Flag = Server ? 0 : Number(Args[5]); Require(Flag <= 1, "producerFlag must be 0 or 1");
		SDL_SetLogOutputFunction([](void *, int, SDL_LogPriority, const char *Message) { std::cerr << "[Probe:Runtime] " << Message << '\n'; }, nullptr);
		SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN); BootstrapNativeRuntimeSchema();
		const auto P = ReliableServiceProfile::PooledService();
		std::cout << "[Probe:Profile] source_label=" << GARGANTUAN_PROBE_SOURCE << " exact_source=external-manifest mode=POOLED_SERVICE phase=1 connected=4 profile_peers=" << P.MaximumConnections
			<< " group=" << GroupBytes << " backend_cap=" << P.Pooled.BackendCap << " structural_pool=" << P.Pooled.StructuralPool
			<< " gameplay_reserve=" << P.Pooled.GameplayReserve << " control_reserve=" << P.Pooled.ControlRealtimeReserve << " transport_reserve=" << P.Pooled.RequiredTransportReserve
			<< " drain_floor=" << P.Pooled.PeerDrainFloor << " peer_credit=" << P.Pooled.PeerCreditRate << " global_credit=" << P.Pooled.GlobalCreditRate
			<< " grants=" << P.Pooled.MaximumDrainGrants << " freshness_us=" << P.Pooled.FeedbackFreshnessMicroseconds << " requalification_us=" << P.Pooled.RequalificationMicroseconds
			<< " peer_burst=" << P.Pooled.PeerBurstCap << " global_burst=" << P.Pooled.GlobalBurstCap << " peer_pending=" << P.Pooled.PeerPendingCap << " global_pending=" << P.Pooled.GlobalPendingCap
			<< " backend_send_rate=" << P.BackendSendRate() << " scope=" << WaveCount << "-barriered-one-G-waves-per-peer contract=F1 actions=not-exercised\n";
		Probe Run(Server, Flag != 0, Server ? 0 : N, {Args[2], static_cast<std::uint16_t>(Port)}, ReadinessSmoke, Server ? static_cast<unsigned>(N) : PeerCount);
		bool Passed = true;
		try { Run.Run(); } catch (const std::exception &E) { Passed = false; std::cerr << "[Probe:Failure] " << E.what() << '\n'; }
		try { Run.Cleanup(); } catch (const std::exception &E) { Passed = false; std::cerr << "[Probe:CleanupFailure] " << E.what() << '\n'; }
		Run.Report(); std::cout << "[Probe:Result] pass=" << Passed << " scope=Phase1-only\n"; return Passed ? 0 : 1;
	} catch (const std::exception &E) { std::cerr << "[Probe:Failure] " << E.what() << '\n'; return 1; }
}
