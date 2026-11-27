#include "gargantuan/render/SpatialViewportCapture.hpp"

#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"
#include "gargantuan/render/RenderExtractor.hpp"

#include <map>

namespace gargantuan {
	struct SpatialViewportCapture::State {
		RenderPublisher Publisher;
		std::map<ObjectId, RenderObjectCreate> Objects;
		std::map<RenderMeshIdentity, RenderMeshCreate> Meshes;
		std::shared_ptr<const RenderTerrainDraws> Terrain;
		std::shared_ptr<const RenderViewportGeometry> Geometry;
	};

	SpatialViewportCapture::SpatialViewportCapture() : Current(std::make_unique<State>()) {}
	SpatialViewportCapture::~SpatialViewportCapture() = default;

	std::optional<RenderViewportScene> SpatialViewportCapture::Capture(const ViewportFrame &Viewport,
		SpatialRegion &World, const AnimationRuntime *Animation, std::uint32_t Width, std::uint32_t Height) {
		const auto Camera = Viewport.GetCurrentCamera();
		if (!Camera || !Width || !Height || Width > MaximumRenderViewportDimension ||
			Height > MaximumRenderViewportDimension || Viewport.GetSpatialRegion().value_or(nullptr).get() != &World ||
			World.Parts.size() > MaximumRenderViewportObjects) return std::nullopt;
		auto &State = *Current;
		auto Input = MakeRenderCameraInput(**Camera);
		Input.FarPlane = 1000.0f;
		State.Publisher.SetTerrainRenderState(World.GetTerrainRenderState());
		const auto Publication = State.Publisher.Publish(World, Input, Width, Height);
		if (Publication->FullResync) {
			State.Objects.clear(); State.Meshes.clear(); State.Terrain.reset(); State.Geometry.reset();
		}
		for (const auto &Remove : Publication->Removes) State.Objects.erase(Remove.Object);
		for (const auto &Create : Publication->Creates) State.Objects.insert_or_assign(Create.Item.Object, Create);
		for (const auto &Update : Publication->Updates)
			State.Objects.insert_or_assign(Update.Object, RenderObjectCreate{
				Update.Item, Update.Mesh, Update.Material, Update.Visible, Update.Primitives});
		const bool Changed = Publication->FullResync || Publication->TerrainDraws || !Publication->MeshCreates.empty() ||
			!Publication->MeshRemoves.empty() || !Publication->MeshVertexUpdates.empty();
		for (const auto &Remove : Publication->MeshRemoves) State.Meshes.erase(Remove.Mesh);
		for (const auto &Mesh : Publication->MeshCreates) State.Meshes.insert_or_assign(Mesh.Mesh, Mesh);
		for (const auto &Update : Publication->MeshVertexUpdates) {
			const auto Found = State.Meshes.find(Update.Mesh);
			if (Found == State.Meshes.end() || !Found->second.Vertices || !Update.Vertices ||
				Update.FirstVertex > Found->second.Vertices->size() ||
				Update.Vertices->size() > Found->second.Vertices->size() - Update.FirstVertex) {
				State.Publisher.RequestFullResync();
				return std::nullopt;
			}
			Found->second.VertexRevision = Update.VertexRevision;
			if (Found->second.Vertices->size() == Update.Vertices->size()) Found->second.Vertices = Update.Vertices;
			else {
				auto Vertices = std::make_shared<std::vector<RenderVertex>>(*Found->second.Vertices);
				std::copy(Update.Vertices->begin(), Update.Vertices->end(), Vertices->begin() + Update.FirstVertex);
				Found->second.Vertices = std::move(Vertices);
			}
			Found->second.Bounds = Update.Bounds;
		}
		if (Publication->TerrainDraws) State.Terrain = Publication->TerrainDraws;
		if (State.Objects.size() > MaximumRenderViewportObjects || State.Meshes.size() > MaximumRenderViewportPrimitives ||
			(State.Terrain && State.Terrain->size() > MaximumRenderViewportPrimitives)) return std::nullopt;
		std::size_t Indices = 0, GeometryBytes = 0;
		for (const auto &[Id, Mesh] : State.Meshes) {
			if (!Mesh.Indices || !Mesh.Vertices || Mesh.Indices->size() > MaximumRenderViewportMeshIndices - Indices)
				return std::nullopt;
			if (Mesh.Vertices->size() > (MaximumRenderViewportGeometryBytes - GeometryBytes) / sizeof(RenderVertex))
				return std::nullopt;
			GeometryBytes += Mesh.Vertices->size() * sizeof(RenderVertex);
			if (Mesh.Indices->size() > (MaximumRenderViewportGeometryBytes - GeometryBytes) / sizeof(std::uint32_t))
				return std::nullopt;
			GeometryBytes += Mesh.Indices->size() * sizeof(std::uint32_t);
			Indices += Mesh.Indices->size();
		}
		if (Changed || (!State.Geometry && !State.Meshes.empty())) {
			if (State.Meshes.empty()) State.Geometry.reset();
			else {
				auto Geometry = std::make_shared<RenderViewportGeometry>();
				Geometry->Meshes.reserve(State.Meshes.size());
				for (const auto &[Id, Mesh] : State.Meshes) Geometry->Meshes.push_back(Mesh);
				Geometry->Terrain = State.Terrain;
				State.Geometry = std::move(Geometry);
			}
		}
		RenderViewportScene Scene{.Viewport = Viewport.GetObjectId(), .Camera = Publication->Frame.Camera,
			.Width = Width, .Height = Height, .World = World.GetObjectId(), .Geometry = State.Geometry};
		std::size_t Primitives = State.Terrain ? State.Terrain->size() : 0, Rigs = 0;
		for (const auto &[Id, Visual] : State.Objects) {
			if (!Visual.Visible) continue;
			Primitives += Visual.Primitives ? Visual.Primitives->size() : 1;
			if (Primitives > MaximumRenderViewportPrimitives) return std::nullopt;
			if (Visual.Primitives && Visual.Mesh && !State.Meshes.contains(*Visual.Mesh))
				for (const auto &Range : *Visual.Primitives) {
					if (Range.IndexCount > MaximumRenderViewportMeshIndices - Indices) return std::nullopt;
					Indices += Range.IndexCount;
				}
			RenderViewportObject Object{.Visual = Visual};
			Object.Visual.Item.CastShadow = false;
			if (Animation && Visual.Mesh) {
				const auto Pose = Animation->GetPose(Id);
				if (Pose && Pose->SkinPalette && Pose->SkeletonCompatibilityId.IsValid()) {
					if (++Rigs > MaximumRenderViewportRigs) return std::nullopt;
					Object.Pose = RenderAnimationPoseUpdate{.Object = Id, .SourceMesh = *Visual.Mesh,
						.PoseRevision = Pose->PoseRevision, .Palette = {{Pose->SkeletonCompatibilityId.Bytes}, Pose->SkinPalette}};
				}
			}
			Scene.Objects.push_back(std::move(Object));
		}
		return Scene;
	}
}
