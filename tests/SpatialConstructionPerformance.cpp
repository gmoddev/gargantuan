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
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#endif

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	void RecordProcessMemory(Json &Result, const std::string &Phase) {
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS_EX Counters{};
		Counters.cb = sizeof(Counters);
		if (!GetProcessMemoryInfo(
				GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Counters), sizeof(Counters)
			))
			throw std::runtime_error("could not read process memory counters");
		Result[Phase + "PrivateCommitBytes"] = Counters.PrivateUsage;
		Result[Phase + "ResidentBytes"] = Counters.WorkingSetSize;
#elif defined(__linux__)
		std::ifstream Status("/proc/self/status");
		if (!Status) throw std::runtime_error("could not read process memory counters");
		std::string Line;
		while (std::getline(Status, Line)) {
			std::istringstream Fields(Line);
			std::string Name, Unit;
			std::uint64_t Kilobytes{};
			if (!(Fields >> Name >> Kilobytes >> Unit) || Unit != "kB") continue;
			if (Name == "VmRSS:") Result[Phase + "ResidentBytes"] = Kilobytes * 1024;
			if (Name == "RssAnon:") Result[Phase + "AnonymousResidentBytes"] = Kilobytes * 1024;
		}
#else
		(void)Result;
		(void)Phase;
#endif
	}

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

		Layout(bool Compact, const std::array<int, 3> &Dimensions, std::size_t Expected)
			: Name(Compact ? "Solid" : "Checkerboard"), Width(Dimensions[0]), Height(Dimensions[1]),
			  Depth(Dimensions[2]), Tops(Width * Depth, -1) {
			for (int Z = 0; Z < Depth; ++Z) {
				for (int Y = 0; Y < Height; ++Y) {
					for (int X = 0; X < Width; ++X) {
						if (!Compact && (X + Z) % 2) continue;
						Cells.push_back({X, Y, Z});
						Tops[Z * Width + X] = Y;
					}
				}
			}
			if (Cells.size() != Expected) throw std::logic_error("constructed workload has the wrong cell count");
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

	Json
	Run(const Layout &Site,
		bool UseGrid,
		const std::filesystem::path &Projects,
		unsigned Trial,
		bool MeasureMemory = false) {
		const std::string Type = UseGrid ? "Grid" : "Parts";
		Json Result{{"Layout", Site.Name}, {"Representation", Type}, {"Trial", Trial}, {"Occupied", Site.Cells.size()}};
		auto Game = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto FolderValue = std::make_shared<Folder>();
		FolderValue->SetName("Structure");
		FolderValue->SetParent(Space);
		std::shared_ptr<BlockGrid> Grid;
		std::vector<std::shared_ptr<Part>> Parts;
		if (MeasureMemory) RecordProcessMemory(Result, "EmptyWorld");
		Result["BuildUs"] = Microseconds([&] {
			if (UseGrid) {
				Grid = std::make_shared<BlockGrid>();
				Grid->SetParent(FolderValue);
				if (Site.Height > 1) {
					Submit(Grid, {{{{0, 0, 0}, {Site.Width, Site.Height, Site.Depth}}, {blockgrid::Stone, 0}}});
				} else {
					for (std::size_t Start = 0; Start < Site.Cells.size(); Start += blockgrid::MaximumCommands) {
						const auto End = std::min(Start + blockgrid::MaximumCommands, Site.Cells.size());
						std::vector<blockgrid::Write> Writes;
						Writes.reserve(End - Start);
						for (auto Index = Start; Index < End; ++Index)
							Writes.push_back({blockgrid::SingleCell(Site.Cells[Index]), {blockgrid::Stone, 0}});
						Submit(Grid, std::move(Writes));
					}
				}
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
		if (MeasureMemory) RecordProcessMemory(Result, "Authored");
		if (Grid) {
			Result["NativeBytes"] = blockgrid::Encode(Grid->GetField()).size();
			Result["Leaves"] = Grid->GetField().GetLeafCount();
			Result["SnapshotAdmissionBytes"] = Grid->GetField().GetSnapshotBytes();
			Result["Channels"] = Grid->GetField().GetChannelCount();
			Result["StateOverrides"] = Grid->GetField().GetOverrideCount();
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
		if (MeasureMemory) RecordProcessMemory(Result, "Projected");
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
		if (MeasureMemory) RecordProcessMemory(Result, "Queried");
		const auto WarmQueries = [&](bool IncludeGrids, const char *Timing) {
			Params.IncludeBlockGrids = IncludeGrids;
			int Hits = 0;
			// The same odd permutation covers every coordinate in the original 256-cell checkerboard.
			Result[Timing] = Microseconds([&] {
				for (int I = 0; I < 512; ++I) {
					const auto Index = (I * 173) % (Site.Width * Site.Depth);
					Hits += Ray(Index % Site.Width, Index / Site.Width, Site.Tops[Index], {});
				}
			});
			return Hits;
		};
		std::optional<int> OptInHits;
		if (!UseGrid && Trial % 2 == 0) OptInHits = WarmQueries(true, "OrdinaryGridOptIn512RaysUs");
		const auto Hits = WarmQueries(UseGrid, "Warm512RaysUs");
		Result["Hits"] = Hits;
		if (!UseGrid) {
			if (!OptInHits) OptInHits = WarmQueries(true, "OrdinaryGridOptIn512RaysUs");
			if (*OptInHits != Hits) throw std::runtime_error("ordinary world result changed with opt-in");
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
		Result["DestroyTreeUs"] = Microseconds([&] { Game->Destroy(); });
		return Result;
	}
}

int main(int Count, char **Arguments) try {
	gargantuan::BootstrapNativeRuntimeSchema();
	std::filesystem::path Projects;
	std::array<int, 3> Solid{8, 4, 8}, Checkerboard{16, 1, 32};
	std::size_t Occupancy = 256;
	unsigned Trials = 7;
	const bool Isolated = Count == 5 && std::string_view(Arguments[1]) == "--profile";
	if (Count == 3 && std::string_view(Arguments[1]) == "--projects") {
		Projects = Arguments[2];
		if (!std::filesystem::create_directory(Projects))
			throw std::runtime_error("benchmark project root must be new");
	} else if (Isolated || (Count == 3 && std::string_view(Arguments[1]) == "--scale")) {
		if (std::string_view(Arguments[2]) == "small") {
			Solid = {2, 4, 2};
			Checkerboard = {4, 1, 8};
			Occupancy = 16;
		} else if (std::string_view(Arguments[2]) == "large") {
			Solid = {16, 8, 16};
			Checkerboard = {64, 1, 64};
			Occupancy = 2048;
		} else if (std::string_view(Arguments[2]) != "medium")
			throw std::runtime_error("supported scales are small, medium and large");
	} else if (Count == 2 && std::string_view(Arguments[1]) == "--tails")
		Trials = 105;
	else if (Count != 1)
		throw std::runtime_error(
			"usage: gargantuan_spatial_construction_benchmark [--projects <new directory> | --tails | "
			"--scale <small|medium|large> | --profile <scale> <solid|checkerboard> <grid|parts>]"
		);
	if (Isolated) {
		const std::string_view LayoutName(Arguments[3]), Representation(Arguments[4]);
		if ((LayoutName != "solid" && LayoutName != "checkerboard") ||
			(Representation != "grid" && Representation != "parts"))
			throw std::runtime_error("unknown layout or representation");
		const bool Compact = LayoutName == "solid";
		const Layout Site(Compact, Compact ? Solid : Checkerboard, Occupancy);
		auto Result = Run(Site, Representation == "grid", Projects, 1, true);
		RecordProcessMemory(Result, "Released");
		std::cout << Result.dump() << '\n';
		return 0;
	}
	for (unsigned Trial = 1; Trial <= Trials; ++Trial) {
		for (const bool Compact : {true, false}) {
			const Layout Site(Compact, Compact ? Solid : Checkerboard, Occupancy);
			for (const bool Grid : {Trial % 2 != 0, Trial % 2 == 0})
				std::cout << Run(Site, Grid, Projects, Trial).dump() << '\n';
		}
	}
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:Benchmark] " << Error.what() << '\n';
	return 1;
}
