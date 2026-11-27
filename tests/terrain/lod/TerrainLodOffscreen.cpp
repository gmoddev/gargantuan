// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/services/Lighting.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/ProcessService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "terrain/RenderStage.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::terrain;
	using Clock = std::chrono::steady_clock;
	constexpr int Width = 640;
	constexpr int Height = 360;

	void Require(bool Result, std::string_view Message) {
		if (!Result) throw std::runtime_error(std::string(Message));
	}

	class CaptureRenderer final : public BaseRenderer {
	public:
		struct TerrainFrameWork {
			std::size_t Created{};
			std::size_t Retired{};
			std::size_t UploadBytes{};
			bool Rebound{};
		};

		CaptureRenderer() : Renderer(Vector2(Width, Height), {.Offscreen = true, .WaitForGpuCompletion = true}),
			Viewport(Width, Height) {}

		void Draw(RenderPublicationPtr Publication) override {
			LastTerrain = {.Rebound = static_cast<bool>(Publication->TerrainDraws)};
			for (const auto &Mesh : Publication->MeshRemoves)
				LastTerrain.Retired += Mesh.Mesh.Domain == RenderMeshDomain::Terrain;
			for (const auto &Mesh : Publication->MeshCreates) {
				if (Mesh.Mesh.Domain != RenderMeshDomain::Terrain) continue;
				++LastTerrain.Created;
				LastTerrain.UploadBytes += Mesh.Vertices->size() * sizeof(RenderVertex) +
					Mesh.Indices->size() * sizeof(std::uint32_t);
			}
			for (const auto &Mesh : Publication->MeshVertexUpdates)
				if (Mesh.Mesh.Domain == RenderMeshDomain::Terrain)
					LastTerrain.UploadBytes += Mesh.Vertices->size() * sizeof(RenderVertex);
			Retired += LastTerrain.Retired;
			Renderer.Draw(Publication);
			if (CaptureNext) {
				Image = Viewport.Capture(std::move(Publication));
				CaptureNext = false;
			} else {
				Viewport.ApplyPublication(std::move(Publication));
			}
		}
		void Resize(int NewWidth, int NewHeight) override {
			Renderer.Resize(NewWidth, NewHeight);
			Viewport.Resize(NewWidth, NewHeight);
		}
		void Destroy() override { Renderer.Destroy(); }
		RendererCapabilities GetCapabilities() const override { return Renderer.GetCapabilities(); }
		std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override { return Renderer.GetViewportSize(); }
		SDLRenderer Renderer;
		EditorViewportRenderer Viewport;
		EditorViewportFrame Image;
		TerrainFrameWork LastTerrain;
		std::size_t Retired{};
		bool CaptureNext{};
	};

	std::unique_ptr<Field> MakePlateau() {
		auto Result = std::make_unique<Field>();
		for (int X = -1; X <= 1; ++X) {
			for (int Z = -1; Z <= 1; ++Z) {
				const ChunkKey Key{X, 0, Z};
				std::vector<SampleWrite> Writes;
				Writes.reserve(ChunkSide * ChunkSide * 14);
				for (int LocalZ = 0; LocalZ < ChunkSide; ++LocalZ)
					for (int Y = 0; Y < 14; ++Y)
						for (int LocalX = 0; LocalX < ChunkSide; ++LocalX) {
							const auto Density = static_cast<std::int16_t>(
								std::clamp((4 * Y - 37) * 2048, -static_cast<int>(SaturatedDensity),
									static_cast<int>(SaturatedDensity)));
							Writes.push_back({{X * ChunkSide + LocalX, Y, Z * ChunkSide + LocalZ},
								{Density, Density < 0 ? 2u : 0u}});
						}
				auto Snapshot = Result->Capture(std::span{&Key, 1}).value();
				Require(Result->Commit(Field::Prepare(std::move(Snapshot), Writes).value()).has_value(),
					"plateau field admission failed");
			}
		}
		return Result;
	}

	std::unique_ptr<Field> MakeCave() {
		auto Result = std::make_unique<Field>();
		for (int X = -1; X <= 1; ++X) {
			for (int Z = -1; Z <= 1; ++Z) {
				const ChunkKey Key{X, 0, Z};
				std::vector<SampleWrite> Writes;
				Writes.reserve(ChunkSide * ChunkSide * 28);
				for (int LocalZ = 0; LocalZ < ChunkSide; ++LocalZ)
					for (int Y = 0; Y < 28; ++Y)
						for (int LocalX = 0; LocalX < ChunkSide; ++LocalX) {
							const auto WorldX = X * ChunkSide + LocalX;
							const auto Across = std::abs(8 * WorldX - 129);
							const auto Up = std::abs(8 * Y - 92);
							// Chamfered tunnel through Z with intact floor and overhanging roof.
							const auto Tunnel = std::min({82 - Across, 74 - Up, 126 - Across - Up});
							const auto Signed = std::max(8 * Y - 210, Tunnel);
							const auto Density = static_cast<std::int16_t>(std::clamp(Signed * 1024,
								-static_cast<int>(SaturatedDensity), static_cast<int>(SaturatedDensity)));
							Writes.push_back({{WorldX, Y, Z * ChunkSide + LocalZ}, {Density, Density < 0 ? 2u : 0u}});
						}
				auto Snapshot = Result->Capture(std::span{&Key, 1}).value();
				Require(Result->Commit(Field::Prepare(std::move(Snapshot), Writes).value()).has_value(),
					"cave field admission failed");
			}
		}
		return Result;
	}

	std::size_t GetTriangles(const Workspace &Space) {
		const auto State = Space.GetTerrainRenderState();
		std::size_t Count = 0;
		for (const auto &[Key, Chunk] : State->Chunks) {
			static_cast<void>(Key);
			Count += Chunk.Resource.Indices->size() / 3;
		}
		return Count;
	}

	void SaveImage(const std::filesystem::path &Path, const EditorViewportFrame &Image) {
		Require(Image.Width == Width && Image.Height == Height && Image.RgbPixels.size() == Width * Height * 3,
			"offscreen snapshot is incomplete");
		std::ofstream Output(Path, std::ios::binary);
		Output << "P6\n" << Width << ' ' << Height << "\n255\n";
		Output.write(reinterpret_cast<const char *>(Image.RgbPixels.data()), Image.RgbPixels.size());
		Require(Output.good(), "offscreen snapshot could not be written");
	}

	struct RunResult {
		std::size_t FarTriangles{};
		std::size_t NearTriangles{};
		float CollisionHeight{};
	};

	RunResult Run(const std::filesystem::path &Output, bool Mixed) {
		const std::string Name = Mixed ? "mixed" : "fine";
		auto Model = std::make_shared<DataModel>();
		auto Space = std::static_pointer_cast<Workspace>(Model->GetService("Workspace"));
		auto PlayerService = std::static_pointer_cast<Players>(Model->GetService("Players"));
		PlayerService->SetDefaultCameraEnabled(false);
		auto GlobalLighting = std::static_pointer_cast<Lighting>(Model->GetService("Lighting"));
		GlobalLighting->SetShadowCascadeCount(2);
		GlobalLighting->SetShadowDistance(1'200.0f);
		Space->RestoreTerrainField(MakePlateau());
		if (Mixed)
			Require(Space->ApplyAttributeMutation("TerrainVisualDensity", WireValue{true}) == MutationStatus::Success,
				"the authored plateau enables mixed visual detail");
		auto CameraValue = Space->GetCurrentCamera();
		CameraValue->SetCFrame(CFrame(glm::vec3{16, 420, -620}, glm::vec3{16, 9, 16}));
		CaptureRenderer Capture;
		Engine Runtime(Model, &Capture, nullptr);
		Runtime.ProcessService->Alive = true;
		Require(Capture.Renderer.GetDriverName() == "vulkan", "Vulkan offscreen renderer is unavailable");
		for (int Frame = 0; Frame < 90; ++Frame) {
			Runtime.Step();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		Capture.CaptureNext = true;
		Runtime.Step();
		SaveImage(Output / (Name + "-far.ppm"), Capture.Image);
		const auto FarTriangles = GetTriangles(*Space);
		std::ofstream Timing(Output / (Name + "-frames.csv"));
		Timing << "frame,engine_ns,wait_ns,uploaded_bytes,draws,shadow_draws\n";
		for (int Frame = 0; Frame < 91; ++Frame) {
			const auto Start = Clock::now();
			Runtime.Step();
			const auto Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Start).count();
			const auto Stats = Capture.Renderer.GetMetrics();
			Timing << Frame << ',' << Ns << ',' << Stats.LastGpuCompletionWaitNanoseconds << ',' << Stats.UploadedBytes
				<< ',' << Stats.DrawCalls << ',' << Stats.ShadowCasterDraws << '\n';
		}
		CameraValue->SetCFrame(CFrame(glm::vec3{18, 27, -10}, glm::vec3{16, 9, 16}));
		Capture.CaptureNext = true;
		Runtime.Step();
		SaveImage(Output / (Name + "-near.ppm"), Capture.Image);
		const auto NearTriangles = GetTriangles(*Space);
		if (Mixed && NearTriangles > FarTriangles)
			Require(Capture.LastTerrain.Rebound && Capture.LastTerrain.Created == 0 &&
				Capture.LastTerrain.Retired == 0 && Capture.LastTerrain.UploadBytes == 0,
				"plateau approach must rebind the resident fine mesh without transfer");
		RaycastParams Query;
		Query.IncludeTerrain = true;
		const auto Ground = Space->ResolveRaycast({16, 20, 16}, {0, -20, 0}, Query);
		Require(Ground.HasHit(), "the frame trial lost authoritative collision");
		std::cout << "[Terrain:LOD] " << Name << " far_triangles=" << FarTriangles << " near_triangles="
			<< NearTriangles << " retired=" << Capture.Retired << " collision_y=" << Ground.Position.y << '\n';
		Runtime.Destroy();
		return {FarTriangles, NearTriangles, Ground.Position.y};
	}

	struct CaveRay {
		std::string_view Name;
		glm::vec3 Origin;
		glm::vec3 Direction;
		bool ExpectsTerrain;
	};

	const std::array CaveRays{
		CaveRay{"passage_front", {16.125f, 11.5f, -40.f}, {0.f, 0.f, 112.f}, false},
		CaveRay{"passage_rear", {16.125f, 11.5f, 72.f}, {0.f, 0.f, -112.f}, false},
		CaveRay{"roof_front", {16.125f, 23.f, -40.f}, {0.f, 0.f, 36.f}, true},
		CaveRay{"ceiling_inside", {16.125f, 11.5f, 16.25f}, {0.f, 16.f, 0.f}, true},
		CaveRay{"floor_inside", {16.125f, 11.5f, 16.25f}, {0.f, -16.f, 0.f}, true},
		CaveRay{"roof_above", {16.125f, 35.f, 16.25f}, {0.f, -16.f, 0.f}, true},
		CaveRay{"wall_inside", {16.125f, 11.5f, 16.25f}, {20.f, 0.f, 0.f}, true}
	};

	struct CaveContact {
		bool Hit{};
		std::array<float, 7> Values{};
		bool operator==(const CaveContact &) const = default;
	};

	using CaveContacts = std::array<CaveContact, CaveRays.size()>;

	CaveContacts ReadCaveCollision(Workspace &Space) {
		RaycastParams Query;
		Query.IncludeTerrain = true;
		Query.FilterType = Enums::RaycastFilterType::Include;
		Query.FilterDescendantsInstances.Values.push_back(Space.GetTerrain());
		CaveContacts Results{};
		for (std::size_t I = 0; I < Results.size(); ++I) {
			const auto &Ray = CaveRays[I];
			const auto Hit = Space.ResolveRaycast(Ray.Origin, Ray.Direction, Query);
			Require(Hit.Succeeded() && Hit.HasHit() == Ray.ExpectsTerrain &&
				(!Hit.HasHit() || Hit.TerrainInstance == Space.GetTerrain()), "unexpected cave collision at " + std::string(Ray.Name));
			Results[I] = {Hit.HasHit(),
				{Hit.Position.x, Hit.Position.y, Hit.Position.z, Hit.Normal.x, Hit.Normal.y, Hit.Normal.z, Hit.Distance}};
		}
		return Results;
	}

	struct CaveView {
		std::string_view Name;
		glm::vec3 Eye;
		glm::vec3 Target;
		int AreaRadius;
		int WarmFrames;
	};

	// The front and rear cameras view the same passage from opposite ends.
	// The near checkpoints capture the first native frame after each handoff.
	const std::array CaveViews{
		CaveView{"front-far", {8.f, 16.f, -548.f}, {16.125f, 11.5f, -32.f}, 22, 80},
		CaveView{"rear-near", {22.f, 17.f, 82.f}, {16.125f, 11.5f, 63.f}, 134, 0},
		CaveView{"rear-far", {48.f, 20.f, 620.f}, {16.125f, 11.5f, 63.f}, 22, 80},
		CaveView{"front-near", {14.f, 13.5f, -53.f}, {16.125f, 11.5f, -32.f}, 134, 0}
	};

	struct CaveFrame {
		EditorViewportFrame Image;
		CaveContacts Collision;
		std::size_t Triangles{};
		CaptureRenderer::TerrainFrameWork TerrainWork;
	};

	using CaveRun = std::array<CaveFrame, CaveViews.size()>;

	CaveRun RunCave(const std::filesystem::path &Output, bool Mixed) {
		const std::string Name = Mixed ? "cave-mixed" : "cave-fine";
		auto Model = std::make_shared<DataModel>();
		auto Space = std::static_pointer_cast<Workspace>(Model->GetService("Workspace"));
		std::static_pointer_cast<Players>(Model->GetService("Players"))->SetDefaultCameraEnabled(false);
		const auto GlobalLighting = std::static_pointer_cast<Lighting>(Model->GetService("Lighting"));
		GlobalLighting->SetShadowCascadeCount(2);
		GlobalLighting->SetShadowDistance(1'200.0f);
		Space->RestoreTerrainField(MakeCave());
		if (Mixed)
			Require(Space->ApplyAttributeMutation("TerrainVisualDensity", WireValue{true}) == MutationStatus::Success,
				"the authored cave enables mixed visual detail");
		const auto FieldRevision = Space->GetTerrain()->GetField().GetRevision();
		const auto CameraValue = Space->GetCurrentCamera();
		CameraValue->SetCFrame(CFrame(CaveViews.front().Eye, CaveViews.front().Target));
		CaptureRenderer Capture;
		Engine Runtime(Model, &Capture, nullptr);
		Runtime.ProcessService->Alive = true;
		Require(Capture.Renderer.GetDriverName() == "vulkan", "Vulkan cave renderer is unavailable");
		std::ofstream Timing(Output / (Name + "-frames.csv"));
		Timing << "view,frame,engine_ns,wait_ns,terrain_created,terrain_retired,terrain_upload_bytes,terrain_rebound,"
			"terrain_cached_variant_bytes\n";
		std::ofstream Collision(Output / (Name + "-collision.csv"));
		Collision << "view,ray,hit,x,y,z,nx,ny,nz,distance\n";
		Collision.precision(9);
		CaveRun Result{};
		const auto InitialCollision = ReadCaveCollision(*Space);
		for (std::size_t I = 0; I < Result.size(); ++I) {
			const auto &View = CaveViews[I];
			CameraValue->SetCFrame(CFrame(View.Eye, View.Target));
			for (int Frame = 0; Frame <= View.WarmFrames; ++Frame) {
				Capture.CaptureNext = Frame == View.WarmFrames;
				const auto Start = Clock::now();
				Runtime.Step();
				const auto Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Start).count();
				const auto Stats = Capture.Renderer.GetMetrics();
				const auto &Terrain = Capture.LastTerrain;
				Timing << View.Name << ',' << Frame << ',' << Ns << ',' << Stats.LastGpuCompletionWaitNanoseconds <<
					',' << Terrain.Created << ',' << Terrain.Retired << ',' << Terrain.UploadBytes << ',' << Terrain.Rebound
					<< ',' << Space->GetTerrainRenderState()->VisualVariantBytes << '\n';
				if (Frame != View.WarmFrames) std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			SaveImage(Output / (Name + "-" + std::string(View.Name) + ".ppm"), Capture.Image);
			auto &Value = Result[I];
			Value = {std::move(Capture.Image), ReadCaveCollision(*Space), GetTriangles(*Space), Capture.LastTerrain};
			Require(Value.Collision == InitialCollision && Space->GetTerrain()->GetField().GetRevision() == FieldRevision,
				"camera handoff changed authored field or native cavity collision");
			for (std::size_t Ray = 0; Ray < CaveRays.size(); ++Ray) {
				Collision << View.Name << ',' << CaveRays[Ray].Name << ',' << Value.Collision[Ray].Hit;
				for (const auto Axis : Value.Collision[Ray].Values) Collision << ',' << Axis;
				Collision << '\n';
			}
		}
		Require(Timing.good() && Collision.good(), "cave receipts could not be written");
		Runtime.Destroy();
		return Result;
	}

	std::size_t ImageOffset(int X, int Y) {
		return static_cast<std::size_t>((Y * Width + X) * 3);
	}

	int ColorDifference(const EditorViewportFrame &A, std::size_t Left, const EditorViewportFrame &B, std::size_t Right) {
		return std::max({
			std::abs(static_cast<int>(A.RgbPixels[Left]) - B.RgbPixels[Right]),
			std::abs(static_cast<int>(A.RgbPixels[Left + 1]) - B.RgbPixels[Right + 1]),
			std::abs(static_cast<int>(A.RgbPixels[Left + 2]) - B.RgbPixels[Right + 2])
		});
	}

	bool HasStrongEdge(const EditorViewportFrame &Image, int X, int Y) {
		const auto At = ImageOffset(X, Y);
		return ColorDifference(Image, At, Image, ImageOffset(X + 1, Y)) >= 24 ||
			ColorDifference(Image, At, Image, ImageOffset(X, Y + 1)) >= 24;
	}

	std::array<std::size_t, 2> CompareAreaEdges(const EditorViewportFrame &A, const EditorViewportFrame &B, int Radius) {
		std::array<std::size_t, 2> Count{};
		for (int Y = Height / 2 - Radius; Y < Height / 2 + Radius; ++Y) {
			for (int X = Width / 2 - Radius; X < Width / 2 + Radius; ++X) {
				if (!HasStrongEdge(A, X, Y)) continue;
				++Count[0];
				bool Matched = false;
				for (int Dy = -1; Dy <= 1; ++Dy)
					for (int Dx = -1; Dx <= 1; ++Dx)
						Matched |= HasStrongEdge(B, X + Dx, Y + Dy);
				Count[1] += !Matched;
			}
		}
		return Count;
	}

	void QualifyCave(const std::filesystem::path &Output) {
		const auto Fine = RunCave(Output, false);
		const auto Mixed = RunCave(Output, true);
		std::ofstream Comparison(Output / "cave-comparison.csv");
		Comparison << "view,fine_triangles,mixed_triangles,changed_pixels,changed_area_pixels,max_channel_delta,"
			"fine_area_edges,mixed_area_edges,fine_edges_unmatched,mixed_edges_unmatched\n";
		bool Reduced = false;
		for (std::size_t I = 0; I < CaveViews.size(); ++I) {
			const auto &View = CaveViews[I];
			const auto &A = Fine[I];
			const auto &B = Mixed[I];
			std::size_t Changed = 0, AreaChanged = 0;
			int MaximumDelta = 0;
			for (int Y = 0; Y < Height; ++Y) {
				for (int X = 0; X < Width; ++X) {
					const auto At = ImageOffset(X, Y);
					const auto Delta = ColorDifference(A.Image, At, B.Image, At);
					Changed += Delta != 0;
					if (X >= Width / 2 - View.AreaRadius && X < Width / 2 + View.AreaRadius &&
						Y >= Height / 2 - View.AreaRadius && Y < Height / 2 + View.AreaRadius)
						AreaChanged += Delta != 0;
					MaximumDelta = std::max(MaximumDelta, Delta);
				}
			}
			const auto FineEdges = CompareAreaEdges(A.Image, B.Image, View.AreaRadius);
			const auto MixedEdges = CompareAreaEdges(B.Image, A.Image, View.AreaRadius);
			Comparison << View.Name << ',' << A.Triangles << ',' << B.Triangles << ',' << Changed << ',' <<
				AreaChanged << ',' << MaximumDelta << ',' << FineEdges[0] << ',' << MixedEdges[0] << ',' <<
				FineEdges[1] << ',' << MixedEdges[1] << '\n';
			Comparison.flush();
			Require(A.Collision == B.Collision, "fine and mixed cameras changed the exact cavity collision receipt");
			Require(FineEdges[0] != 0 && MixedEdges[0] != 0, "test camera did not capture a visible cavity silhouette");
			if (View.WarmFrames != 0) {
				Reduced |= B.Triangles < A.Triangles;
				Require(FineEdges[1] == 0 && MixedEdges[1] == 0,
					"a strong cave-area edge moved more than one pixel at " + std::string(View.Name));
			} else {
				Require(B.Triangles == A.Triangles && Changed == 0,
					"first near frame failed to restore exact native fine output at " + std::string(View.Name));
				if (Mixed[I - 1].Triangles < Fine[I - 1].Triangles)
					Require(B.TerrainWork.Rebound && B.TerrainWork.Created == 0 && B.TerrainWork.Retired == 0 &&
						B.TerrainWork.UploadBytes == 0, "first near frame transferred a warm cached mesh at " + std::string(View.Name));
			}
		}
		Require(Reduced && Comparison.good(), "cave trial needs accepted mixed-detail output and an exact comparison receipt");
		std::cout << "[Terrain:LOD] Cave fronts and rear camera handoffs preserved matched areas and collision\n";
	}
}

int main(int Count, char **Arguments) try {
	Require(Count == 2, "usage: gargantuan_terrain_lod_offscreen <evidence-directory>");
	const std::filesystem::path Output = Arguments[1];
	std::filesystem::create_directories(Output);
	SDL_SetHintWithPriority(SDL_HINT_GPU_DRIVER, "vulkan", SDL_HINT_OVERRIDE);
	Require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
	struct VideoLifetime { ~VideoLifetime() { SDL_Quit(); } } Video;
	BootstrapNativeRuntimeSchema();
	const auto Fine = Run(Output, false);
	const auto Mixed = Run(Output, true);
	Require(Mixed.FarTriangles < Fine.FarTriangles && Mixed.NearTriangles == Fine.NearTriangles &&
		Mixed.CollisionHeight == Fine.CollisionHeight, "mixed detail or collision parity failed");
	QualifyCave(Output);
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Terrain:LOD] " << Error.what() << '\n';
	return 1;
}
