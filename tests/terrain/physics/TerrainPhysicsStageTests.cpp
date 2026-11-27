// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include "terrain/PhysicsStage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::terrain;

	void Require(bool Result, std::string_view Message) {
		if (!Result)
			throw std::runtime_error(std::string(Message));
	}

	ChunkPublicationCandidate MakePlane(ChunkKey Key, float Height, bool Geometry = true) {
		CollisionSurfaceMesh Mesh;
		Mesh.Positions = {{0,Height,0}, {32,Height,0}, {32,Height,32}, {0,Height,32}};
		Mesh.Indices = {0,2,1,0,3,2};
		Mesh.TriangleMaterials = {7,7};
		return {Key, nullptr, std::move(Mesh), Geometry};
	}

	PhysicsRaycastHit Ground(const PhysicsWorld &World, float X, float Z = 12.25f) {
		const auto Hit = World.Raycast({{X,7,Z}, {0,-9,0}, {}});
		Require(Hit.Succeeded() && Hit.HasHit(), "downward native ray");
		return Hit.Candidates.front();
	}

	void TestNativeCohort() {
		PhysicsWorldConfig Config;
		Config.CollisionResourceLimit = 3;
		PhysicsWorld World(Config);
		std::unique_ptr<StagedPublication> Late;
		{
			PhysicsStage Adapter(World);
			Require(Adapter.IsHealthy(), "terrain physics starts healthy");
			const std::array Initial{MakePlane({-1,0,0}, 1), MakePlane({0,0,0}, 1)};
			auto Staged = Adapter.Stage({}, Initial);
			Require(Staged.has_value() && Adapter.IsStaging() && Adapter.GetBodyCount() == 0,
				"staged bodies have no published terrain owner");
			Require(World.GetCollisionResidency().ActiveResources == 2, "native reserved cohort");
			const auto Reentered = Adapter.Stage({}, Initial);
			Require(!Reentered && Reentered.error() == EditStatus::Unavailable, "denied staging reentrancy");
			(*Staged)->Install(10);
			Require((*Staged)->IsInstalled() && !Adapter.IsStaging() && Adapter.GetBodyCount() == 2,
				"install cohort maps");
			const auto Left = Ground(World, -0.25f), Right = Ground(World, 0.25f);
			Require(Left.Body != Right.Body && Left.Position.y == 1 && Right.Position.y == 1 &&
				Left.Normal.y > .99f && Right.Normal.y > .99f, "native seam rays");
			const auto LeftOwner = Adapter.FindOwner(Left.Body), RightOwner = Adapter.FindOwner(Right.Body);
			Require(LeftOwner && RightOwner && LeftOwner->Key == ChunkKey{-1,0,0} &&
				RightOwner->SurfaceRevision == 10, "native query maps hidden bodies");

			PhysicsShapeCastRequest Shape;
			Shape.Shape.Kind = PhysicsQueryShapeKind::Sphere;
			Shape.Shape.Transform.Position = {0,4,12.25f};
			Shape.Shape.Radius = 0.4f;
			Shape.Direction = {0,-5,0};
			const auto ShapeHit = World.ShapeCast(Shape);
			Require(ShapeHit.Succeeded() && ShapeHit.HasHit() && ShapeHit.Candidates.front().Normal.y > .9f,
				"native sphere cast overlaps chunk seam");
			Shape.Shape.Transform.Position.y = 1.18f;
			const auto Overlap = World.Overlap({Shape.Shape, {}});
			Require(Overlap.Succeeded() && !Overlap.Bodies.empty(), "native overlap at chunk seam");
			const auto Mover = World.MoveKinematicCapsule({{-1,2.05f,12.25f}, .35f, 2.0f,
				{2,-0.1f,0}, {0,0,0}});
			Require(Mover.Succeeded() && Mover.AppliedTranslation.x > 1.9f && Mover.HasFloor &&
				Mover.FloorNormal.y > .9f,
				"kinematic capsule crosses split terrain surface");

			// The first edit body fits beside the originals. The following body
			// exceeds the real provider's live resource limit.
			const std::array TooMany{MakePlane({-1,0,0}, 2), MakePlane({0,0,0}, 2)};
			const auto Before = World.GetCollisionResidency();
			const auto Denied = Adapter.Stage({}, TooMany);
			const auto After = World.GetCollisionResidency();
			Require(!Denied && Denied.error() == EditStatus::PublicationDenied && !Adapter.IsStaging(),
				"provider failure rolls back a partly prepared cohort");
			Require(After.ActiveResources == 2 && After.ActiveConsumers == 2 &&
				After.CreatedResources == Before.CreatedResources + 1 &&
				After.RetiredResources == Before.RetiredResources + 1 &&
				After.FailedAdmissions == Before.FailedAdmissions + 1, "native accounting after partial denial");
			Require(Ground(World, -.25f).Body == Left.Body && Ground(World, .25f).Body == Right.Body,
				"provider denial retains old collision");

			auto Discarded = Adapter.Stage({}, std::span{TooMany}.first(1));
			Require(Discarded.has_value() && World.GetCollisionResidency().ActiveResources == 3,
				"explicitly discarded native candidate was admitted");
			Discarded->reset();
			Require(!Adapter.IsStaging() && Ground(World, -.25f).Body == Left.Body &&
				World.GetCollisionResidency().ActiveResources == 2, "discard restores prior native view");

			auto Replacement = Adapter.Stage({}, std::span{TooMany}.first(1));
			Require(Replacement.has_value(), "admit one replacement");
			(*Replacement)->Install(11);
			const auto New = Ground(World, -.25f);
			const auto NewOwner = Adapter.FindOwner(New.Body);
			Require(New.Body != Left.Body && New.Position.y == 2 && !World.IsBodyValid(Left.Body) &&
				!Adapter.FindOwner(Left.Body) && NewOwner && NewOwner->SurfaceRevision == 11,
				"install updates canonical ownership and retires old live body");

			auto Paint = MakePlane({-1,0,0}, 2, false);
			Paint.Collision.TriangleMaterials = {8,8};
			const auto BeforePaint = World.GetCollisionResidency();
			auto Painted = Adapter.Stage({}, std::span{&Paint, 1});
			Require(Painted.has_value(), "stage material-only revision");
			(*Painted)->Install(12);
			const auto PaintedOwner = Adapter.FindOwner(New.Body);
			Require(World.GetCollisionResidency().CreatedResources == BeforePaint.CreatedResources &&
				Ground(World, -.25f).Body == New.Body && PaintedOwner && PaintedOwner->SurfaceRevision == 12,
				"paint updates ownership metadata without native recook");

			ChunkPublicationCandidate Cleared{{-1,0,0}, nullptr, {}, true};
			auto Clearing = Adapter.Stage({}, std::span{&Cleared, 1});
			Require(Clearing.has_value(), "empty replacement can retire original");
			(*Clearing)->Install(13);
			Require(!World.Raycast({{-.25f,7,12.25f},{0,-9,0},{}}).HasHit() &&
				!World.IsBodyValid(New.Body) && Adapter.GetBodyCount() == 1, "chunk removal");

			auto Final = Adapter.Stage({}, std::span{TooMany}.first(1));
			Require(Final.has_value(), "staged handle outlives adapter test");
			Late = std::move(*Final);
		}
		Late->Install(14);
		Require(!Late->IsInstalled() && World.GetCollisionResidency().ActiveResources == 0 &&
			World.GetCollisionResidency().ActiveConsumers == 0, "teardown retires old and staged bodies");
	}

	void TestConditionedCook() {
		PhysicsWorld World(PhysicsWorldConfig{});
		PhysicsStage Adapter(World);
		constexpr std::uint32_t Width = 7;
		std::vector<Sample> Samples(Width * Width * Width, AirSample);
		// Hint forced below the provider area floor before interpolation conditioning.
		Samples[(2 * Width + 2) * Width + 2] = {-1,1};
		SurfaceMesh Visual;
		Require(ExtractSurface({Samples,4,4,4,1.0}, {}, Visual) == SurfaceStatus::Success, "closed sliver extraction");
		ChunkPublicationCandidate Chunk{{0,0,0}, nullptr, {}, true};
		for (const auto &Vertex : Visual.Vertices)
			Chunk.Collision.Positions.push_back(Vertex.Position);
		Chunk.Collision.Indices = Visual.Indices;
		Chunk.Collision.TriangleMaterials.resize(Visual.Indices.size() / 3, 1);
		const auto Rejected = Adapter.Stage({}, std::span{&Chunk, 1});
		Require(!Rejected && Rejected.error() == EditStatus::PublicationDenied, "actual provider rejects raw slivers");
		Require(BuildCollisionSurface(Visual, {}, Chunk.Collision) == SurfaceStatus::Success,
			"condition closed slivers without deleting any faces");
		auto Accepted = Adapter.Stage({}, std::span{&Chunk, 1});
		Require(Accepted.has_value(), "actual provider cooks condition-preserved shell");
		(*Accepted)->Install(1);
		const auto Hit = Ground(World, 1.0001f, 1.0001f);
		Require(Adapter.FindOwner(Hit.Body).has_value() && Hit.Position.y > 1.0f, "query conditioned closed shell");
	}

	ChunkPublicationCandidate MakeTunnelChunk(int ChunkX, bool Open) {
		constexpr int Width = ChunkSide + 3;
		std::vector<Sample> Samples;
		Samples.reserve(Width * Width * Width);
		for (int Z = -1; Z <= ChunkSide + 1; ++Z)
			for (int Y = -1; Y <= ChunkSide + 1; ++Y)
				for (int X = -1; X <= ChunkSide + 1; ++X) {
					const float Box = static_cast<float>(std::max({
						std::abs(X + ChunkX * ChunkSide) - 12, std::abs(Y - 8) - 6, std::abs(Z - 10) - 7
					}));
					const float Tunnel = std::hypot(static_cast<float>(Y - 8), static_cast<float>(Z - 10)) - 1.9f;
					const float Field = Open ? std::max(Box, -Tunnel) : Box;
					Samples.push_back({static_cast<std::int16_t>(std::lround(200 * Field)), Field < 0 ? 1u : 0u});
				}
		SurfaceMesh Visual;
		Require(ExtractSurface({Samples,ChunkSide,ChunkSide,ChunkSide,1.0}, {}, Visual) == SurfaceStatus::Success,
			"volumetric tunnel fixture extraction");
		ChunkPublicationCandidate Result{{ChunkX,0,0}, nullptr, {}, true};
		Require(BuildCollisionSurface(Visual, {}, Result.Collision) == SurfaceStatus::Success,
			"volumetric tunnel fixture conditioning");
		return Result;
	}

	void TestTunnelFill() {
		PhysicsWorld World(PhysicsWorldConfig{});
		PhysicsStage Adapter(World);
		const std::array Open{MakeTunnelChunk(-1, true), MakeTunnelChunk(0, true)};
		auto Tunnel = Adapter.Stage({}, Open);
		Require(Tunnel.has_value(), "native two-chunk tunnel cook");
		(*Tunnel)->Install(1);
		const PhysicsRaycastRequest Passage{{-15,8,10}, {30,0,0}, {}};
		const PhysicsRaycastRequest Wall{{-15,11,10}, {30,0,0}, {}};
		Require(!World.Raycast(Passage).HasHit() && World.Raycast(Wall).HasHit(),
			"conditioned tunnel stays open across seam while surrounding rock remains");
		PhysicsShapeCastRequest Traverse;
		Traverse.Shape.Transform.Position = Passage.Origin;
		Traverse.Shape.Radius = .4f;
		Traverse.Direction = Passage.Direction;
		const auto Traversal = World.ShapeCast(Traverse);
		Require(Traversal.Succeeded() && !Traversal.HasHit(), "sphere traverses conditioned tunnel seam");

		const std::array Closed{MakeTunnelChunk(-1, false), MakeTunnelChunk(0, false)};
		auto Fill = Adapter.Stage({}, Closed);
		Require(Fill.has_value(), "native two-chunk tunnel fill cook");
		(*Fill)->Install(2);
		const auto Hit = World.Raycast(Passage);
		Require(Hit.Succeeded() && Hit.HasHit(), "filled tunnel now blocks the center query");
		const auto Owner = Adapter.FindOwner(Hit.Candidates.front().Body);
		Require(Owner && Owner->SurfaceRevision == 2, "tunnel fill publishes the replacement revision");
	}
}

int main() {
	try {
		TestNativeCohort();
		TestConditionedCook();
		TestTunnelFill();
		std::cout << "[Terrain:PhysicsStage] Native staging and conditioned cook checks passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Terrain:PhysicsStage] " << Error.what() << '\n';
		return 1;
	}
}
