// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	template <typename F> double Microseconds(F &&Operation) {
		const auto Start = std::chrono::steady_clock::now();
		Operation();
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
	}

	struct Layout {
		const char *Name;
		int Width, Height, Depth;
		std::vector<blockgrid::Cell> Cells;
		std::vector<int> Tops;

		explicit Layout(bool Compact)
			: Name(Compact ? "Solid" : "Checkerboard"), Width(Compact ? 8 : 16), Height(Compact ? 4 : 1),
			  Depth(Compact ? 8 : 32), Tops(Width * Depth, -1) {
			for (int Z = 0; Z < Depth; ++Z) {
				for (int Y = 0; Y < Height; ++Y) {
					for (int X = 0; X < Width; ++X) {
						if (!Compact && (X + Z) % 2) continue;
						Cells.push_back({X, Y, Z});
						Tops[Z * Width + X] = Y;
					}
				}
			}
			if (Cells.size() != 256) throw std::logic_error("layout must contain 256 occupied cells");
		}
	};

	void Submit(const std::shared_ptr<BlockGrid> &Grid, std::vector<blockgrid::Write> Writes) {
		auto Request = std::make_shared<blockgrid::EditRequest>();
		Request->Writes = std::move(Writes);
		MutationGateway Gateway;
		auto Result = Gateway.Apply(
			SubmitBlockGridEditCommand{Grid->GetObjectId(), Grid->GetDataModel()->GetObjectId(), Request},
			ScriptSecurityContext::CoreTrusted()
		);
		if (!Result.Succeeded()) throw std::runtime_error(Result.Message);
	}

	Json Run(const Layout &Site, bool UseGrid, const std::filesystem::path &Projects, unsigned Trial) {
		const std::string Type = UseGrid ? "Grid" : "Parts";
		Json Result{{"Layout", Site.Name}, {"Representation", Type}, {"Trial", Trial}, {"Occupied", Site.Cells.size()}};
		auto Game = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto FolderValue = std::make_shared<Folder>();
		FolderValue->SetName("Structure");
		FolderValue->SetParent(Space);
		std::shared_ptr<BlockGrid> Grid;
		std::vector<std::shared_ptr<Part>> Parts;
		Result["BuildUs"] = Microseconds([&] {
			if (UseGrid) {
				Grid = std::make_shared<BlockGrid>();
				Grid->SetParent(FolderValue);
				std::vector<blockgrid::Write> Writes;
				if (Site.Height > 1)
					Writes.push_back({{{0, 0, 0}, {Site.Width, Site.Height, Site.Depth}}, {blockgrid::Stone, 0}});
				else {
					Writes.reserve(Site.Cells.size());
					for (const auto Cell : Site.Cells)
						Writes.push_back({blockgrid::SingleCell(Cell), {blockgrid::Stone, 0}});
				}
				Submit(Grid, std::move(Writes));
			} else {
				Parts.reserve(Site.Cells.size());
				for (const auto Cell : Site.Cells) {
					auto Piece = std::make_shared<Part>();
					Piece->SetSize({1, 1, 1});
					Piece->SetAnchored(true);
					Piece->SetColor(Color3(139.f / 255.f, 144.f / 255.f, 151.f / 255.f));
					Piece->SetCFrame(CFrame(Cell.X + 0.5f, Cell.Y + 0.5f, Cell.Z + 0.5f));
					Piece->SetParent(FolderValue);
					Parts.push_back(std::move(Piece));
				}
			}
		});
		Result["AuthoredObjects"] = FolderValue->GetDescendants().size();
		if (Grid) {
			Result["NativeBytes"] = blockgrid::Encode(Grid->GetField()).size();
			Result["Leaves"] = Grid->GetField().GetLeafCount();
		}

		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Camera = MakeLookAtRenderCameraInput({10, 17, 47}, {8, 1, 12}, {0, 1, 0}, 75);
		const auto Publish = [&](std::string Phase) {
			RenderProjectionChanges Changed;
			Result[Phase + "PublishUs"] = Microseconds([&] {
				const auto Packet = Publisher.Publish(*Space, Camera, 800, 600);
				if (!Packet || !Packet->VisualAttachmentsComplete)
					throw std::runtime_error("incomplete render publication");
				Changed = Projection.Apply(*Packet);
			});
			Result[Phase + "MeshBytes"] = Changed.VertexUploadBytes;
			Result[Phase + "ObjectsChanged"] = Changed.Created + Changed.Updated + Changed.Removed;
		};
		Publish("Cold");
		Result["DrawObjects"] = Projection.GetSize();
		if (!Projection.GetSize()) throw std::runtime_error("scene published no geometry");
		Publish("Warm");

		RaycastParams Params;
		Params.IncludeBlockGrids = UseGrid;
		const auto Ray = [&](int X, int Z, int Top, const glm::vec3 &Offset) {
			const auto Hit = Space->ResolveRaycast(glm::vec3(X + 0.5f, 12, Z + 0.5f) + Offset, {0, -20, 0}, Params);
			if (!Hit.Succeeded() || Hit.HasHit() != (Top >= 0)) throw std::runtime_error("ray occupancy disagrees");
			if (Top < 0) return 0;
			if (std::abs(Hit.Position.y - (Top + 1.f + Offset.y)) > 0.01f || Hit.Normal.y < 0.9f ||
				(UseGrid && (Hit.GridInstance != Grid || Hit.Cell != std::optional(glm::ivec3(X, Top, Z)))) ||
				(!UseGrid && !Hit.Instance))
				throw std::runtime_error("ray semantic identity or distance disagrees");
			return 1;
		};
		Result["ColdRayUs"] = Microseconds([&] { (void)Ray(0, 0, Site.Tops[0], {}); });
		int Hits = 0;
		const auto WarmQueries = [&] {
			// This odd permutation visits each checkerboard location once and repeats the small footprint evenly.
			for (int I = 0; I < 512; ++I) {
				const auto Index = (I * 173) % (Site.Width * Site.Depth);
				Hits += Ray(Index % Site.Width, Index / Site.Width, Site.Tops[Index], {});
			}
		};
		Result["Warm512RaysUs"] = Microseconds(WarmQueries);
		Result["Hits"] = Hits;
		if (!UseGrid) {
			Params.IncludeBlockGrids = true;
			Hits = 0;
			Result["OrdinaryGridOptIn512RaysUs"] = Microseconds(WarmQueries);
			if (Hits != Result["Hits"].get<int>())
				throw std::runtime_error("ordinary world result changed with opt-in");
			Params.IncludeBlockGrids = false;
		}

		Game->MarkPersistenceSubtreeArchivable();
		const std::array<std::shared_ptr<Instance>, 1> Roots{FolderValue};
		Result["ModelEncodeUs"] = Microseconds([&] {
			const auto Document = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Model).value();
			Result["ModelBytes"] = Gas::Encode(Document).value().size();
		});
		if (Trial == 1 && !Projects.empty()) {
			const auto Destination = Projects / (std::string(Site.Name) + Type);
			if (!std::filesystem::create_directory(Destination)) throw std::runtime_error("project destination exists");
			DiskFilesystem Files(Destination);
			(void)Project::fromInit(&Files, "Spatial performance", Game);
		}

		const auto Last = Site.Cells.back();
		Result["OneCellEditUs"] = Microseconds([&] {
			if (Grid)
				Submit(Grid, {{blockgrid::SingleCell(Last), {}}});
			else {
				Parts.back()->Destroy();
				Parts.pop_back();
			}
		});
		Publish("Edit");
		const int NextTop = Site.Height == 1 ? -1 : Last.Y - 1;
		Result["EditRayUs"] = Microseconds([&] { (void)Ray(Last.X, Last.Z, NextTop, {}); });
		const glm::vec3 Offset(3, 2, 1);
		Result["PoseMutationUs"] = Microseconds([&] {
			if (Grid)
				Grid->SetCFrame(CFrame(Offset));
			else
				for (auto &Piece : Parts) {
					auto Pose = Piece->GetCFrame();
					Pose.Position += Offset;
					Piece->SetCFrame(Pose);
				}
		});
		Publish("Pose");
		if (Result["PoseMeshBytes"].get<std::size_t>() != 0) throw std::runtime_error("pose change uploaded geometry");
		Result["PoseRayUs"] = Microseconds([&] { (void)Ray(0, 0, Site.Tops[0], Offset); });
		Game->Destroy();
		return Result;
	}
}

int main(int Count, char **Arguments) try {
	gargantuan::BootstrapNativeRuntimeSchema();
	std::filesystem::path Projects;
	if (Count == 3 && std::string_view(Arguments[1]) == "--projects") {
		Projects = Arguments[2];
		if (!std::filesystem::create_directory(Projects))
			throw std::runtime_error("benchmark project root must be new");
	} else if (Count != 1)
		throw std::runtime_error("usage: gargantuan_spatial_construction_benchmark [--projects <new directory>]");
	for (unsigned Trial = 1; Trial <= 7; ++Trial) {
		for (const bool Compact : {true, false}) {
			const Layout Site(Compact);
			for (const bool Grid : {Trial % 2 != 0, Trial % 2 == 0})
				std::cout << Run(Site, Grid, Projects, Trial).dump() << '\n';
		}
	}
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:Benchmark] " << Error.what() << '\n';
	return 1;
}
