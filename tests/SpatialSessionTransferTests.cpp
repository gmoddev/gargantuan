// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include <fstream>
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/terrain/Field.hpp"
#include "gargantuan/network/TerrainSession.hpp"
#include "../src/terrain/ScenePersistence.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
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
	void Require(bool Value, std::string_view Message) {
		if (!Value) throw std::runtime_error(std::string(Message));
	}
	std::shared_ptr<Instance> Find(const Engine &Runtime, std::string_view Name) {
		for (const auto &Object : Runtime.DataModel->GetDescendants())
			if (Object->GetName() == Name) return Object;
		return {};
	}
	std::shared_ptr<Part> MakePart(std::string Name, CFrame Pose, const std::shared_ptr<Instance> &Parent) {
		auto PartValue = std::make_shared<Part>();
		PartValue->SetName(Name);
		PartValue->SetAnchored(true);
		PartValue->SetSize({2, 2, 2});
		PartValue->SetCFrame(Pose);
		PartValue->SetParent(Parent);
		return PartValue;
	}
	void MakeGrid(std::string Name, CFrame Pose, const std::shared_ptr<Instance> &Parent) {
		auto Grid = std::make_shared<BlockGrid>();
		Grid->SetName(Name);
		Grid->SetCellSize(1);
		Grid->SetCFrame(Pose);
		blockgrid::Field Field;
		const std::array Writes{blockgrid::Write{blockgrid::SingleCell({0, 0, 0}), {blockgrid::Stone, 0}}};
		Require(Field.Apply(Writes), "authoring grid cell failed");
		Grid->RestoreField(std::move(Field));
		Grid->SetParent(Parent);
	}
	std::string MakeOffsetMesh(DataModel &World) {
		const auto Root=std::filesystem::temp_directory_path()/("GargantuanSpatialStage-"+AssetId::New().ToString());
		std::filesystem::create_directories(Root/"assets");
		struct FixtureFiles {
			std::filesystem::path Root;
			~FixtureFiles() {
				std::error_code Error;
				std::filesystem::remove(Root/"assets"/"offset.obj",Error);
				std::filesystem::remove(Root/"assets",Error);
				std::filesystem::remove(Root,Error);
			}
		} Cleanup{Root};
		std::ofstream(Root/"assets"/"offset.obj")<<"v 10 0 -1\nv 12 0 -1\nv 10 2 1\nf 1 2 3\n";
		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		const auto Assets=std::dynamic_pointer_cast<AssetService>(World.GetService("AssetService"));
		const auto Imported=Assets->ImportProjectAsset(Mount,"assets/offset.obj",AssetKind::Mesh);
		Require(Imported.Ok && Imported.Record.has_value(),"offset mesh import failed");
		return Imported.Record->Reference.Value;
	}
	void Run(bool CharacterProfile, bool Socket) {
		auto Sim = SimulatedNetwork::Create(
			{.BaseLatency = 2ms, .MaximumReorderDelay = 5ms, .UnreliableReorderProbability = .25}
		);
		auto Transport = [&]() -> std::shared_ptr<IGameTransport> {
#if defined(GARGANTUAN_WITH_GNS)
			if (Socket) return std::make_shared<GameNetworkingSocketsTransport>();
#endif
			if (Socket) throw std::runtime_error("GNS unavailable");
			return Sim->CreateTransport();
		};
		const auto World = std::make_shared<DataModel>();
		const auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		std::shared_ptr<const TerrainSessionData> ServerTerrain, ReceiverTerrain;
		if (CharacterProfile) {
			auto Field=std::make_unique<terrain::Field>();
			const terrain::SphereBrush Brush{{7,3,7},2.75,terrain::BrushMode::Add,5};
			const auto Keys=terrain::GetRegionChunks(terrain::GetBrushBounds(Field->GetGrid(),Brush).value()).value();
			auto Edit=terrain::Field::Prepare(Field->Capture(Keys).value(),Brush).value();
			(void)Field->Commit(std::move(Edit)).value();
			Space->RestoreTerrainField(std::move(Field));
			const auto Project=ProjectId::New();
			const auto Content=terrain::CaptureSceneTerrain(*World,Project).value();
			ServerTerrain=MakeWorkspaceTerrainSender(Project,1,Content.Artifacts,Content.ManifestDigest);
			ReceiverTerrain=MakeTerrainReceiver(Project,1,Content.ManifestDigest,Content.Index.Grid);
		}
		const auto Room = std::make_shared<SpatialRegion>(), Gallery = std::make_shared<SpatialRegion>();
		Room->SetName("LandingRoom");
		Room->SetParent(Space);
		if (CharacterProfile) {
			auto Field = std::make_unique<terrain::Field>(terrain::GridSpec{.SpacingShift=1});
			const terrain::SphereBrush Brush{{4,26,4},3.5,terrain::BrushMode::Add,3};
			const auto Area = terrain::GetBrushBounds(Field->GetGrid(),Brush).value();
			const auto Keys = terrain::GetRegionChunks(Area).value();
			auto Edit = terrain::Field::Prepare(Field->Capture(Keys).value(),Brush).value();
			(void)Field->Commit(std::move(Edit)).value();
			Room->RestoreTerrainField(std::move(Field));
		}
		Gallery->SetName("SpectatorRoom");
		Gallery->SetParent(Space);
		const auto SourcePose = CFrame(0, 8, 0), Target = CFrame(0, 30, 0);
		const SpatialDestinationBounds Bounds{{-8, 24, -7}, {8, 36, 7}};
		const auto Actor = MakePart("TransferBody", SourcePose, Space);
		const auto ActorId = Actor->GetObjectId();
		MakePart("LandingWall", CFrame(-5, 30, 0), Room);
		MakePart("GalleryWall", CFrame(-5, 30, 0), Gallery);
		MakeGrid("LandingGrid", CFrame(5, 30, 0), Room);
		MakeGrid("DistantGrid", CFrame(10500, 30, 0), Room);
		std::shared_ptr<KinematicCharacter> Avatar;
		if (CharacterProfile) {
			Avatar = std::make_shared<KinematicCharacter>();
			Avatar->SetName("TransferCharacter");
			Avatar->SetParent(Space);
			Avatar->SetCFrame(SourcePose);
			Actor->SetParent(Avatar);
			Avatar->SetRootPart(Actor);
			Avatar->SetCapsuleRadius(1);
			Avatar->SetCapsuleHeight(4);
		}
		const auto Parent = Actor->GetParent();
		HeadlessRenderer HostView(Vector2(64, 64)), OwnerView(Vector2(64, 64)), SpectatorView(Vector2(64, 64));
		Engine ServerRuntime(
			World, &HostView, nullptr, {.Mode = RuntimeMode::NetworkServer, .AdmitNetworkTerrain=bool(ServerTerrain), .AdmitSpatialContent = true}
		);
		ServerRuntime.ProcessService->Alive = true;
		ServerRuntime.Players->SetCharacterAutoLoads(false);
		const TransportEndpoint Endpoint{"127.0.0.1", static_cast<std::uint16_t>(CharacterProfile ? 36739 : 36738)};
		const auto Limits = GameSessionConfiguration::DefaultLimits();
		bool SpectatorStarted{};
		GameSession Server(
			Transport(),
			{.Role = GameSessionRole::Server,
			 .Endpoint = Endpoint,
			 .Limits = Limits,
			 .TerrainBootstrap=ServerTerrain,
			 .SpatialContent = true,
			 .ResolveInitialSpatialAudience =
				 [&](const Player &) { return SpectatorStarted ? Gallery->GetObjectId() : ObjectId{}; }},
			&ServerRuntime
		);
		const GameSessionConfiguration Client{
			.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap=ReceiverTerrain, .SpatialContent = true
		};
		std::unique_ptr<Engine> OwnerRuntime, SpectatorRuntime;
		GameSession Owner(Transport(), Client), Spectator(Transport(), Client);
		struct Cleanup {
			GameSession &Owner, &Spectator, &Server;
			std::unique_ptr<Engine> &OwnerRuntime, &SpectatorRuntime;
			Engine &ServerRuntime;
			~Cleanup() {
				Owner.Stop(); Spectator.Stop(); Server.Stop();
				if (OwnerRuntime) OwnerRuntime->Destroy();
				if (SpectatorRuntime) SpectatorRuntime->Destroy();
				ServerRuntime.Destroy();
			}
		} End{Owner, Spectator, Server, OwnerRuntime, SpectatorRuntime, ServerRuntime};
		std::uint64_t Tick{};
		std::optional<CharacterMotionResult> LastMotion;
		bool ObserveStage{}, SawScopedGrid{};
		std::optional<ConnectionId> Connection;
		auto Exchange = [&] {
			if (Socket)
				std::this_thread::sleep_for(2ms);
			else {
				(void)Sim->Advance(10ms);
				Sim->Pump();
			}
			(void)Server.Poll();
			(void)Owner.Poll();
			if (SpectatorStarted) (void)Spectator.Poll();
			Server.Step(++Tick);
			Owner.Step(Tick);
			if (SpectatorStarted) Spectator.Step(Tick);
			if (SpectatorRuntime)
				Require(
					!Find(*SpectatorRuntime, "LandingWall") && !Find(*SpectatorRuntime, "LandingGrid") &&
						!Find(*SpectatorRuntime, "DistantGrid"),
					"spectator received private destination content"
				);
			if (ObserveStage && OwnerRuntime && Connection &&
				Server.GetSpatialTransferStatus(*Connection) == SpatialTransferStatus::Progress) {
				Require(!Find(*OwnerRuntime, "DistantGrid") && !Find(*OwnerRuntime,"DistantMeshScene") &&
					!Find(*OwnerRuntime,"FarBoxMesh") && !Find(*OwnerRuntime,"OffsetMesh"),
					"staging exposed a nonintersecting grid, mesh or scene ancestor");
				const auto Grid = std::dynamic_pointer_cast<BlockGrid>(Find(*OwnerRuntime, "LandingGrid"));
				SawScopedGrid |= Grid && Grid->HasReplicaField();
			}
		};
		auto Wait = [&](auto Predicate, std::string_view Failure) {
			for (int I = 0; I < 800 && !Predicate(); ++I) {
				Exchange();
				if (Owner.GetStatus() == GameSessionStatus::Failed ||
					Spectator.GetStatus() == GameSessionStatus::Failed)
					break;
			}
			if (!Predicate()) {
				std::cerr << "[Spatial:Session] " << Failure << " tick=" << Tick << " character=" << CharacterProfile
						  << " owner=" << Owner.GetFailure() << " spectator=" << Spectator.GetFailure()
						  << " server=" << Server.GetFailure();
				if (LastMotion) {
					std::cerr << " motionStatus=" << static_cast<int>(LastMotion->Status)
						<< " appliedX=" << LastMotion->AppliedTranslation.x << " contact=" << LastMotion->Collided
						<< " physicsNs=" << LastMotion->PhysicsCpuNanoseconds;
					if (Connection) std::cerr << " serverMotion={" << detail::GameSessionTestAccess::DescribeSpatialMotion(Server, *Connection) << '}';
					if (const auto Primary=Owner.GetPrimaryConnection())
						std::cerr << " ownerMotion={" << detail::GameSessionTestAccess::DescribeSpatialMotion(Owner, *Primary) << '}';
				}
				if (Connection) {
					const auto Status = Server.GetSpatialTransferStatus(*Connection);
					if (Status) std::cerr << " transfer=" << static_cast<int>(*Status);
				}
				if (const auto Primary=Owner.GetPrimaryConnection()) {
					const auto Status=Owner.GetSpatialTransferStatus(*Primary);
					if (Status) std::cerr << " ownerTransfer=" << static_cast<int>(*Status);
				}
				std::cerr << '\n';
				throw std::runtime_error(std::string(Failure));
			}
		};
		auto Attach = [&](GameSession &Session, HeadlessRenderer &View, std::unique_ptr<Engine> &Runtime) {
			Wait(
				[&] { return Session.GetStatus() == GameSessionStatus::Accepted; }, "spatial session bootstrap failed"
			);
			Runtime = std::make_unique<Engine>(
				Session.GetClientDataModel(),
				&View,
				nullptr,
				EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitNetworkTerrain=bool(ReceiverTerrain), .AdmitSpatialContent = true}
			);
			Runtime->ProcessService->Alive = true;
			Require(Session.AttachClientRuntime(*Runtime), "client runtime attachment failed");
		};
		Require(Server.Start().Succeeded() && Owner.Start().Succeeded(), "server/owner startup failed");
		Attach(Owner, OwnerView, OwnerRuntime);
		Wait([&] { return Server.GetMetrics().ReadyPeers == 1; }, "owner not ready");
		for (const auto &Identity : Server.GetPeerIdentities())
			if (Identity.Ready) Connection = Identity.Connection;
		Require(Connection.has_value(), "accepted controlling connection missing");
		const std::array SourceFocus{SourcePose.Position};
		Require(Server.SetTrustedReplicationFocus(*Connection, SourceFocus), "owner source interest unavailable");
		SpectatorStarted = true;
		Require(Spectator.Start().Succeeded(), "spectator startup failed");
		Attach(Spectator, SpectatorView, SpectatorRuntime);
		Wait([&] { return Server.GetMetrics().ReadyPeers == 2; }, "spectator handshake not ready");
		std::optional<ConnectionId> GalleryConnection;
		for (const auto &Identity : Server.GetPeerIdentities())
			if (Identity.Ready && Identity.Connection != *Connection) GalleryConnection = Identity.Connection;
		const std::array GalleryFocus{glm::vec3(-5, 30, 0)};
		Require(
			GalleryConnection && Server.SetTrustedReplicationFocus(*GalleryConnection, GalleryFocus),
			"spectator gallery interest unavailable"
		);
		Wait(
			[&] { return Server.GetMetrics().ReadyPeers == 2 && Find(*SpectatorRuntime, "GalleryWall"); },
			"spectator not independently ready"
		);
		if (CharacterProfile) {
			Require(OwnerRuntime->Workspace->GetTerrain()->GetField().GetStats().MaterializedPages>0 &&
				SpectatorRuntime->Workspace->GetTerrain()->GetField().GetStats().MaterializedPages==0,
				"Workspace bootstrap samples crossed the spectator's chart boundary");
		}
		const auto PlayerValue = Server.GetAcceptedPlayer(*Connection);
		Require(bool(PlayerValue), "authoritative player missing");
		if (Avatar)
			PlayerValue->SetCharacter(Avatar);
		else
			PlayerValue->SetPossession(Actor);
		auto Bindings = [&] {
			return Avatar ? Server.GetMetrics().CharacterControlBindings : Server.GetMetrics().PossessionInput.Bindings;
		};
		Wait(
			[&] {
				const auto Local = OwnerRuntime->Players->GetLocalPlayer().value_or(nullptr);
				return Bindings() > 0 && Local &&
					   (Avatar ? bool(Local->GetCharacter()) : bool(Local->GetPossessedEntity()));
			},
			"control not attached"
		);
		const auto ReplicaActor = std::dynamic_pointer_cast<Part>(Find(*OwnerRuntime, "TransferBody"));
		Require(
			ReplicaActor && !Find(*OwnerRuntime, "LandingWall") && !Find(*OwnerRuntime, "LandingGrid"),
			"initial audience mismatch"
		);
		const auto ReplicaId = ReplicaActor->GetObjectId();
		Require(
			Owner.RequestSpatialTransfer(*Owner.GetPrimaryConnection(), Room->GetObjectId(), Target, Bounds) ==
				SpatialTransferStatus::Denied,
			"client originated authority"
		);
		Require(
			Server.RequestSpatialTransfer(*Connection, Room->GetObjectId(), Target, {{0, 30, 0}, {0, 30, 0}}) ==
				SpatialTransferStatus::Denied,
			"undersized landing volume admitted"
		);
		if (!CharacterProfile) {
			const auto Visual=MakeOffsetMesh(*World);
			auto Scene=std::make_shared<Folder>();
			Scene->SetName("DistantMeshScene");Scene->SetParent(Room);
			const auto Mesh=[&](std::string Name,CFrame Pose,Enums::MeshCollisionMode Mode,const std::shared_ptr<Instance>& ParentValue) {
				auto Value=std::make_shared<MeshPart>();
				Value->SetName(std::move(Name));Value->SetMesh(Visual);Value->SetAnchored(true);Value->SetSize({2,1,1});
				Value->SetCFrame(Pose);Value->SetCollisionMode(Mode);Value->SetParent(ParentValue);return Value;
			};
			const auto Quarter=CFrame::Angles(0,1.57079632679f,0);
			const auto FarBox=Mesh("FarBoxMesh",CFrame(1200,30,0),Enums::MeshCollisionMode::Box,Scene);
			const auto Offset=Mesh("OffsetMesh",CFrame(0,30,0)*Quarter,Enums::MeshCollisionMode::StaticMesh,Scene);
			auto Private=std::make_shared<Folder>();
			Private->SetName("PrivateMeshOwner");Private->SetParent(Room);
			const auto Unsupported=Mesh("LocalPrivateMesh",CFrame(5,30,22)*Quarter,Enums::MeshCollisionMode::StaticMesh,Private);
			const auto Native=Room->PrepareSpatialDestination(Bounds);
			Require(Native && std::ranges::any_of(Native->Providers,[&](const auto& P){
				return P.Owner==Unsupported->GetObjectId() && P.Shape==PhysicsShapeKind::StaticTriangleMesh;
			}) && std::ranges::none_of(Native->Providers,[&](const auto& P){
				return P.Owner==Offset->GetObjectId() || P.Owner==FarBox->GetObjectId();
			}),"native receipt ignored scaled, rotated, offset collision geometry");
			Require(Server.RequestSpatialTransfer(*Connection,Room->GetObjectId(),Target,Bounds)==SpatialTransferStatus::Denied,
				"unsupported native collider received a staged audience");
			for (int I=0;I<12;++I) Exchange();
			Require(!Find(*OwnerRuntime,"PrivateMeshOwner") && !Find(*OwnerRuntime,"LocalPrivateMesh") &&
				!Find(*OwnerRuntime,"DistantMeshScene"),"rejected stage published a destination identity");
			Private->Destroy();
		}
		const auto PreviousBindings = Bindings();
		Require(
			Server.RequestSpatialTransfer(*Connection, Room->GetObjectId(), Target, Bounds) ==
				SpatialTransferStatus::Progress,
			"cancel fixture request failed"
		);
		const std::array NewFocus{glm::vec3(0, 8, 2)};
		Require(
			Server.SetTrustedReplicationFocus(*Connection, NewFocus) &&
				Server.GetSpatialTransferStatus(*Connection) == SpatialTransferStatus::Canceled,
			"focus replacement failed to revoke the active grant"
		);
		Wait([&] { return Bindings() > PreviousBindings; }, "old control did not retire after cancel");
		Require(
			Actor->GetCFrame().FuzzyEq(SourcePose) && WorldRoot::GetEffectiveWorld(*Actor) == Space,
			"cancellation moved the actor"
		);
		ObserveStage = true;
		std::shared_ptr<Script> Creator;
		if (CharacterProfile) {
			const auto Shared = std::make_shared<ModuleScript>();
			Shared->SetName("MovementFixtureState");
			Shared->SetSource("return {}");
			Shared->SetParent(Space);
			Creator = std::make_shared<Script>();
			Creator->SetName("MovementIssuer");
			Creator->SetRunContext(Enums::RunContext::Server);
			Creator->SetParent(Space);
			Creator->SetSource(R"lua(
local State = require("@game/Workspace/MovementFixtureState")
local Room = game.Workspace.LandingRoom
local Owner
for _, Player in game:GetService("Players"):GetPlayers() do
	if Player.Character and Player.Character.Name == "TransferCharacter" then Owner = Player end
end
local Transfer, Reason = Owner:RequestSpatialTransfer(Room, CFrame.new(0,30,0), Vector3.new(-8,24,-7), Vector3.new(8,36,7))
assert(Transfer and not Reason and typeof(Transfer) == "SpatialTransfer" and Transfer:GetStatus() == "Pending")
State.Ticket = Transfer
task.defer(function()
	local State = Transfer:GetStatus()
	assert(State == "Applied" and Transfer:Cancel() == State)
	script:SetAttribute("ObservedApplied", true)
end)
)lua");
			ServerRuntime.Script->RunBootstrapScript(Creator);
			if (Creator->Status == ScriptStatus::Error) throw std::runtime_error(Creator->ErrorMessage);
			const auto Other = std::make_shared<Script>();
			Other->SetName("MovementOther");
			Other->SetRunContext(Enums::RunContext::Server);
			Other->SetParent(Space);
			Other->SetSource(R"lua(
local Ticket = require("@game/Workspace/MovementFixtureState").Ticket
assert(not pcall(function() Ticket:Cancel() end))
assert(not pcall(function() Ticket:GetStatus() end))
script:SetAttribute("Isolated", true)
)lua");
			ServerRuntime.Script->RunBootstrapScript(Other);
			if (Other->Status == ScriptStatus::Error) throw std::runtime_error(Other->ErrorMessage);
			Require(Other->GetAttributeValue("Isolated").has_value(), "another Script acquired the actor's ticket");
		} else {
			Require(
				Server.RequestSpatialTransfer(*Connection, Room->GetObjectId(), Target, Bounds) ==
					SpatialTransferStatus::Progress,
				"movement request failed"
			);
		}
		Require(
			Server.RequestSpatialTransfer(*Connection, Room->GetObjectId(), Target, Bounds) ==
				SpatialTransferStatus::Busy,
			"overlapping movement accepted"
		);
		Require(
			Server.SetTrustedReplicationFocus(*Connection, NewFocus) &&
				Server.GetSpatialTransferStatus(*Connection) == SpatialTransferStatus::Progress,
			"idempotent focus update revoked a live grant"
		);
		Wait(
			[&] {
				return Server.GetSpatialTransferStatus(*Connection) == SpatialTransferStatus::Applied &&
					   Owner.GetSpatialTransferStatus(*Owner.GetPrimaryConnection()) == SpatialTransferStatus::Applied;
			},
			"bilateral movement did not converge"
		);
		if (Creator) {
			ServerRuntime.Script->Step();
			Require(
				Creator->GetAttributeValue("ObservedApplied").has_value(),
				"creator did not observe its terminal native result"
			);
		}
		Require(SawScopedGrid, "owner moved without an observed installed landing grid");
		const auto ReplicaRoom = std::dynamic_pointer_cast<SpatialRegion>(Find(*OwnerRuntime, "LandingRoom"));
		if (CharacterProfile) {
			RaycastParams Filter;
			Filter.IncludeTerrain = true;
			const auto Cast = [&](SpatialRegion &Domain) { return Domain.ResolveRaycast({4,34,4},{0,-12,0},Filter); };
			const auto ServerHit = Cast(*Room);
			Require(ReplicaRoom && ReplicaRoom->GetTerrain()->GetField().GetGrid()==Room->GetTerrain()->GetField().GetGrid() &&
				ServerHit.TerrainInstance==Room->GetTerrain() && Cast(*ReplicaRoom).TerrainInstance==ReplicaRoom->GetTerrain(),
				"named terrain window and native collider did not follow the authenticated transfer");
		}
		Require(
			ReplicaRoom && WorldRoot::GetEffectiveWorld(*ReplicaActor) == ReplicaRoom &&
				WorldRoot::GetEffectiveWorld(*Actor) == Room && Actor->GetObjectId() == ActorId &&
				ReplicaActor->GetObjectId() == ReplicaId && Actor->GetParent() == Parent &&
				Actor->GetCFrame().FuzzyEq(Target) && ReplicaActor->GetCFrame().FuzzyEq(Target),
			"actor identity, domain or pose diverged"
		);
		Require(
			Room->ResolveRaycast({0, 33, 0}, {0, -6, 0}).GetHitInstance() == Actor &&
				ReplicaRoom->ResolveRaycast({0, 33, 0}, {0, -6, 0}).GetHitInstance() == ReplicaActor &&
				!Space->ResolveRaycast({0, 11, 0}, {0, -6, 0}).HasHit(),
			"provider-local physics disagreed after transfer"
		);
		for (int I = 0; I < 24; ++I)
			Exchange();
		Require(
			Server.GetMetrics().ReadyPeers == 2 && Owner.GetStatus() == GameSessionStatus::Ready &&
				ReplicaActor->GetCFrame().FuzzyEq(Target),
			"late ordinary journal/control frames undid the handoff"
		);
		if (Avatar) {
			const auto LocalAvatar = std::dynamic_pointer_cast<KinematicCharacter>(
				Find(*OwnerRuntime, "TransferCharacter")
			);
			Require(
				LocalAvatar && LocalAvatar->GetCFrame().FuzzyEq(Target) && Avatar->GetCFrame().FuzzyEq(Target) &&
					LocalAvatar->GetRootPart().value_or(nullptr) == ReplicaActor,
				"character and physical root diverged"
			);
			bool Advanced = false;
			const CharacterMotionRequest Step{.Translation={.25f,0,0}};
			Wait([&] {
				if (!Advanced) {
					LastMotion=Avatar->AdmitMotion(*Room,Step);
					Advanced=LastMotion->Succeeded() && LastMotion->AppliedTranslation.x>.2f;
				}
				return Advanced;
			}, "ordinary movement never received matching collision coverage after the handoff");
			const auto Before=Avatar->GetCFrame().Position;
			auto NewWall=MakePart("ChangedCoverage",CFrame(2,30,1),Room);
			Require(!Avatar->AdmitMotion(*Room,Step).Succeeded() && Avatar->GetCFrame().Position==Before,
				"new authority geometry failed to invalidate the acknowledged movement area");
			Require(OwnerRuntime->Workspace->GetTerrain()->GetField().GetStats().MaterializedPages==0,
				"leaving Workspace retained formerly authorized samples");
			Require(!LocalAvatar->AdmitMotion(*ReplicaRoom,Step).Succeeded(),
				"controlled spatial replica advanced predicted physics before authority");
			NewWall->Destroy();
			const SpatialDestinationBounds ReturnArea{{-8,2,-8},{8,14,8}};
			Require(Server.RequestSpatialTransfer(*Connection,Space->GetObjectId(),SourcePose,ReturnArea)==SpatialTransferStatus::Progress,
				"return could not stage Workspace");
			Wait([&] { return Server.GetSpatialTransferStatus(*Connection)==SpatialTransferStatus::Applied &&
				WorldRoot::GetEffectiveWorld(*ReplicaActor)==OwnerRuntime->Workspace; }, "return to streamed Workspace failed");
			RaycastParams TerrainFilter;
			TerrainFilter.IncludeTerrain=true;
			Require(OwnerRuntime->Workspace->ResolveRaycast({7,8,7},{0,-9,0},TerrainFilter).TerrainInstance==OwnerRuntime->Workspace->GetTerrain() &&
				SpectatorRuntime->Workspace->GetTerrain()->GetField().GetStats().MaterializedPages==0,
				"returning controller failed to reacquire Workspace terrain exclusively");
			const auto FormerTerrain = OwnerRuntime->Workspace->GetTerrain();
			Owner.Stop();
			bool Readable = true;
			try { (void)FormerTerrain->GetField(); } catch (const std::runtime_error &) { Readable = false; }
			Require(!Readable && !OwnerRuntime->Workspace->IsTerrainCollisionReady(),
				"a disconnected client retained its old terrain samples or collision");
		}

	}
}
int main(int Count, char **Args) {
	SDL_SetLogOutputFunction(
		[](void *, int, SDL_LogPriority, const char *Message) { std::cerr << Message << '\n'; }, nullptr
	);
	SDL_SetLogPriority(LogCategory::App, SDL_LOG_PRIORITY_WARN);
	try {
		BootstrapNativeRuntimeSchema();
		const bool Socket = Count > 1 && std::string_view(Args[1]) == "--gns";
		Run(false, Socket);
		Run(true, Socket);
		std::cout
			<< "[Spatial:Session] Controlled rigid and character transfers converged with an isolated spectator\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Spatial:Session] " << Error.what() << '\n';
		return 1;
	}
}
