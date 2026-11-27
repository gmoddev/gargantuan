// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "../../src/network/BlockGridPeer.hpp"
#include "RegionalGridFixture.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/network/BlockGridStream.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace gargantuan;
using namespace gargantuan::network;
using namespace std::chrono_literals;

namespace {
	void Require(bool Value, const char *Description) {
		if (!Value) throw std::runtime_error(Description);
	}
	std::shared_ptr<BlockGrid> Find(DataModel &World, std::string_view Name) {
		for (const auto &Node : World.GetDescendants())
			if (Node->GetName() == Name) return std::dynamic_pointer_cast<BlockGrid>(Node);
		return {};
	}
	std::shared_ptr<BlockGrid> Seed(std::shared_ptr<Instance> Parent, std::string Name, std::uint16_t Material) {
		auto Grid = std::make_shared<BlockGrid>();
		Grid->SetName(Name);
		Grid->SetCellSize(2);
		blockgrid::Field Field;
		const std::array Writes{blockgrid::Write{blockgrid::SingleCell({-3, 0, 1}), {Material, 0}}};
		Require(Field.Apply(Writes), "authoritative placement failed");
		Require(
			Field.DefineChannel({.Name = "private_score", .Kind = blockgrid::ChannelKind::UInt8, .Persistent = true}),
			"private channel"
		);
		Require(
			Field.DefineChannel({.Name = "powered", .Kind = blockgrid::ChannelKind::Bool, .RelevantPeers = true}),
			"public channel"
		);
		Grid->RestoreField(std::move(Field));
		Grid->SetParent(Parent);
		return Grid;
	}
	void CheckInstallationRaces() {
		auto Author = std::make_shared<DataModel>();
		auto Source = Seed(Author->GetService("Workspace"), "Placed", blockgrid::Stone);
		const auto Id = Source->GetObjectId();
		ReplicationCoordinator Coordinator(Author);
		const auto Baseline = Coordinator.AddPeer({1, 1}, ReplicationEpoch(1));
		ReplicaApplier Replica;
		Require(Baseline.Succeeded() && Replica.ApplyFrame(*Baseline.Frame).Succeeded(), "structural baseline failed");
		HeadlessRenderer View(Vector2(64,64));
		Engine ReplicaRuntime(std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot()), &View, nullptr,
			{.Mode = RuntimeMode::Offline, .AdmitSpatialContent = true});
		auto Shell = std::dynamic_pointer_cast<BlockGrid>(Replica.Resolve(Id));
		Require(Shell && Shell->IsReplicaMaterialization(), "missing replicated shell");
		Shell->SetCellSize(1);
		BlockGridPeer Out(true, 37, {}), In(false, 37, {});
		auto ResolveSource = [&](ObjectId Object) { return Object == Id ? Source : nullptr; };
		auto ResolveShell = [&](ObjectId Object) { return Object == Id ? Shell : nullptr; };
		std::vector<std::byte> Packet;
		auto Send = [&](const GridStreamMessage &M) {
			Packet = EncodeGridStream(M);
			return true;
		};
		auto Read = [&]() {
			auto M = DecodeGridStream(Packet);
			Require(M.has_value(), "failed to decode test packet");
			return *M;
		};
		In.Track(Id);
		In.Pump(ResolveShell, Send, 1);
		Out.Receive(Read(), ResolveSource, 1);
		Out.Pump(ResolveSource, Send, 2);
		auto Offer = Read();
		Require(Offer.Kind == GridStreamKind::Offer, "expected first offer");
		In.Receive(Offer, ResolveShell, 2);
		Out.Pump(ResolveSource, Send, 3);
		In.Receive(Read(), ResolveShell, 3);
		Packet.clear();
		In.Pump(ResolveShell, Send, 4);
		Require(Packet.empty() && !Shell->HasReplicaField(), "installed field at the wrong cell size");
		Source->SetCanQuery(false);
		Out.Pump(ResolveSource, Send, 5);
		Require(Read().Kind == GridStreamKind::Forget, "stale placement did not cancel");
		In.Receive(Read(), ResolveShell, 5);
		Offer.Kind = GridStreamKind::Installed;
		Out.Receive(Offer, ResolveSource, 5);
		Require(!Out.IsInstalled(Id, *Source), "late acknowledgement admitted stale placement");
		Shell->SetCellSize(2);
		Shell->SetCanQuery(false);
		Out.Pump(ResolveSource, Send, 6);
		In.Receive(Read(), ResolveShell, 6);
		Out.Pump(ResolveSource, Send, 7);
		In.Receive(Read(), ResolveShell, 7);
		In.Pump(ResolveShell, Send, 8);
		auto Ack = Read();
		if (Ack.Kind != GridStreamKind::Installed || !Shell->HasReplicaField()) {
			const auto Owner = WorldRoot::GetEffectiveWorld(*Shell);
			const auto Position = Shell->GetCFrame().Position;
			const std::array Required{Shell->GetObjectId()};
			const auto Proof = Owner ? Owner->PrepareSpatialDestination({Position,Position}, Required)
				: std::expected<SpatialDestinationReceipt,PhysicsOperationStatus>(std::unexpected(PhysicsOperationStatus::InvalidId));
			std::cerr << "[BlockGrid:Installation] Field=" << Shell->HasReplicaField() << " Frame=" << int(Ack.Kind)
				<< " HasWorld=" << bool(Owner) << " HasTerrain=" << bool(Owner && Owner->GetOwnedTerrain())
				<< " NativeStatus=" << int(Proof ? PhysicsOperationStatus::Success : Proof.error()) << '\n';
		}
		Require(Ack.Kind == GridStreamKind::Installed && Shell->HasReplicaField(), "fresh field did not install");
		auto Forged = Ack;
		Forged.Placement.Bytes[0] ^= 1;
		bool Rejected = false;
		try {
			Out.Receive(Forged, ResolveSource, 8);
		} catch (const std::invalid_argument &) {
			Rejected = true;
		}
		Require(Rejected && !Out.IsInstalled(Id, *Source), "changed placement digest was accepted");
		Forged=Ack;
		++Forged.Membership;
		Rejected=false;
		try { Out.Receive(Forged,ResolveSource,8); }
		catch (const std::invalid_argument&) { Rejected=true; }
		Require(Rejected && !Out.IsInstalled(Id,*Source),"changed native membership was accepted");
		Out.Receive(Ack, ResolveSource, 8);
		Require(Out.IsInstalled(Id, *Source), "verified current placement did not open");
		Source->SetCanCollide(false);
		Require(!Out.IsInstalled(Id, *Source), "changing only collision retained an old receipt");
		Out.Pump(ResolveSource, Send, 9);
		auto Updated = Read();
		Require(Updated.Revision == Ack.Revision && Updated.Prefix == Updated.Size, "stationary field bytes resent");
		In.Receive(Updated, ResolveShell, 9);
		Shell->SetCanCollide(false);
		In.Pump(ResolveShell, Send, 10);
		Out.Receive(Read(), ResolveSource, 10);
		Require(Out.IsInstalled(Id, *Source), "collision-only update did not install");
		bool Removed{};
		const auto RemoveAtPose=Shell->GetPropertyChangedSignal("CFrame")->Once([&](auto) {
			Removed=true;
			In.Forget(Id);
		});
		Source->SetCFrame(CFrame(4,2,0));
		Out.Pump(ResolveSource,Send,11);
		In.Receive(Read(),ResolveShell,11);
		Packet.clear();
		In.Pump(ResolveShell,Send,12);
		Require(Removed && Packet.empty() && !Shell->HasReplicaField() && !Shell->GetAssemblyProjection(),
			"retiring a moving shell from its pose callback retained content or acknowledged its installation");
		RemoveAtPose->Disconnect();
		Out.Forget(Id);
		Require(!Out.IsInstalled(Id, *Source), "retirement retained the installation");
		ReplicaRuntime.Destroy();
	}
