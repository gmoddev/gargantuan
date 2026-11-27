// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/terrain/Field.hpp"
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
			World, &HostView, nullptr, {.Mode = RuntimeMode::NetworkServer, .AdmitSpatialContent = true}
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
			 .SpatialContent = true,
			 .ResolveInitialSpatialAudience =
				 [&](const Player &) { return SpectatorStarted ? Gallery->GetObjectId() : ObjectId{}; }},
			&ServerRuntime
		);
		const GameSessionConfiguration Client{
			.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .SpatialContent = true
		};
		GameSession Owner(Transport(), Client), Spectator(Transport(), Client);
		std::unique_ptr<Engine> OwnerRuntime, SpectatorRuntime;
		std::uint64_t Tick{};
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
				Require(!Find(*OwnerRuntime, "DistantGrid"), "staging exposed a nonintersecting grid");
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
				std::cerr << "[Spatial:Session] tick=" << Tick << " character=" << CharacterProfile
						  << " owner=" << Owner.GetFailure() << " spectator=" << Spectator.GetFailure()
						  << " server=" << Server.GetFailure();
				if (Connection) {
					const auto Status = Server.GetSpatialTransferStatus(*Connection);
					if (Status) std::cerr << " transfer=" << static_cast<int>(*Status);
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
				EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitSpatialContent = true}
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
		SpectatorStarted = true;
		Require(Spectator.Start().Succeeded(), "spectator startup failed");
		Attach(Spectator, SpectatorView, SpectatorRuntime);
		Wait(
			[&] { return Server.GetMetrics().ReadyPeers == 2 && Find(*SpectatorRuntime, "GalleryWall"); },
			"spectator not independently ready"
		);
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
		Require(
			Server.RequestSpatialTransfer(*Connection, Room->GetObjectId(), Target, Bounds) ==
				SpatialTransferStatus::Progress,
			"movement request failed"
		);
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
		}
		Owner.Stop();
		Spectator.Stop();
		Server.Stop();
		OwnerRuntime->Destroy();
		SpectatorRuntime->Destroy();
		ServerRuntime.Destroy();
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
