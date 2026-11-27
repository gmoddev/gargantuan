// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	template <typename Operation> double TimeUs(Operation &&Run) {
		const auto Start = std::chrono::steady_clock::now();
		Run();
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
	}

	void Require(bool Value, const char *Message) {
		if (!Value) throw std::runtime_error(Message);
	}

	Json Measure(unsigned Count, bool UseGrid, unsigned Trial) {
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		World->SetGravity(20.f);
		auto Ground = std::make_shared<Part>();
		Ground->SetSize({64, 1, 64});
		Ground->SetCFrame(CFrame(8, -0.5, 4));
		Ground->SetAnchored(true);
		Ground->SetParent(World);
		std::shared_ptr<BlockGrid> Grid;
		std::vector<std::shared_ptr<Part>> Parts;
		std::vector<blockgrid::Cell> Cells;
		for (unsigned I = 0; I < Count; ++I)
			Cells.push_back({static_cast<int>(2 * (I % 8)), 0, static_cast<int>(2 * (I / 8))});
		const auto Center = [](blockgrid::Cell Cell) { return glm::vec3(Cell.X + 0.5f, Cell.Y + 0.5f, Cell.Z + 0.5f); };
		Json Result{{"Trial", Trial}, {"Cubes", Count}, {"Representation", UseGrid ? "BlockGrid" : "LockedJointParts"}};
		Result["PrepareUs"] = TimeUs([&] {
			if (UseGrid) {
				Grid = std::make_shared<BlockGrid>();
				Grid->SetCFrame(CFrame(0, 4, 0));
				Grid->SetParent(World);
				MutationGateway Gateway;
				auto Request = std::make_shared<blockgrid::EditRequest>();
				for (const auto Cell : Cells)
					Request->Writes.push_back({blockgrid::SingleCell(Cell), {blockgrid::Stone, 0}});
				auto Submit = [&](const auto &Edit) {
					auto Admission = Gateway.Apply(
						SubmitBlockGridEditCommand{Grid->GetObjectId(), Game->GetObjectId(), Edit},
						ScriptSecurityContext::CoreTrusted()
					);
					if (!Admission.Succeeded()) throw std::runtime_error(Admission.Message);
				};
				Submit(Request);
				auto Motion = std::make_shared<blockgrid::EditRequest>();
				Motion->ExpectedRevision = Grid->GetBlockRevision();
				Motion->Motion = blockgrid::MotionEdit{true};
				Submit(Motion);
			} else {
				for (const auto Cell : Cells) {
					auto Piece = std::make_shared<Part>();
					Piece->SetSize({1, 1, 1});
					Piece->SetCFrame(CFrame(Center(Cell) + glm::vec3(0, 4, 0)));
					Piece->SetParent(World);
					if (!Parts.empty()) {
						const auto Neighbor = Parts.size() % 8 == 0 ? Parts.size() - 8 : Parts.size() - 1;
						const auto Offset = (Piece->GetCFrame().Position - Parts[Neighbor]->GetCFrame().Position) / 2.f;
						auto A = std::make_shared<Attachment>();
						auto B = std::make_shared<Attachment>();
						A->SetCFrame(CFrame(Offset));
						B->SetCFrame(CFrame(-Offset));
						A->SetParent(Parts[Neighbor]);
						B->SetParent(Piece);
						auto Joint = std::make_shared<JointConstraint>();
						Joint->SetJointType("Hinge");
						Joint->SetEnableLimit(true);
						Joint->SetLowerLimit(0.f);
						Joint->SetUpperLimit(0.f);
						Joint->SetAttachment0(A);
						Joint->SetAttachment1(B);
						Joint->SetParent(World);
					}
					Parts.push_back(std::move(Piece));
				}
			}
		});
		std::vector<double> Steps;
		Steps.reserve(180);
		for (unsigned Frame = 0; Frame < 180; ++Frame) {
			Steps.push_back(TimeUs([&] { World->StepPhysics(1. / 60, std::nullopt); }));
			if (Frame == 0) Result["FirstStepUs"] = Steps.front();
		}
		double Sum = 0;
		for (const auto Value : Steps)
			Sum += Value;
		Result["Physics180Us"] = Sum;
		std::sort(Steps.begin(), Steps.end());
		Result["StepP50Us"] = Steps[89];
		Result["StepP95Us"] = Steps[170];
		Result["StepP99Us"] = Steps[178];
		Result["StepMaxUs"] = Steps[179];
		const auto Projection = Grid ? Grid->GetAssemblyProjection() : std::optional<blockgrid::AssemblyProjection>{};
		if (Grid)
			Require(
				Projection && Projection->Dynamic && blockgrid::ValidAssemblyProjection(*Projection),
				"missing authoritative body"
			);
		double Mass = Grid ? Projection->Mass : 0.;
		if (!Grid)
			for (const auto &Piece : Parts)
				Mass += Piece->GetMass();
		const double UnitDensity = Grid ? 1. : .7;
		Require(std::abs(Mass - Count * UnitDensity) < .02, "aggregate mass differs from the native material density");
		Result["UnitDensity"] = UnitDensity;
		Result["Mass"] = Mass;
		const auto Position = [&](std::size_t Index) {
			return Grid ? Projection->Pose.PointToWorldSpace(Center(Cells[Index])) : Parts[Index]->GetCFrame().Position;
		};
		const auto Origin = Position(0);
		double FloorError = 0, RigidError = 0;
		RaycastParams Params;
		Params.IncludeBlockGrids = UseGrid;
		for (std::size_t I = 0; I < Cells.size(); ++I) {
			const auto P = Position(I);
			FloorError = std::max(FloorError, std::abs(static_cast<double>(P.y) - .5));
			RigidError = std::max(
				RigidError,
				std::abs(
					static_cast<double>(glm::length(P - Origin) - glm::length(Center(Cells[I]) - Center(Cells[0])))
				)
			);
			const auto Hit = World->ResolveRaycast(P + glm::vec3(0, 3, 0), {0, -6, 0}, Params);
			const bool Matched = Grid ? Hit.GridInstance == Grid &&
				Hit.Cell == std::optional(glm::ivec3(Cells[I].X, Cells[I].Y, Cells[I].Z)) : Hit.Instance == Parts[I];
			if (!Hit.HasHit() || !Matched)
				throw std::runtime_error("settled shape lost its physical ray owner: " + Result.dump() +
					" cell=" + std::to_string(I) + " y=" + std::to_string(P.y) +
					" separationError=" + std::to_string(RigidError));
		}
		Result["MaximumFloorError"] = FloorError;
		Result["MaximumSeparationError"] = RigidError;
		Require(FloorError < .14 && RigidError < .18, "settled assembly lost physical support or rigidity");
		Result["DestroyTreeUs"] = TimeUs([&] { Game->Destroy(); });
		return Result;
	}
}
int main() try {
	gargantuan::BootstrapNativeRuntimeSchema();
	gargantuan::ScriptSecurityScope Authority(gargantuan::ScriptSecurityContext::CoreTrusted());
	for (unsigned Trial = 1; Trial <= 9; ++Trial)
		for (unsigned Count : {2u, 8u, 32u})
			for (bool Grid : {Trial % 2 != 0, Trial % 2 == 0})
				std::cout << Measure(Count, Grid, Trial).dump() << '\n';
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:MotionBenchmark] " << Error.what() << '\n';
	return 1;
}