#include "PairedGridStream.inl"
#include "ControllerGridStream.inl"
	void Run(bool Socket) {
		BootstrapNativeRuntimeSchema();
		CheckInstallationRaces();
		CheckPairedInstallations();
		CheckControllerGridStream();
		auto Sim = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto Transport = [&]() -> std::shared_ptr<IGameTransport> {
#if defined(GARGANTUAN_WITH_GNS)
			if (Socket) return std::make_shared<GameNetworkingSocketsTransport>();
#endif
			if (Socket) throw std::runtime_error("GNS unavailable");
			return Sim->CreateTransport();
		};
		const TransportEndpoint Endpoint{"127.0.0.1", 36735};
		const auto Limits = GameSessionConfiguration::DefaultLimits();
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Interior = std::make_shared<SpatialRegion>();
		Interior->SetName("Interior");
		Interior->SetParent(Space);
		auto Outside = Seed(Space, "ExteriorGrid", blockgrid::Stone);
		auto Inside = Seed(Interior, "InteriorGrid", blockgrid::Brick);
		auto Large=std::make_shared<BlockGrid>();
		Large->SetName("LargeGrid"); Large->SetCanCollide(false);
		Large->RestoreField(blockgrid::test::LargePublicField());
		Large->SetParent(Space);
		HeadlessRenderer ServerView(Vector2(64, 64)), ViewA(Vector2(64, 64)), ViewB(Vector2(64, 64));
		Engine ServerRuntime(
			World, &ServerView, nullptr, {.Mode = RuntimeMode::NetworkServer, .AdmitSpatialContent = true}
		);
		ServerRuntime.ProcessService->Alive = true;
		bool GrantInterior = false;
		GameSession Server(
			Transport(),
			{.Role = GameSessionRole::Server,
			 .Endpoint = Endpoint,
			 .Limits = Limits,
			 .SpatialContent = true,
			 .ResolveInitialSpatialAudience =
				 [&](const Player &) { return GrantInterior ? Interior->GetObjectId() : ObjectId{}; }},
			&ServerRuntime
		);
		GameSessionConfiguration ClientConfig{
			.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .SpatialContent = true
		};
		GameSession A(Transport(), ClientConfig), B(Transport(), ClientConfig);
		std::unique_ptr<Engine> ClientA, ClientB;
		bool StartedB = false;
		std::uint64_t Tick{};
		auto Exchange = [&] {
			if (!Socket) {
				(void)Sim->Advance(10ms);
				Sim->Pump();
			}
			(void)Server.Poll();
			(void)A.Poll();
			if (StartedB) (void)B.Poll();
			Server.Step(++Tick);
			A.Step(Tick);
			if (StartedB) B.Step(Tick);
			if (Socket) std::this_thread::sleep_for(3ms);
		};
		auto Wait = [&](auto Predicate, const char *Failure) {
			for (int I = 0; I < 500; ++I) {
				if (Predicate()) return;
				Exchange();
				if (A.GetStatus() == GameSessionStatus::Failed || B.GetStatus() == GameSessionStatus::Failed) break;
			}
			if (!Predicate()) {
				std::cerr << "[BlockGrid:Session] tick=" << Tick << " server=" << static_cast<int>(Server.GetStatus())
						  << " first=" << static_cast<int>(A.GetStatus())
						  << " second=" << static_cast<int>(B.GetStatus())
						  << " players=" << Server.GetMetrics().PlayersCreated
						  << " accepted=" << Server.GetMetrics().AcceptedPeers << " ServerError=" << Server.GetFailure()
						  << " FirstError=" << A.GetFailure() << " SecondError=" << B.GetFailure() << '\n';
				throw std::runtime_error(Failure);
			}
		};
		auto Attach = [&](GameSession &Session, HeadlessRenderer &View, std::unique_ptr<Engine> &Runtime) {
			Wait(
				[&] { return Session.GetStatus() == GameSessionStatus::Accepted; },
				"client did not complete trusted spatial bootstrap"
			);
			Runtime = std::make_unique<Engine>(
				Session.GetClientDataModel(),
				&View,
				nullptr,
				EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitSpatialContent = true}
			);
			Runtime->ProcessService->Alive = true;
			Require(Session.AttachClientRuntime(*Runtime), "spatial runtime attachment failed");
		};
		Require(Server.Start().Succeeded() && A.Start().Succeeded(), "start first peer");
		Attach(A, ViewA, ClientA);
		Wait([&] { auto G=Find(*ClientA->DataModel,"LargeGrid"); return G && G->HasReplicaField() &&
			G->GetField().FindDefinitionId("project:near").has_value(); },"large source failed to stream the initial window");
		auto LargeReplica=Find(*ClientA->DataModel,"LargeGrid");
		const auto LargeId=LargeReplica->GetObjectId();
		auto Covered=[&](blockgrid::Cell P) {
			try { LargeReplica->RequireReplicaCoverage(blockgrid::SingleCell(P)); return true; }
			catch (const std::runtime_error&) { return false; }
		};
		const blockgrid::Cell Near{12,12,12},Far{84,52,52};
		Require(Covered(Near) && !Covered(Far) && LargeReplica->GetField().GetValue(Near,0)==17 &&
			LargeReplica->GetField().Get(Near).Orientation==11 && LargeReplica->GetField().GetChannelCount()==1 &&
			!LargeReplica->GetField().FindDefinitionId("project:distant"),"large replica included distant or private data");
		const auto Peers=Server.GetPeerIdentities();
		Require(Peers.size()==1,"large fixture requires one subscribed exterior peer");
		const std::array FocusFar{glm::vec3(84.5f,52.5f,52.5f)};
		Require(Server.SetTrustedReplicationFocus(Peers.front().Connection,FocusFar),"server rejected trusted grid interest");
		Wait([&] { return Covered(Far) && !Covered(Near) && LargeReplica->GetField().FindDefinitionId("project:distant").has_value(); },
			"moved interest retained the old window or missed the new one");
		Require(LargeReplica->GetObjectId()==LargeId && LargeReplica->GetField().Get(Far).Orientation==9 &&
			!LargeReplica->GetField().FindDefinitionId("project:near"),"window movement replaced grid identity or retained its old schema");
		auto Clear=std::make_shared<blockgrid::EditRequest>();
		Clear->Writes.push_back({blockgrid::SingleCell(Far),{}});
		Require(MutationGateway{}.Apply(SubmitBlockGridEditCommand{Large->GetObjectId(),World->GetObjectId(),Clear},
			ScriptSecurityContext::CoreTrusted()).Succeeded(),"large far-cell deletion failed");
		Wait([&] { return Covered(Far) && !LargeReplica->GetField().Get(Far).Definition &&
			!LargeReplica->GetField().FindDefinitionId("project:distant"); },"large deletion or last-use schema retirement failed");
		const std::array MultiFocus{glm::vec3(12.5f),FocusFar.front()};
		Require(Server.SetTrustedReplicationFocus(Peers.front().Connection,MultiFocus),"server rejected independent interests");
		Wait([&] { return Covered(Far) && Covered(Near) && LargeReplica->GetField().GetValue(Near,0)==17; },
			"independent origins did not converge in one grid");
		const std::array ResetFocus{glm::vec3(0)};
		Require(Server.SetTrustedReplicationFocus(Peers.front().Connection,ResetFocus),"server rejected initial interest");
		Wait([&] { return Covered(Near) && !Covered(Far); },"retired distant coverage still admitted reads");
		GrantInterior = true;
		Require(B.Start().Succeeded(), "start second peer");
		StartedB = true;
		Attach(B, ViewB, ClientB);
		auto Received = [&](Engine &Runtime, std::string_view Name, std::uint16_t Expected) {
			auto Grid = Find(*Runtime.DataModel, Name);
			return Grid && Grid->HasReplicaField() && Grid->GetField().Get({-3, 0, 1}).Definition == Expected;
		};
		Wait(
			[&] {
				return Received(*ClientA, "ExteriorGrid", blockgrid::Stone) &&
					   Received(*ClientB, "InteriorGrid", blockgrid::Brick);
			},
			"scoped native baseline failed"
		);
		Require(
			!Find(*ClientA->DataModel, "InteriorGrid") && !Find(*ClientB->DataModel, "ExteriorGrid") &&
				!Find(*ClientB->DataModel,"LargeGrid"),
			"incorrect audience"
		);
		auto Replica = Find(*ClientA->DataModel, "ExteriorGrid");
		Require(
			Replica->GetCellSize() == 2 && Replica->GetField().GetChannelCount() == 1 &&
				Replica->GetField().GetChannel(0).Spec.Name == "powered",
			"transform sizing or public channel projection failed"
		);
		auto Request = std::make_shared<blockgrid::EditRequest>();
		Request->Writes.push_back({blockgrid::SingleCell({-3, 0, 1}), {blockgrid::Brick, 0}});
		MutationGateway Gateway;
		Require(
			Gateway
				.Apply(
					SubmitBlockGridEditCommand{Outside->GetObjectId(), World->GetObjectId(), Request},
					ScriptSecurityContext::CoreTrusted()
				)
				.Succeeded(),
			"server patch failed"
		);
		Wait([&] { return Received(*ClientA, "ExteriorGrid", blockgrid::Brick); }, "public delta failed");
		Require(Received(*ClientB, "InteriorGrid", blockgrid::Brick), "unrelated domain changed");
		auto Motion = [&](bool Dynamic) {
			auto Edit = std::make_shared<blockgrid::EditRequest>();
			Edit->ExpectedRevision = Outside->GetBlockRevision();
			Edit->Motion = blockgrid::MotionEdit{Dynamic};
			Require(
				Gateway
					.Apply(
						SubmitBlockGridEditCommand{Outside->GetObjectId(), World->GetObjectId(), Edit},
						ScriptSecurityContext::CoreTrusted()
					)
					.Succeeded(),
				"server assembly transition failed"
			);
		};
		Motion(true);
		Wait([&] { return Replica->GetMotionMode() == "Dynamic"; }, "projected body did not activate");
		const auto Initial = Replica->GetAssemblyProjection();
		Require(
			Initial && Initial->Dynamic && std::abs(Initial->Mass - 8.f) < .001f && !Replica->IsDynamicAssembly(),
			"replica mass did not match the server-owned two-stud cube"
		);
		const auto Bytes = blockgrid::Encode(Replica->GetField());
		for (unsigned I = 0; I < 20; ++I) {
			Space->StepPhysics(1. / 60, std::nullopt);
			Exchange();
		}
		const auto Last = Outside->GetAssemblyProjection();
		Require(Last && Last->Revision > Initial->Revision, "native assembly did not move");
		Wait(
			[&] {
				const auto Applied = Replica->GetAssemblyProjection();
				return Applied && Applied->Revision == Last->Revision &&
					   Applied->Pose.Position == Last->Pose.Position &&
					   Applied->LinearVelocity == Last->LinearVelocity && Applied->LocalInertia == Last->LocalInertia;
			},
			"normal GameSession lost body state or pose"
		);
		Require(
			!Replica->ApplyReplicaAssemblyProjection(*Initial) &&
				Replica->GetCFrame().Position == Last->Pose.Position && blockgrid::Encode(Replica->GetField()) == Bytes,
			"old receipt rewound the body or motion changed cell data"
		);
		Motion(false);
		Wait(
			[&] { return Replica->GetMotionMode() == "Static" && Replica->GetAssemblyProjection()->Mass == 0.f; },
			"relevant replica did not retire dynamic state"
		);
		Outside->SetSpatialRegion(Interior);
		Wait([&] { return !Find(*ClientA->DataModel,"ExteriorGrid") && Received(*ClientB,"ExteriorGrid",blockgrid::Brick); },
			"cross-chart assignment failed to retire the old audience and install the new grid");
		Require(Replica->GetDestroyed() || !Replica->HasReplicaField(),"detached grid shell retained its installed pages");
		Outside->SetSpatialRegion(std::nullopt);
		Wait([&] { return Received(*ClientA,"ExteriorGrid",blockgrid::Brick) && !Find(*ClientB->DataModel,"ExteriorGrid"); },
			"returning grid shell did not acquire a fresh subscription");
		const auto Returning=Find(*ClientA->DataModel,"ExteriorGrid");
		const auto CloseOnField=Returning->GetPropertyChangedSignal("BlockRevision")->Once([&](auto) {
			A.Stop();
		});
		Request=std::make_shared<blockgrid::EditRequest>();
		Request->Writes.push_back({blockgrid::SingleCell({-3,0,1}),{blockgrid::Stone,0}});
		Require(Gateway.Apply(SubmitBlockGridEditCommand{Outside->GetObjectId(),World->GetObjectId(),Request},
			ScriptSecurityContext::CoreTrusted()).Succeeded(),"final exterior edit failed");
		Wait([&] { return A.GetStatus()==GameSessionStatus::Closed; },"field publication did not close its session");
		Require(!Returning->HasReplicaField() && B.GetStatus()==GameSessionStatus::Ready && Received(*ClientB,"InteriorGrid",blockgrid::Brick),
			"field callback retained a retired installation or interrupted the other player");
		CloseOnField->Disconnect();
		const auto InteriorReplica=Find(*ClientB->DataModel,"InteriorGrid");
		const auto CloseOnConfiguration=InteriorReplica->GetPropertyChangedSignal("Visible")->Once([&](auto) {
			B.Stop();
		});
		Inside->SetVisible(false);
		Wait([&] { return B.GetStatus()==GameSessionStatus::Closed; },"configuration publication did not close its session");
		Require(!InteriorReplica->HasReplicaField(),"configuration callback retained a retired installation");
		CloseOnConfiguration->Disconnect();
		Server.Stop();
		ClientA->Destroy();
		ClientB->Destroy();
		ServerRuntime.Destroy();
	}
}
int main(int Count, char **Arguments) {
	SDL_SetLogOutputFunction(
		[](void *, int, SDL_LogPriority, const char *Text) { std::cerr << Text << '\n'; }, nullptr
	);
	SDL_SetLogPriority(LogCategory::App, SDL_LOG_PRIORITY_WARN);
	try {
		Run(Count > 1 && std::string_view(Arguments[1]) == "--gns");
		std::cout << "[BlockGrid:Session] Two scoped peers installed collision, public state and live edits\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[BlockGrid:Session] " << Error.what() << '\n';
		return 1;
	}
}
