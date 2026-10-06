#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Players.hpp"
#include "../src/host/server/FarmF1Evidence.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "../src/network/ReliableServiceFeedback.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;
	std::uint32_t QualificationPeerCount = 1;
	bool QualificationAggregateStructural = false;
	bool QualificationDiagnostic = false;
	bool QualificationPooled = false;
	bool QualificationFeedbackRefresh = false;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	ReliableServiceProfile CandidateReliableService() {
		if (QualificationPooled) return ReliableServiceProfile::PooledService();
		ReliableServiceProfile Profile;
		Profile.ConnectionRate = 8ull * 1024 * 1024;
		Profile.AggregateRate = Profile.ConnectionRate * QualificationPeerCount;
		Profile.BackendRate = 2 * Profile.ConnectionRate;
		Profile.MaximumConnections = QualificationPeerCount;
		Profile.GlobalBacklog = MaximumReliableServiceGroupBytes +
			2 * QualificationPeerCount * Profile.GameplayBurst;
		Profile.RequireLatencyCompatibility = true;
		return Profile;
	}

	GameSessionConfiguration Configuration(GameSessionRole Role, std::uint16_t Port, bool Profiled) {
		GameSessionConfiguration Result{
			.Role = Role,
			.Endpoint = {"127.0.0.1", Port},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 600,
			.ClientNonce = Role == GameSessionRole::Client ? 0x3dfeed1234ull : 0,
		};
		if (Profiled && Role == GameSessionRole::Server) Result.ReliableService = CandidateReliableService();
		// This fixture measures feedback ordering on the offered step, independent
		// of the normal six-tick relevance/catalog refresh cadence.
		if (QualificationFeedbackRefresh && Role == GameSessionRole::Server) Result.Relevance.UpdateIntervalTicks = 1;
		return Result;
	}

	// Delay one real native query, not the timestamp or contents it returns.
	// Only armed after all 32 actual peers and their accepted debt are idle.
	class DelayedFeedbackTransport final : public IGameTransport {
		std::shared_ptr<IGameTransport> Delegate;
		mutable std::size_t Remaining = 0;
		bool Recording = false, Withhold = false;
		bool EnableReliableServiceFeedback() override { return detail::ReliableServiceFeedbackAccess::Enable(*Delegate); }
		bool ReleaseReliableServiceFeedback(ConnectionId Id) override { return detail::ReliableServiceFeedbackAccess::Release(*Delegate, Id); }
		std::optional<detail::ReliableServiceFeedback> ReadReliableServiceFeedback(ConnectionId Id) const override {
			if (!Recording) return detail::ReliableServiceFeedbackAccess::Observe(*Delegate, Id);
			const bool Initial = Remaining != 0;
			if (Initial && --Remaining == 0) {
				std::this_thread::sleep_for(60ms);
				++Delays;
			}
			auto Sample = detail::ReliableServiceFeedbackAccess::Observe(*Delegate, Id);
			if (Initial) {
				InitialQueries.emplace_back(Id, Sample);
			} else {
				++RefreshQueries;
				if (Withhold) return {};
				RefreshedQueries.emplace_back(Id, Sample);
			}
			return Sample;
		}
	  public:
		mutable std::size_t Delays = 0, RefreshQueries = 0;
		std::size_t StructuralSends = 0;
		mutable std::vector<std::pair<ConnectionId, std::optional<detail::ReliableServiceFeedback>>> InitialQueries, RefreshedQueries;
		explicit DelayedFeedbackTransport(std::shared_ptr<IGameTransport> Input) : Delegate(std::move(Input)) {}
		TransportOperationResult Start(const TransportStartConfiguration &Input) override { return Delegate->Start(Input); }
		TransportOperationResult Stop(DisconnectInfo Input) override { return Delegate->Stop(std::move(Input)); }
		TransportOperationResult Disconnect(ConnectionId Id, DisconnectInfo Input) override { return Delegate->Disconnect(Id, std::move(Input)); }
		TransportOperationResult Send(const NetworkMessageIntent &Input) override {
			auto Result = Delegate->Send(Input);
			if (Recording && Result.Succeeded() && detail::ReliableServiceFeedbackAccess::Token(Input)) ++StructuralSends;
			return Result;
		}
		std::size_t PollEvents(std::span<TransportEvent> Output) override { return Delegate->PollEvents(Output); }
		std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Id) const override { return Delegate->GetAvailableDatagramBytes(Id); }
		std::optional<NetworkStatistics> GetStatistics(ConnectionId Id) const override { return Delegate->GetStatistics(Id); }
		void Arm(bool Missing) {
			Remaining = 32; Delays = RefreshQueries = StructuralSends = 0; InitialQueries.clear(); RefreshedQueries.clear();
			Withhold = Missing; Recording = true;
		}
		void Disarm() { Recording = false; }
	};

	void TestPooledFeedbackRefresh(Engine &ServerRuntime, Engine &PrimaryRuntime, GameSession &Server,
		GameSession &Primary, DelayedFeedbackTransport &Transport, std::uint16_t Port, std::uint64_t &Tick) {
		struct Peer {
			std::shared_ptr<GameNetworkingSocketsTransport> Transport;
			std::unique_ptr<GameSession> Session;
			std::unique_ptr<HeadlessRenderer> Renderer;
			std::unique_ptr<Engine> Runtime;
			~Peer() { if (Session) Session->Stop(); if (Runtime) Runtime->Destroy(); }
		};
		std::vector<std::unique_ptr<Peer>> Peers;
		auto Step = [&] {
			(void)Server.Poll(); (void)Primary.Poll();
			PrimaryRuntime.Step(); ServerRuntime.Step();
			Server.Step(Tick); Primary.Step(Tick);
			for (auto &Value : Peers) {
				(void)Value->Session->Poll();
				if (!Value->Runtime && Value->Session->GetClientDataModel()) {
					Value->Renderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
					Value->Runtime = std::make_unique<Engine>(Value->Session->GetClientDataModel(), Value->Renderer.get(),
						std::function<void(std::string, std::string)>{},
						EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
					Value->Runtime->ProcessService->Alive = true;
					Check(Value->Session->AttachClientRuntime(*Value->Runtime), "feedback fixture peer attaches after production bootstrap");
				}
				if (Value->Runtime) Value->Runtime->Step();
				Value->Session->Step(Tick);
			}
			++Tick;
			std::this_thread::sleep_for(1ms);
		};
		const auto SetupDeadline = std::chrono::steady_clock::now() + 20s;
		for (std::uint32_t Index = 1; Index < 32; ++Index) {
			auto Value = std::make_unique<Peer>();
			Value->Transport = std::make_shared<GameNetworkingSocketsTransport>();
			auto Config = Configuration(GameSessionRole::Client, Port, false);
			Config.ClientNonce += Index;
			Value->Session = std::make_unique<GameSession>(Value->Transport, Config);
			Check(Value->Session->Start().Succeeded(), "feedback fixture actual GNS peer starts");
			Peers.push_back(std::move(Value));
			while (Server.GetMetrics().ReadyPeers != Index + 1 && std::chrono::steady_clock::now() < SetupDeadline) Step();
			Check(Server.GetMetrics().ReadyPeers == Index + 1, "feedback fixture reaches each actual Ready peer within its bound");
			if (Failures) return;
		}
		auto Idle = [&] {
			const auto M = Server.GetMetrics();
			return M.ReadyPeers == 32 && M.JournalBacklogRecords == 0 && M.MaterializationBacklog == 0 &&
				M.ReliableAdmission.AcceptedBytes == M.ReliableAdmission.VerifiedAttributedRetirement &&
				M.ReliableAdmission.OutstandingBytes == 0 && M.ReliableAdmission.ActiveDrainGrants == 0 &&
				M.ReliableAdmission.TerminalReleasedBytes == 0;
		};
		auto Drain = [&] {
			const auto Deadline = std::chrono::steady_clock::now() + 5s;
			do { Step(); } while (!Idle() && std::chrono::steady_clock::now() < Deadline);
			Check(Idle() && Server.GetStatus() == GameSessionStatus::Listening,
				"feedback fixture converges exact native ACK/retirement and source debt without terminal release");
		};
		Drain();
		if (Failures) return;
		auto Floor = ServerRuntime.DataModel->FindFirstChild("QualificationFloor", true);
		Check(Floor && PrimaryRuntime.DataModel->FindFirstChild("QualificationFloor", true),
			"feedback work targets the existing server and primary materialized Floor");
		for (const auto &Value : Peers)
			Check(Value->Runtime && Value->Runtime->DataModel->FindFirstChild("QualificationFloor", true),
				"feedback work target is already materialized for every actual peer");
		if (Failures || !Floor) return;
		for (const bool Missing : {false, true}) {
			const auto BeforeCursor = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId());
			Check(Floor->ApplyAttributeMutation("FeedbackRefreshRegression", WireValue(Missing ? 2 : 1),
				ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success,
				"feedback regression offers one legitimate tiny current-state attribute to all peers");
			const auto Committed = ChangeJournal::Get().Read(BeforeCursor, 1);
			Check(Committed.Records.size() == 1 && Committed.Records.front().Object == Floor->GetObjectId() &&
				std::holds_alternative<AttributeUpdatedChange>(Committed.Records.front().Payload),
				"feedback work is an actual committed attribute on the materialized source object");
			(void)Server.Poll();
			ServerRuntime.Step();
			const auto WorkTail = ChangeJournal::Get().CreateCursor(ServerRuntime.DataModel->GetObjectId()).NextSequence;
			std::size_t BackloggedPeers = 0;
			for (const auto &Reader : detail::GameSessionTestAccess::GetJournalRequirements(Server)) {
				if (Reader.Catalog) continue;
				Check(Reader.Cursor.Scope == BeforeCursor.Scope && Reader.Cursor.NextSequence < WorkTail,
					"each actual structural peer cursor is behind committed work before the delayed query sweep");
				if (Reader.Cursor.Scope == BeforeCursor.Scope && Reader.Cursor.NextSequence < WorkTail) ++BackloggedPeers;
			}
			Check(BackloggedPeers == 32, "all 32 actual source readers require structural progress before eligibility");
			if (Failures) return;
			const auto Before = Server.GetMetrics().ReliableAdmission;
			Transport.Arm(Missing);
			Server.Step(Tick++);
			Transport.Disarm();
			Check(Server.GetStatus() == GameSessionStatus::Listening && Server.GetMetrics().ReadyPeers == 32,
				"both delayed-query phases preserve the healthy actual 32-peer server");
			const auto After = Server.GetMetrics().ReliableAdmission;
			const auto At = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
			std::size_t OldSamples = 0;
			std::set<ConnectionId> InitialPeers;
			for (const auto &[Id, Sample] : Transport.InitialQueries) {
				Check(Sample && Sample->Connection == Id && Sample->CountersValid &&
					Sample->State == ConnectionState::Connected && Sample->ObservedAtMicroseconds <= At,
					"feedback regression retains unchanged generation-valid real native snapshots");
				InitialPeers.insert(Id);
				if (Sample && At - Sample->ObservedAtMicroseconds > 50'000) ++OldSamples;
			}
			Check(Transport.Delays == 1 && Transport.InitialQueries.size() == 32 && InitialPeers.size() == 32 && OldSamples >= 31,
				"one bounded native query delay ages at least the first 31 envelope snapshots");
			for (const auto &[Id, Sample] : Transport.RefreshedQueries) {
				const auto Prior = std::ranges::find_if(Transport.InitialQueries, [&](const auto &Row) { return Row.first == Id; });
				Check(Sample && Prior != Transport.InitialQueries.end() && Prior->second &&
					Sample->ObservedAtMicroseconds > Prior->second->ObservedAtMicroseconds,
					"eligibility refresh reads a later native snapshot instead of retimestamping cached data");
			}
			const auto Grants = Transport.StructuralSends;
			std::cout << "[Network:PooledFeedbackRefresh] unavailable=" << Missing << " initial_old=" << OldSamples
				<< " refresh_queries=" << Transport.RefreshQueries << " accepted_grants=" << Grants
				<< " feedback_deferrals=" << After.FeedbackDeferrals - Before.FeedbackDeferrals << '\n';
			Check(Transport.RefreshQueries >= 31, "actual GameSession refreshes stale evidence before first eligibility");
			Check(Missing ? (Grants <= 1 && After.FeedbackDeferrals > Before.FeedbackDeferrals) :
				(Grants >= 2 && Grants <= 4 && After.AcceptedBytes > Before.AcceptedBytes &&
				 After.ActiveDrainGrants >= 2 && After.ActiveDrainGrants <= 4),
				"native refresh restores real eligible service while unavailable feedback remains denied by unchanged admission");
			Drain();
			if (Failures) return;
		}
		const auto Native = Server.GetMetrics();
		Check(Native.StructuralAcceptedFeedbackBytes == Native.StructuralFirstSentFeedbackBytes &&
			Native.StructuralFirstSentFeedbackBytes == Native.StructuralAckedFeedbackBytes &&
			Native.StructuralAckedFeedbackBytes == Native.ReliableAdmission.VerifiedAttributedRetirement,
			"feedback ordering preserves unique native first-send, ACK and exact retirement conservation");
	}
}

#include "ReliableGameplayWorkloadFixture.hpp"

int main(int ArgumentCount, char **Arguments) {
	using namespace gargantuan;
	using namespace gargantuan::network;
	if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--pooled") {
		QualificationPooled = true; --ArgumentCount; ++Arguments;
	}
	const bool FeedbackRefresh = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--pooled-feedback-refresh";
	QualificationFeedbackRefresh = FeedbackRefresh;
	if (FeedbackRefresh) { QualificationPooled = true; QualificationPeerCount = 32; }
	QualificationDiagnostic = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--ki008-attribution";
	QualificationAggregateStructural = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--reliable-workload-32-structural";
	QualificationAggregateStructural = QualificationAggregateStructural || QualificationDiagnostic;
	const bool Aggregate = QualificationAggregateStructural || (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--reliable-workload-32");
	if (Aggregate) QualificationPeerCount = 32;
	if (QualificationDiagnostic) QualificationPeerCount = DiagnosticDimension("KI008_PEERS", 32, 1, 32);
	const bool Workload = Aggregate || (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--reliable-workload");
	const bool Profiled = FeedbackRefresh || Workload || (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--reliable-profile");
	if (ArgumentCount > 1 && !Profiled) {
		std::cerr << "usage: gargantuan_game_session_real_transport_tests [--pooled] [--reliable-profile|--reliable-workload|--reliable-workload-32|--reliable-workload-32-structural|--ki008-attribution|--pooled-feedback-refresh]\n";
		return 2;
	}
	if (Profiled) {
		SDL_SetLogOutputFunction([](void *, int, SDL_LogPriority, const char *Message) {
			std::cerr << Message << '\n';
		}, nullptr);
		SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
		const auto Profile = CandidateReliableService();
		Check(Profile.IsValid() && Profile.IsLatencyCompatible(),
			"profiled real GNS fixture uses the approved capacity-compatible candidate");
	}
	gargantuan::BootstrapNativeRuntimeSchema();

	auto ServerWorld = std::make_shared<DataModel>();
	auto QualificationFunction = std::make_shared<RemoteFunction>();
	auto QualificationEvent = std::make_shared<RemoteEvent>();
	{
		auto Floor = std::make_shared<Part>();
		Floor->SetName("QualificationFloor");
		Floor->SetAnchored(true);
		Floor->SetSize({1024.0f, 1.0f, 1024.0f});
		Floor->SetPosition({0.0f, -1.0f, 0.0f});
		Floor->SetParent(ServerWorld->GetService("Workspace"));
	}
	if (Workload) {
		QualificationFunction->SetName("QualificationFunction");
		QualificationFunction->SetParent(ServerWorld);
		QualificationEvent->SetName("QualificationEvent");
		QualificationEvent->SetParent(ServerWorld);
	}
	DiskFilesystem SampleFilesystem(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT));
	auto ServerAssets = std::dynamic_pointer_cast<AssetService>(ServerWorld->GetService("AssetService"));
	ServerAssets->LoadProjectAssets(SampleFilesystem);
	const auto RuntimeAssets = ServerAssets->CaptureRuntimeAssets();
	auto ServerActionPolicy = std::make_shared<Script>();
	ServerActionPolicy->SetName("GnsActionPolicy");
	ServerActionPolicy->SetRunContext(Enums::RunContext::Server);
	ServerActionPolicy->SetSource(R"(
local CharacterControl = game:GetService("CharacterControlService")
assert(CharacterControl:RegisterAction(
	"GnsLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetActionPolicy(function(Player, Character, ActionName)
	local Accepted = ActionName == "GnsLunge" and Player.Character == Character
	if Accepted then
		Character:SetAttribute("GnsActionAuthorized", true)
	end
	return Accepted
end)
)");
	ServerActionPolicy->SetParent(ServerWorld);
	auto ClientActionPolicy = std::make_shared<Script>();
	ClientActionPolicy->SetName("GnsActionRequest");
	ClientActionPolicy->SetRunContext(Enums::RunContext::Client);
	ClientActionPolicy->SetSource(R"(
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
assert(CharacterControl:RegisterAction(
	"GnsLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if ActionName == "GnsLunge" and Accepted then
		Character:SetAttribute("GnsActionResolved", true)
	end
end)
local Requested = false
RunService.PreSimulation:Connect(function()
	if not Requested and Players.LocalPlayer and Players.LocalPlayer.Character then
		Requested = CharacterControl:RequestAction("GnsLunge")
	end
end)
)");
	ClientActionPolicy->SetParent(ServerWorld);
	if (Workload) ClientActionPolicy->SetSource(ClientActionPolicy->GetSource() + R"(
local WorkloadStarted = 0
local WorkloadSequence = 0
CharacterControl.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if WorkloadSequence > 0 and ActionName == "GnsLunge" then
		CharacterControl:SetAttribute("WorkloadResolved", WorkloadSequence)
		CharacterControl:SetAttribute("WorkloadAccepted", Accepted)
		WorkloadSequence = 0
	end
end)
RunService.PreSimulation:Connect(function()
	local Sequence = CharacterControl:GetAttribute("WorkloadRequest") or 0
	if Sequence > WorkloadStarted and WorkloadSequence == 0 then
		WorkloadStarted = Sequence
		WorkloadSequence = Sequence
		if not CharacterControl:RequestAction("GnsLunge") then
			CharacterControl:SetAttribute("WorkloadRejected", Sequence)
			WorkloadSequence = 0
		end
	end
end)
)");
	auto RelevanceNpc = std::make_shared<KinematicCharacter>();
	RelevanceNpc->SetName("GnsRelevanceNpc");
	auto RelevanceRoot = std::make_shared<Part>();
	RelevanceRoot->SetName("GnsRelevanceRoot");
	RelevanceRoot->SetParent(RelevanceNpc);
	RelevanceNpc->SetRootPart(RelevanceRoot);
	RelevanceNpc->SetPosition({5'000.0f, 6.0f, 0.0f});
	RelevanceNpc->SetParent(ServerWorld);
	HeadlessRenderer ServerRenderer(Vector2(320, 240));
	Engine ServerRuntime(
		ServerWorld,
		&ServerRenderer,
		nullptr,
		EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
	);
	ServerRuntime.ProcessService->Alive = true;

	std::shared_ptr<GameNetworkingSocketsTransport> ServerTransport;
	std::shared_ptr<DelayedFeedbackTransport> FeedbackTransport;
	std::unique_ptr<host::detail::FarmF1Evidence> FiniteEvidence;
	if (QualificationPooled) FiniteEvidence = std::make_unique<host::detail::FarmF1Evidence>();
	std::unique_ptr<GameSession> Server;
	std::uint16_t Port = 0;
	for (std::uint32_t Candidate = 39400; Candidate < 39500; ++Candidate) {
		GameNetworkingSocketsTransportConfiguration TransportConfiguration;
		if (Profiled) {
			const auto Profile = CandidateReliableService();
			TransportConfiguration.MaximumConnections = Profile.MaximumConnections;
			TransportConfiguration.SendRate = static_cast<std::uint32_t>(Profile.BackendSendRate());
		}
		auto CandidateTransport = std::make_shared<GameNetworkingSocketsTransport>(TransportConfiguration);
		std::shared_ptr<IGameTransport> SessionTransport = CandidateTransport;
		if (FeedbackRefresh) {
			FeedbackTransport = std::make_shared<DelayedFeedbackTransport>(CandidateTransport);
			SessionTransport = FeedbackTransport;
		}
		auto CandidateSession = std::make_unique<GameSession>(
			SessionTransport,
			Configuration(GameSessionRole::Server, static_cast<std::uint16_t>(Candidate), Profiled),
			&ServerRuntime
		);
		if (!CandidateSession->Start().Succeeded()) continue;
		Port = static_cast<std::uint16_t>(Candidate);
		ServerTransport = std::move(CandidateTransport);
		Server = std::move(CandidateSession);
		break;
	}
	Check(Port != 0 && Server, "real GNS GameSession server binds a bounded loopback port");
	if (!Server) return 1;

	auto ClientTransport = std::make_shared<GameNetworkingSocketsTransport>();
	auto ClientConfiguration = Configuration(GameSessionRole::Client, Port, false);
	ClientConfiguration.ClientNonce = 0x32f1a69u;
	GameSession Client(ClientTransport, ClientConfiguration);
	Check(Client.Start().Succeeded(), "real GNS GameSession client starts");

	std::unique_ptr<HeadlessRenderer> ClientRenderer;
	std::unique_ptr<Engine> ClientRuntime;
	std::uint64_t Tick = 1;
	const auto ReadyDeadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < ReadyDeadline && Server->GetMetrics().ReadyPeers != 1) {
		(void)Server->Poll();
		(void)Client.Poll();
		if (!ClientRuntime && Client.GetClientDataModel()) {
			auto ClientAssets = std::dynamic_pointer_cast<AssetService>(
				Client.GetClientDataModel()->GetService("AssetService")
			);
			ClientAssets->LoadRuntimeAssetSnapshot(RuntimeAssets);
			Check(
				PackageBuilder::HydrateClientCode(ServerWorld, Client.GetClientDataModel()) >= 1,
				"real GNS client hydrates trusted packaged client policy"
			);
			ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
			ClientRuntime = std::make_unique<Engine>(
				Client.GetClientDataModel(),
				ClientRenderer.get(),
				std::function<void(std::string, std::string)>{},
				EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
			);
			ClientRuntime->ProcessService->Alive = true;
			Check(Client.AttachClientRuntime(*ClientRuntime), "real GNS client attaches after trusted bootstrap");
		}
		if (ClientRuntime) ClientRuntime->Step();
		ServerRuntime.Step();
		Server->Step(Tick);
		Client.Step(Tick);
		++Tick;
		std::this_thread::sleep_for(1ms);
	}

	Check(
		Server->GetMetrics().ReadyPeers == 1 && Client.GetStatus() == GameSessionStatus::Ready,
		"real GNS completes accepted peer, trusted LocalPlayer, and gameplay-ready phases"
	);
	const auto PeerIdentities = Server->GetPeerIdentities();
	if (FiniteEvidence && !Workload) {
		// Real GameSession bootstrap uses production admission and pinned GNS,
		// unlike a synthetic completed-certificate fixture. Retain exact bootstrap
		// debt while deliberately excluding its sentinel activation from F1.
		const auto Deadline = std::chrono::steady_clock::now() + 2s;
		while (std::chrono::steady_clock::now() < Deadline && Server->GetMetrics().ReliableAdmission.OutstandingBytes) {
			(void)Server->Poll(); (void)Client.Poll();
			Server->Step(Tick); Client.Step(Tick++); std::this_thread::sleep_for(1ms);
		}
		const auto Observations = FiniteEvidence->Observations();
		Check(Observations.size() == 1 && Observations[0].BootstrapAccepted > 0 &&
			Observations[0].BootstrapBytes > 0 && Observations[0].BootstrapBytes == Observations[0].BootstrapRetired &&
			Observations[0].BootstrapBytes + Observations[0].QualifiedBytes == Observations[0].Accepted &&
			Observations[0].Accepted == Observations[0].FirstSent && Observations[0].FirstSent == Observations[0].Acked &&
			Observations[0].Acked == Observations[0].Retired && !Observations[0].PendingToken && !Observations[0].Failed,
			"real pooled GameSession bootstrap keeps exact admission/send/ACK/retirement outside qualified F1 certificates");
	}
	Check(PeerIdentities.size() == 1 && PeerIdentities.front().Ready &&
		PeerIdentities.front().Nonce == ClientConfiguration.ClientNonce &&
		PeerIdentities.front().PlayerId != 0 && PeerIdentities.front().SessionEpoch != 0,
		"real GNS exposes the run-scoped client nonce and accepted ready Player identity"
	);
	if (Profiled) {
		const auto Metrics = Server->GetMetrics();
		Check(Metrics.ReliableAdmissionPeerStates == 1,
			"profiled real GNS session owns one generation-scoped byte-admission peer state");
		Check(Metrics.ReliableAdmission.AcceptedBytes > 0,
			"profiled real GNS session admits structural bytes through the production accountant");
		Check(Metrics.ReliableAdmission.PeerCreditHighWater > 0 &&
			Metrics.ReliableAdmission.GlobalCreditHighWater > 0,
			"profiled real GNS session exercises finite peer and global elapsed-time credit");
		// Whether this real-time bootstrap happens to defer is timing-dependent:
		// service time accrues finite credit while the connection/bootstrap work runs.
		// Deterministic zero-credit and exact-size deferral behavior is covered by
		// ReliableByteAdmissionFixture and the production-admission matrix.
	}
	auto ServerPlayers = ServerRuntime.Players->GetPlayers();
	Check(
		ServerPlayers.size() == 1 && ServerPlayers.front()->GetCharacter().has_value(),
		"real GNS server owns Player and Character creation"
	);
	Check(
		ClientRuntime && ClientRuntime->Players->GetLocalPlayer().has_value(),
		"real GNS client resolves its exact trusted LocalPlayer ObjectId"
	);
	if (FeedbackRefresh && ClientRuntime && FeedbackTransport && !Failures) {
		TestPooledFeedbackRefresh(ServerRuntime, *ClientRuntime, *Server, Client, *FeedbackTransport, Port, Tick);
		Client.Stop(); Server->Stop();
		ClientRuntime->Destroy(); ServerRuntime.Destroy();
		return Failures == 0 ? 0 : 1;
	}

	if (ClientRuntime && !ServerPlayers.empty() && ServerPlayers.front()->GetCharacter()) {
		if (Workload) {
			if (Aggregate) RunAggregateReliableGameplay(ServerRuntime, *ClientRuntime, *Server, Client, Port,
				Tick, QualificationFunction, QualificationEvent);
			else RunReliableGameplayWorkload(ServerRuntime, *ClientRuntime, *Server, Client,
				*ClientTransport, Tick, *ServerTransport, QualificationFunction, QualificationEvent);
			Client.Stop();
			Server->Stop();
			Check(Server->GetMetrics().ReliableAdmissionPeerStates == 0,
				"qualification shutdown releases admission peer state");
			if (QualificationPooled) {
				const auto M = Server->GetMetrics().ReliableAdmission;
				Check(M.AcceptedBytes == M.VerifiedAttributedRetirement + M.TerminalReleasedBytes + M.OutstandingBytes &&
					!M.OutstandingBytes && !M.ActiveDrainGrants && M.VerifiedAttributedRetirement,
					"pooled workload shutdown conserves exact structural debt and clears grants");
				std::cout << "[Network:PooledGns] created=" << M.AcceptedBytes << " retired=" << M.VerifiedAttributedRetirement
					<< " terminal=" << M.TerminalReleasedBytes << " outstanding=" << M.OutstandingBytes
					<< " grants_high=" << M.DrainGrantsHighWater << " debt_high=" << M.OutstandingHighWater << '\n';
			}
			ClientRuntime->Destroy();
			ServerRuntime.Destroy();
			return Failures == 0 ? 0 : 1;
		}
		auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*ServerPlayers.front()->GetCharacter());
		auto FindClientRelevanceNpc = [&]() {
			return ClientRuntime ? std::dynamic_pointer_cast<KinematicCharacter>(
							   ClientRuntime->DataModel->FindFirstChild("GnsRelevanceNpc", true)
						   )
						 : nullptr;
		};
		auto StepNetwork = [&]() {
			ClientRuntime->Step();
			ServerRuntime.Step();
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			std::this_thread::sleep_for(1ms);
		};
		const auto InitialPosition = CharacterValue ? CharacterValue->GetPosition() : glm::vec3{};
		(void)ClientRuntime->ProcessEvent(
			KeyEvent{
				.Device = {1},
				.Physical = PhysicalKey::W,
				.Logical = LogicalKey::W,
				.State = ButtonState::Pressed,
			}
		);
		const auto MovementDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < MovementDeadline &&
			   (!CharacterValue || glm::distance(CharacterValue->GetPosition(), InitialPosition) <= 0.25f)) {
			ClientRuntime->Step();
			ServerRuntime.Step();
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			std::this_thread::sleep_for(1ms);
		}
		Check(
			CharacterValue && glm::distance(CharacterValue->GetPosition(), InitialPosition) > 0.25f,
			"real GNS carries ordinary Luau semantic input to authoritative Character movement"
		);
		(void)ClientRuntime->ProcessEvent(KeyEvent{
			.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W, .State = ButtonState::Released,
		});

		const auto BeforeAction = CharacterValue ? CharacterValue->GetPosition() : glm::vec3{};
		const auto ActionDeadline = std::chrono::steady_clock::now() + 10s;
		bool ActionResolved = false;
		while (std::chrono::steady_clock::now() < ActionDeadline &&
			   (!ActionResolved || !CharacterValue || CharacterValue->GetPosition().x <= BeforeAction.x + 0.4f)) {
			ClientRuntime->Step();
			ServerRuntime.Step();
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			if (auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				LocalPlayer && (*LocalPlayer)->GetCharacter())
				ActionResolved =
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("GnsActionResolved").has_value();
			std::this_thread::sleep_for(1ms);
		}
		Check(
			CharacterValue && CharacterValue->GetAttributeValue("GnsActionAuthorized").has_value(),
			"real GNS action reaches generic server Luau authorization"
		);
		Check(ActionResolved, "real GNS authoritative action state resolves to client Luau");
		Check(
			CharacterValue && CharacterValue->GetPosition().x > BeforeAction.x + 0.4f,
			"real GNS action state applies server-owned pinned root motion"
		);

		Check(!FindClientRelevanceNpc(), "real GNS initial materialization excludes a distant NPC");
		const auto FirstNpcTarget = CharacterValue->GetPosition() + glm::vec3(96.0f, 0.0f, 0.0f);
		RelevanceNpc->SetPosition(FirstNpcTarget);
		const auto FirstNpcDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < FirstNpcDeadline && !FindClientRelevanceNpc())
			StepNetwork();
		auto FirstNpcReplica = FindClientRelevanceNpc();
		if (QualificationPooled) {
			const auto M = Server->GetMetrics();
			std::cout << "[Network:PooledNpc] present=" << bool(FirstNpcReplica)
				<< " root=" << bool(FirstNpcReplica && FirstNpcReplica->GetRootPart())
				<< " target_distance=" << (FirstNpcReplica ? glm::distance(FirstNpcReplica->GetPosition(), FirstNpcTarget) : -1.0f)
				<< " authoritative_distance=" << (FirstNpcReplica ? glm::distance(FirstNpcReplica->GetPosition(), RelevanceNpc->GetPosition()) : -1.0f)
				<< " peer_distance=" << glm::distance(CharacterValue->GetPosition(), RelevanceNpc->GetPosition())
				<< " peer_y=" << CharacterValue->GetPosition().y << " npc_y=" << RelevanceNpc->GetPosition().y
				<< " outstanding=" << M.ReliableAdmission.OutstandingBytes << " grants=" << M.ReliableAdmission.ActiveDrainGrants
				<< " probes=" << M.ReliableAdmission.QualificationGrants << " pending_enters=" << M.StructuralPendingEnters << '\n';
		}
		Check(
			FirstNpcReplica && FirstNpcReplica->GetRootPart() &&
				glm::distance(FirstNpcReplica->GetPosition(), FirstNpcTarget) < 1.0f,
			"real GNS materializes an entering NPC with its RootPart and current authoritative transform"
		);

		RelevanceNpc->SetPosition({6'000.0f, 6.0f, 0.0f});
		const auto NpcLeaveDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < NpcLeaveDeadline && FindClientRelevanceNpc()) StepNetwork();
		Check(
			!FindClientRelevanceNpc() && !RelevanceNpc->GetDestroyed(),
			"real GNS peer unpublish removes the NPC replica without destroying server authority"
		);
		const auto ReentryNpcTarget = CharacterValue->GetPosition() + glm::vec3(128.0f, 0.0f, 0.0f);
		RelevanceNpc->SetPosition(ReentryNpcTarget);
		const auto NpcReentryDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < NpcReentryDeadline && !FindClientRelevanceNpc()) StepNetwork();
		auto ReenteredNpcReplica = FindClientRelevanceNpc();
		Check(
			ReenteredNpcReplica && ReenteredNpcReplica->GetRootPart() &&
				glm::distance(ReenteredNpcReplica->GetPosition(), ReentryNpcTarget) < 1.0f,
			"real GNS reentry uses the NPC's current state rather than replaying off-interest motion"
		);

		const auto ClientConnection = Client.GetPrimaryConnection();
		Check(ClientConnection.has_value(), "real GNS client retains its session connection identity");
		if (ClientConnection)
			(void)ClientTransport->Disconnect(
				*ClientConnection, {DisconnectReason::LocalShutdown, "real GNS lifecycle test disconnect"}
			);
		const auto DisconnectDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < DisconnectDeadline && !ServerRuntime.Players->GetPlayers().empty()) {
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			std::this_thread::sleep_for(1ms);
		}
		Check(
			ServerRuntime.Players->GetPlayers().empty() && Server->GetMetrics().PlayersRemoved == 1,
			"real GNS disconnect revokes control and removes the authoritative Player"
		);
		Check(
			CharacterValue && CharacterValue->GetDestroyed(), "real GNS disconnect destroys default Character policy"
		);
		Check(
			Client.GetStatus() == GameSessionStatus::Failed && !ClientRuntime->Players->GetLocalPlayer().has_value(),
			"real GNS hard disconnect stops client control and clears trusted LocalPlayer"
		);
		if (Profiled)
			Check(Server->GetMetrics().ReliableAdmissionPeerStates == 0,
				"profiled real GNS disconnect releases generation-scoped byte-admission state");
	}

	Client.Stop();
	Server->Stop();
	if (QualificationPooled) {
		const auto M = Server->GetMetrics().ReliableAdmission;
		Check(M.AcceptedBytes == M.VerifiedAttributedRetirement + M.TerminalReleasedBytes &&
			!M.OutstandingBytes && !M.ActiveDrainGrants && !Server->GetMetrics().ReliableAdmissionPeerStates && M.VerifiedAttributedRetirement,
			"pooled lifecycle conserves debt and clears every generation owner");
		std::cout << "[Network:PooledGns] created=" << M.AcceptedBytes << " retired=" << M.VerifiedAttributedRetirement
			<< " terminal=" << M.TerminalReleasedBytes << " outstanding=" << M.OutstandingBytes << '\n';
	}
	if (ClientRuntime) ClientRuntime->Destroy();
	ServerRuntime.Destroy();
	if (Failures == 0)
		std::cout << (Profiled ? "Profiled real GNS game-session lifecycle tests passed\n"
							   : "Real GNS game-session lifecycle tests passed\n");
	return Failures == 0 ? 0 : 1;
}
