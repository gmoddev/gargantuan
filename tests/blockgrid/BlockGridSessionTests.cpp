// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/network/BlockGridStream.hpp"
#include "gargantuan/network/GameSession.hpp"
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
	void Run(bool Socket) {
		BootstrapNativeRuntimeSchema();
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
					<< " first=" << static_cast<int>(A.GetStatus()) << " second=" << static_cast<int>(B.GetStatus())
					<< " players=" << Server.GetMetrics().PlayersCreated << " accepted=" << Server.GetMetrics().AcceptedPeers
					<< " ServerError=" << Server.GetFailure() << " FirstError=" << A.GetFailure() << " SecondError=" << B.GetFailure() << '\n';
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
			!Find(*ClientA->DataModel, "InteriorGrid") && !Find(*ClientB->DataModel, "ExteriorGrid"),
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
		A.Stop();
		B.Stop();
		Server.Stop();
		ClientA->Destroy();
		ClientB->Destroy();
		ServerRuntime.Destroy();
	}
}
int main(int Count, char **Arguments) {
	try {
		Run(Count > 1 && std::string_view(Arguments[1]) == "--gns");
		std::cout << "[BlockGrid:Session] Two scoped peers installed collision, public state and live edits\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[BlockGrid:Session] " << Error.what() << '\n';
		return 1;
	}
}
