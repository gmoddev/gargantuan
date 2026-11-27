// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/Workspace.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

namespace gargantuan {
	struct EditorViewportRendererTestAccess {
		static SDLRendererMetrics GetMetrics(EditorViewportRenderer &Renderer) { return Renderer.GetMetricsForTesting(); }
	};
}

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	constexpr std::uint32_t Width = 384, Height = 256;
	void Require(bool Ok, const char *Message) {
		if (!Ok) throw std::runtime_error(Message);
	}
	void Run(bool Solid, bool UseGrid, bool Sample) {
		SDL_SetHintWithPriority(SDL_HINT_GPU_DRIVER, "vulkan", SDL_HINT_OVERRIDE);
		Require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
		struct Video {
			~Video() {
				SDL_Quit();
			}
		} Lifetime;
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto FolderValue = std::make_shared<Folder>();
		FolderValue->SetParent(World);
		const int Xs = Solid ? 8 : 16, Ys = Solid ? 4 : 1, Zs = Solid ? 8 : 32;
		const glm::vec3 Center(Xs * .5f, Ys * .5f, Zs * .5f);
		const auto Camera = MakeLookAtRenderCameraInput(Center + glm::vec3(11, 26, 32), Center, {0, 1, 0}, 52);
		EditorViewportRenderer View(Width, Height);
		RenderPublisher Publisher;
		std::vector<std::uint8_t> Blank, Pixels, Initial;
		auto Stage = [&](const char *Name, unsigned Frames) {
			auto Measurements = Json::array();
			const auto Before = EditorViewportRendererTestAccess::GetMetrics(View);
			for (unsigned I = 0; I < Frames; ++I) {
				const auto Start = std::chrono::steady_clock::now();
				auto Publication = Publisher.Publish(*World, Camera, Width, Height);
				const auto PublishUs =
					std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
				const auto Frame = View.CaptureBgra(std::move(Publication));
				Require(
					Frame.Width == Width && Frame.Height == Height && Frame.BgraPixels.size() == Width * Height * 4,
					"incomplete GPU readback"
				);
				Measurements.push_back(
					{{"PublishUs", PublishUs},
					 {"SubmissionUs", Frame.RenderSubmission.count()},
					 {"FenceReadbackUs", Frame.GpuReadbackWait.count()},
					 {"ExtractUs", Frame.CpuExtraction.count()},
					 {"CaptureUs", Frame.Total.count()}}
				);
				if (I + 1 == Frames) Pixels.assign(Frame.BgraPixels.begin(), Frame.BgraPixels.end());
			}
			if (Blank.empty()) Blank = Pixels;
			std::uint64_t CoverageHash = 14695981039346656037ull;
			unsigned Covered = 0;
			for (std::size_t I = 0; I < Pixels.size(); I += 4) {
				const bool Changes = Pixels[I] != Blank[I] || Pixels[I + 1] != Blank[I + 1] ||
									 Pixels[I + 2] != Blank[I + 2];
				Covered += Changes;
				CoverageHash = (CoverageHash ^ static_cast<unsigned>(Changes)) * 1099511628211ull;
			}
			const auto After = EditorViewportRendererTestAccess::GetMetrics(View);
			Json Row{
				{"Layout", Solid ? "Solid" : "Checkerboard"},
				{"Representation", UseGrid ? "BlockGrid" : "Parts"},
				{"Stage", Name},
				{"CoveredPixels", Covered},
				{"CoverageHash", std::to_string(CoverageHash)},
				{"DrawCalls", After.DrawCalls - Before.DrawCalls},
				{"UploadedBytes", After.UploadedBytes - Before.UploadedBytes},
				{"VertexBufferCreates", After.VertexBufferCreations - Before.VertexBufferCreations},
				{"Frames", std::move(Measurements)}
			};
			std::cout << Row.dump() << std::endl;
			if (Sample) {
				std::string Ack;
				Require(std::getline(std::cin, Ack) && Ack == "continue", "missing OS counter sample acknowledgment");
			}
		};
		Stage("Empty", 1);
		std::vector<blockgrid::Write> Writes;
		std::shared_ptr<BlockGrid> Grid;
		std::shared_ptr<Part> Removed;
		const blockgrid::Cell Target{Xs - 2, Ys - 1, Zs - 2};
		for (int Z = 0; Z < Zs; ++Z)
			for (int Y = 0; Y < Ys; ++Y)
				for (int X = 0; X < Xs; ++X) {
					if (!Solid && (X + Z) % 2) continue;
					const blockgrid::Cell Cell{X, Y, Z};
					Writes.push_back({blockgrid::SingleCell(Cell), {blockgrid::Stone, 0}});
					if (!UseGrid) {
						auto Piece = std::make_shared<Part>();
						Piece->SetSize({1, 1, 1});
						Piece->SetAnchored(true);
						Piece->SetColor(Color3(139.f / 255.f, 144.f / 255.f, 151.f / 255.f));
						Piece->SetCFrame(CFrame(X + .5f, Y + .5f, Z + .5f));
						Piece->SetParent(FolderValue);
						if (Cell == Target) Removed = Piece;
					}
				}
		Require(Writes.size() == 256, "invalid authored geometry");
		if (UseGrid) {
			Grid = std::make_shared<BlockGrid>();
			blockgrid::Field Field;
			Require(Field.Apply(Writes), "grid construction failed");
			Grid->RestoreField(std::move(Field));
			Grid->SetParent(FolderValue);
		}
		Stage("Cold", 1);
		Initial = Pixels;
		Require(Initial != Blank, "geometry produced no pixels");
		Stage("Warm", 105);
		Require(Pixels == Initial, "stationary frames changed");
		if (Grid) {
			auto Edit = std::make_shared<blockgrid::EditRequest>();
			Edit->Writes.push_back({blockgrid::SingleCell(Target), {}});
			MutationGateway Gateway;
			Require(
				Gateway
					.Apply(
						SubmitBlockGridEditCommand{Grid->GetObjectId(), Game->GetObjectId(), Edit},
						ScriptSecurityContext::CoreTrusted()
					)
					.Succeeded(),
				"cell removal failed"
			);
		} else {
			Require(bool(Removed), "missing authored Part");
			Removed->Destroy();
		}
		Stage("Edit", 1);
		Require(Pixels != Initial, "removed cell produced no pixel change");
		FolderValue->Destroy();
		Stage("Retired", 1);
		Require(Pixels == Blank, "retired scene retained pixels");
		Game->Destroy();
	}
}
int main(int Count, char **Args) try {
	if (Count != 3 && Count != 4)
		throw std::invalid_argument("expected solid|checkerboard grid|parts [--sample-counters]");
	const std::string Layout = Args[1], Representation = Args[2];
	Require(
		(Layout == "solid" || Layout == "checkerboard") && (Representation == "grid" || Representation == "parts") &&
			(Count == 3 || std::string_view(Args[3]) == "--sample-counters"),
		"unsupported GPU benchmark arguments"
	);
	gargantuan::BootstrapNativeRuntimeSchema();
	Run(Layout == "solid", Representation == "grid", Count == 4);
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:GpuBenchmark] " << Error.what() << '\n';
	return 1;
}
