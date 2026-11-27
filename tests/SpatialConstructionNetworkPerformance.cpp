// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "../src/network/ReliableServiceFeedback.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using Clock = std::chrono::steady_clock;
	using Json = nlohmann::ordered_json;
	using namespace std::chrono_literals;
	constexpr blockgrid::Cell Removed{-4, 7, -4};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	double Microseconds(Clock::time_point Start) {
		return std::chrono::duration<double, std::micro>(Clock::now() - Start).count();
	}

	struct Scene {
		std::shared_ptr<Folder> Collection = std::make_shared<Folder>();
		std::shared_ptr<BlockGrid> Grid;
		std::shared_ptr<Part> Removable;
		std::set<std::array<int, 3>> Cells;
	};

	Scene Author(const std::shared_ptr<Workspace> &Space, std::string_view Case) {
		Scene Value;
		Value.Collection->SetName("BenchmarkConstruction");
		Value.Collection->SetParent(Space);
		if (Case == "Empty") return Value;
		std::vector<blockgrid::Write> Writes;
		for (int Z = -4; Z < 4; ++Z)
			for (int Y = 0; Y < 8; ++Y)
				for (int X = -4; X < 4; ++X) {
					if (X != -4 && X != 3 && Y != 0 && Y != 7 && Z != -4 && Z != 3) continue;
					const blockgrid::Cell Cell{X, Y, Z};
					Value.Cells.insert({X, Y, Z});
					if (Case == "BlockGrid") {
						Writes.push_back({blockgrid::SingleCell(Cell), {blockgrid::Stone, 0}});
					} else {
						auto Piece = std::make_shared<Part>();
						Piece->SetSize({1, 1, 1});
						Piece->SetAnchored(true);
						Piece->SetColor(Color3(139.f / 255.f, 144.f / 255.f, 151.f / 255.f));
						Piece->SetCFrame(CFrame(X + .5f, Y + .5f, Z + .5f));
						Piece->SetParent(Value.Collection);
						if (Cell == Removed) Value.Removable = Piece;
					}
				}
		Require(Value.Cells.size() == 296, "invalid construction");
		if (Case == "BlockGrid") {
			blockgrid::Field Field;
			Require(Field.Apply(Writes), "initial grid failed");
			Value.Grid = std::make_shared<BlockGrid>();
			Value.Grid->SetName("Construction");
			Value.Grid->RestoreField(std::move(Field));
			Value.Grid->SetParent(Value.Collection);
		}
		return Value;
	}

	struct Stamp {
		std::uint64_t Payload{}, Messages{}, Packets{}, PacketBytes{};
		bool HasPacketObservation = false;
	};

	Stamp Capture(const GameNetworkingSocketsTransport &Transport, ConnectionId Connection) {
		const auto Adapter = Transport.GetStatistics(Connection);
		Require(Adapter && Adapter->BytesSent && Adapter->MessagesSent, "missing GNS application counters");
		Stamp Value{*Adapter->BytesSent, *Adapter->MessagesSent};
		const auto Native = detail::ReliableServiceFeedbackAccess::Observe(Transport, Connection);
		if (Native) {
			Value.Packets = Native->NativePacketsSent;
			Value.PacketBytes = Native->NativePacketBytesSent;
			Value.HasPacketObservation = true;
		}
		return Value;
	}

	void WriteTransport(Json &Row, const std::string &Prefix, const Stamp &Start, const Stamp &End) {
		Row[Prefix + "PayloadBytes"] = End.Payload - Start.Payload;
		Row[Prefix + "Messages"] = End.Messages - Start.Messages;
		const bool Observed = End.HasPacketObservation && Start.HasPacketObservation;
		Row[Prefix + "NativePackets"] = Observed ? Json(End.Packets - Start.Packets) : Json(nullptr);
		Row[Prefix + "NativePacketBytes"] = Observed ? Json(End.PacketBytes - Start.PacketBytes) : Json(nullptr);
	}

	void Verify(
		const Scene &Source,
		const std::shared_ptr<Workspace> &Space,
		const std::shared_ptr<Instance> &Collection,
		bool Edited
	) {
		auto Expected = Source.Cells;
		if (Edited) Expected.erase({Removed.X, Removed.Y, Removed.Z});
		if (Source.Grid) {
			auto Grid = std::dynamic_pointer_cast<BlockGrid>(Collection->FindFirstChild("Construction", false));
			Require(Grid && Grid->HasReplicaField() && Collection->GetChildren().size() == 1, "missing grid content");
			for (int Z = -4; Z < 4; ++Z)
				for (int Y = 0; Y < 8; ++Y)
					for (int X = -4; X < 4; ++X)
						Require(
							Grid->GetField().Get({X, Y, Z}).Definition ==
								(Expected.contains({X, Y, Z}) ? blockgrid::Stone : blockgrid::Air),
							"grid receiver differs at a cell"
						);
		} else {
			for (const auto &Child : Collection->GetChildren()) {
				const auto Piece = std::dynamic_pointer_cast<Part>(Child);
				Require(Piece && Piece->GetAnchored() && Piece->GetSize() == glm::vec3(1), "invalid received Part");
				const auto P = Piece->GetCFrame().Position - glm::vec3(.5f);
				const glm::ivec3 Cell(P);
				Require(
					P == glm::vec3(Cell) && Expected.erase({Cell.x, Cell.y, Cell.z}) == 1, "unexpected received Part"
				);
			}
			Require(Expected.empty(), "receiver is missing Parts");
		}
		RaycastParams Params;
		Params.IncludeBlockGrids = true;
		Params.FilterType = Enums::RaycastFilterType::Include;
		Params.FilterDescendantsInstances.Values = {Collection};
		auto Hit = Space->ResolveRaycast({-3.5f, 10, -3.5f}, {0, -12, 0}, Params);
		if (Source.Cells.empty())
			Require(!Hit.HasHit(), "empty control returned a construction hit");
		else
			Require(
				Hit.HasHit() && std::abs(Hit.Position.y - (Edited ? 7.f : 8.f)) < .02f,
				"received physical geometry differs at the edited cell"
			);
	}

	void Run(std::string_view Case, std::uint16_t Port) {
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto SceneValue = Author(Space, Case);
		HeadlessRenderer ServerView({64, 64}), ClientView({64, 64});
		Engine Runtime(World, &ServerView, nullptr, {.Mode = RuntimeMode::NetworkServer, .AdmitSpatialContent = true});
		Runtime.ProcessService->Alive = true;
		auto Out = std::make_shared<GameNetworkingSocketsTransport>();
		auto In = std::make_shared<GameNetworkingSocketsTransport>();
		const auto Limits = GameSessionConfiguration::DefaultLimits();
		const TransportEndpoint Endpoint{"127.0.0.1", Port};
		GameSession Server(
			Out,
			{.Role = GameSessionRole::Server, .Endpoint = Endpoint, .Limits = Limits, .SpatialContent = true},
			&Runtime
		);
		GameSession Client(
			In, {.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .SpatialContent = true}
		);
		std::unique_ptr<Engine> RemoteRuntime;
		std::uint64_t Tick{}, StartTick{};
		double ServerUs{}, ClientUs{};
		auto Start = Clock::now();
		Require(Server.Start().Succeeded() && Client.Start().Succeeded(), "unable to start private loopback trial");
		auto Pump = [&] {
			const auto T = ++Tick;
			auto Mark = Clock::now();
			(void)Server.Poll();
			Server.Step(T);
			ServerUs += Microseconds(Mark);
			Mark = Clock::now();
			(void)Client.Poll();
			Client.Step(T);
			ClientUs += Microseconds(Mark);
			std::this_thread::sleep_for(2ms);
		};
		auto Wait = [&](auto Predicate) {
			const auto Deadline = Clock::now() + 10s;
			while (!Predicate()) {
				if (Clock::now() >= Deadline || Client.GetStatus() == GameSessionStatus::Failed ||
					Server.GetStatus() == GameSessionStatus::Failed)
					throw std::runtime_error(
						"loopback convergence failed, server=" + Server.GetFailure() + ", client=" + Client.GetFailure()
					);
				Pump();
			}
		};
		Wait([&] { return Client.GetStatus() == GameSessionStatus::Accepted; });
		RemoteRuntime = std::make_unique<Engine>(
			Client.GetClientDataModel(),
			&ClientView,
			nullptr,
			EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitSpatialContent = true}
		);
		RemoteRuntime->ProcessService->Alive = true;
		Require(Client.AttachClientRuntime(*RemoteRuntime), "runtime attachment failed");
		Wait([&] { return Server.GetMetrics().ReadyPeers == 1; });
		const auto Accepted = Server.GetPeerIdentities();
		const auto Connection = Client.GetPrimaryConnection();
		Require(Accepted.size() == 1 && Connection, "missing single admitted peer");
		const std::array Focus{glm::vec3(0, 4, 0)};
		Require(Server.SetTrustedReplicationFocus(Accepted.front().Connection, Focus), "construction interest failed");
		auto ReplicaSpace = std::dynamic_pointer_cast<Workspace>(RemoteRuntime->DataModel->GetService("Workspace"));
		std::shared_ptr<Instance> Replica;
		auto Ready = [&](bool Edited) {
			if (!Replica) Replica = ReplicaSpace->FindFirstChild("BenchmarkConstruction", false);
			if (!Replica) return false;
			if (!SceneValue.Grid)
				return Replica->GetChildren().size() ==
					   SceneValue.Cells.size() - (Edited && SceneValue.Removable ? 1 : 0);
			auto Grid = std::dynamic_pointer_cast<BlockGrid>(Replica->FindFirstChild("Construction", false));
			return Grid && Grid->HasReplicaField() &&
				   Grid->GetField().Get(Removed).Definition == (Edited ? blockgrid::Air : blockgrid::Stone);
		};
		Wait([&] { return Ready(false); });
		Stamp ServerStart, ClientStart;
		ServerStart.HasPacketObservation = ClientStart.HasPacketObservation = true;
		auto Finish = [&](std::string_view Phase) {
			for (int I = 0; I < 8; ++I)
				Pump();
			const auto ServerNow = Capture(*Out, Accepted.front().Connection);
			const auto ClientNow = Capture(*In, *Connection);
			Json Row{
				{"Representation", Case},
				{"Phase", Phase},
				{"Occupied", SceneValue.Cells.size()},
				{"PumpCalls", Tick - StartTick},
				{"ElapsedUs", Microseconds(Start)},
				{"ServerPumpUs", ServerUs},
				{"ClientPumpUs", ClientUs}
			};
			WriteTransport(Row, "Server", ServerStart, ServerNow);
			WriteTransport(Row, "Client", ClientStart, ClientNow);
			std::cout << Row.dump() << '\n';
			ServerStart = ServerNow;
			ClientStart = ClientNow;
			StartTick = Tick;
			ServerUs = ClientUs = 0;
		};
		Finish("Join");
		Verify(SceneValue, ReplicaSpace, Replica, false);
		Start = Clock::now();
		Finish("Idle");
		Start = Clock::now();
		if (SceneValue.Grid) {
			auto Edit = std::make_shared<blockgrid::EditRequest>();
			Edit->Writes.push_back({blockgrid::SingleCell(Removed), {}});
			MutationGateway Gateway;
			Require(
				Gateway
					.Apply(
						SubmitBlockGridEditCommand{SceneValue.Grid->GetObjectId(), World->GetObjectId(), Edit},
						ScriptSecurityContext::CoreTrusted()
					)
					.Succeeded(),
				"one-cell destruction failed"
			);
		} else if (SceneValue.Removable)
			SceneValue.Removable->Destroy();
		Wait([&] { return Ready(true); });
		Finish("Delete");
		Verify(SceneValue, ReplicaSpace, Replica, true);
		Client.Stop();
		Server.Stop();
		RemoteRuntime->Destroy();
		Runtime.Destroy();
	}
}
int main(int Count, char **Args) try {
	if (Count != 3) throw std::invalid_argument("expected Empty, BlockGrid or Parts and a private loopback port");
	const std::string Case = Args[1];
	const auto Port = std::stoul(Args[2]);
	Require(
		(Case == "Empty" || Case == "BlockGrid" || Case == "Parts") && Port && Port < 65536, "invalid trial arguments"
	);
	gargantuan::BootstrapNativeRuntimeSchema();
	Run(Case, static_cast<std::uint16_t>(Port));
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:NetworkBenchmark] " << Error.what() << '\n';
	return 1;
}
