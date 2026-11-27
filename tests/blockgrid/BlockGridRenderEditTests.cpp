// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace gargantuan;

namespace {
	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void Run() {
		BootstrapNativeRuntimeSchema();
		auto Game = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Grid = std::make_shared<BlockGrid>();
		blockgrid::Field Field;
		std::vector<blockgrid::Write> Writes;
		for (int Z = 0; Z < 32; ++Z)
			for (int X = 0; X < 16; ++X)
				if ((X + Z) % 2 == 0) Writes.push_back({blockgrid::SingleCell({X,0,Z}),{blockgrid::Stone,0}});
		Require(Field.Apply(Writes), "checkerboard seed");
		Grid->RestoreField(std::move(Field));
		Grid->SetParent(Space);
		RenderProjection Projection;
		RenderPublisher Publisher;
		const auto Camera = MakeLookAtRenderCameraInput({10,17,47},{8,1,12},{0,1,0},75);
		const auto Publish = [&] {
			auto Packet = Publisher.Publish(*Space,Camera,320,240);
			Require(Packet && Packet->VisualAttachmentsComplete,"incomplete render publication");
			(void)Projection.Apply(*Packet);
			return Packet;
		};
		const auto Edit = [&](blockgrid::Cell Cell, std::uint16_t Value) {
			auto Request = std::make_shared<blockgrid::EditRequest>();
			Request->Writes.push_back({blockgrid::SingleCell(Cell),{Value,0}});
			MutationGateway Gateway;
			Require(Gateway.Apply(SubmitBlockGridEditCommand{Grid->GetObjectId(),Game->GetObjectId(),Request},
				ScriptSecurityContext::CoreTrusted()).Succeeded(),"cell mutation failed");
		};
		const auto Cold = Publish();
		Require(Cold->MeshCreates.size() == 1, "missing initial grid mesh");
		Edit({4,0,4},blockgrid::Air);
		const auto Deleted = Publish();
		Require(Deleted->MeshCreates.empty() && Deleted->MeshRemoves.empty() && Deleted->MeshVertexUpdates.size() == 1,
			"isolated deletion replaced the grid");
		const auto &Patch = Deleted->MeshVertexUpdates.front();
		Require(Patch.Vertices->size() == 24 && Patch.FirstVertex > 0 &&
			std::ranges::all_of(*Patch.Vertices,[&](const RenderVertex &Vertex) { return Vertex.Position == Patch.Vertices->front().Position; }),
			"removed cell still has a visible face");
		Edit({4,0,4},blockgrid::Brick);
		const auto Restored = Publish();
		Require(Restored->MeshCreates.empty() && Restored->MeshVertexUpdates.size() == 1,"restoration replaced the grid");
		const auto &New = Restored->MeshVertexUpdates.front();
		for (std::size_t I = 0; I < New.Vertices->size(); ++I)
			Require(New.Vertices->at(I).Position == Cold->MeshCreates.front().Vertices->at(Patch.FirstVertex + I).Position &&
				New.Vertices->at(I).Tint[0] == 150,"restored face or tint differs from source");
		Edit({4,0,4},blockgrid::Air);
		(void)Publish();
		Publisher.RequestFullResync();
		const auto Full = Publish();
		Require(Full->FullResync && Full->MeshCreates.size() == 1 &&
			Full->MeshCreates.front().Vertices->size() == 255 * 24,"full resync resurrected removed cells");
		Edit({1,0,0},blockgrid::Stone);
		const auto Joined = Publish();
		Require(Joined->MeshCreates.size() == 1 && Joined->MeshVertexUpdates.empty(),"new shared faces need a topology rebuild");
		Game->Destroy();
	}
}

int main() {
	try { Run(); return 0; }
	catch (const std::exception &Error) { std::cerr << "[BlockGrid:RenderTest] " << Error.what() << '\n'; return 1; }
}
