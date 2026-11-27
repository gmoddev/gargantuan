#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/render/RenderExtractor.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace gargantuan {
	namespace {
		constexpr std::size_t FaceBytes = 4 * sizeof(RenderVertex) + 6 * sizeof(std::uint32_t);
		constexpr std::size_t MeshRecordBytes = sizeof(RenderMeshCreate) + sizeof(RenderObjectCreate) +
			sizeof(RenderMeshRemove) + sizeof(RenderObjectUpdate);

		glm::vec3 Vector(const blockgrid::Point &Point) {
			return {Point[0], Point[1], Point[2]};
		}

		bool Covered(const blockgrid::Field &Field, blockgrid::Cell Position, unsigned Face) {
			glm::ivec3 Neighbor(Position.X, Position.Y, Position.Z);
			Neighbor[Face / 2] += (Face & 1) ? -1 : 1;
			const blockgrid::Cell Adjacent{Neighbor.x, Neighbor.y, Neighbor.z};
			if (!blockgrid::ValidCell(Adjacent)) return false;
			const auto Block = Field.Get(Adjacent);
			if (Block.Definition <= blockgrid::Brick) return Block.Definition != blockgrid::Air;
			const auto Shape = Field.GetDefinition(Block.Definition).Geometry;
			return (blockgrid::GetGeometry(Shape, Block.Orientation).FullFaces & (1u << (Face ^ 1))) != 0;
		}

		std::size_t UploadBytes(const RenderMeshCreate &Mesh) {
			return Mesh.Vertices->size() * sizeof(RenderVertex) + Mesh.Indices->size() * sizeof(std::uint32_t);
		}

		std::optional<RenderMeshCreate> MakeGridMesh(const blockgrid::Field &Field, std::size_t Budget) {
			auto Vertices = std::make_shared<std::vector<RenderVertex>>();
			auto Indices = std::make_shared<std::vector<std::uint32_t>>();
			RenderBounds Bounds{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
			bool WithinBudget = true;
			Field.ForEachOccupied([&](blockgrid::Cell Cell, blockgrid::Block Value) {
				if (!WithinBudget) return;
				const glm::ivec3 Position(Cell.X, Cell.Y, Cell.Z);
				const auto &Definition = Field.GetDefinition(Value.Definition);
				std::array<std::uint8_t, 4> Tint{0, 0, 0, 255};
				for (unsigned C = 0; C < 3; ++C)
					Tint[C] = static_cast<std::uint8_t>(std::lround(Definition.Color[C] * 255.0f));
				if (Definition.Geometry != blockgrid::Shape::Cube) {
					for (const auto &Face : blockgrid::GetGeometry(Definition.Geometry, Value.Orientation).Faces) {
						if (Face.Boundary >= 0 && Covered(Field, Cell, static_cast<unsigned>(Face.Boundary))) continue;
						const auto Bytes = Face.Count * sizeof(RenderVertex) + (Face.Count - 2) * 3 * sizeof(std::uint32_t);
						if (Bytes > Budget) { WithinBudget = false; return; }
						Budget -= Bytes;
						const auto First = static_cast<std::uint32_t>(Vertices->size());
						const auto Origin = Vector(Face.Corners[0]);
						const auto Normal = Vector(Face.Normal);
						const auto Tangent = glm::normalize(Vector(Face.Corners[1]) - Origin);
						const auto Bitangent = glm::cross(Normal, Tangent);
						for (unsigned I = 0; I < Face.Count; ++I) {
							const auto Offset = Vector(Face.Corners[I]);
							RenderVertex Vertex;
							Vertex.Position = glm::vec3(Position) + Offset;
							Vertex.Normal = Normal;
							Vertex.Tangent = glm::vec4(Tangent, 1.0f);
							Vertex.TextureCoordinate = {glm::dot(Offset - Origin, Tangent), glm::dot(Offset - Origin, Bitangent)};
							Vertex.Tint = Tint;
							Vertices->push_back(Vertex);
							Bounds.Minimum = glm::min(Bounds.Minimum, Vertex.Position);
							Bounds.Maximum = glm::max(Bounds.Maximum, Vertex.Position);
						}
						Indices->insert(Indices->end(), {First, First + 1, First + 2});
						if (Face.Count == 4) Indices->insert(Indices->end(), {First, First + 2, First + 3});
					}
					return;
				}
				for (unsigned Axis = 0; Axis < 3; ++Axis) {
					glm::ivec3 Normal(0), U(0), V(0);
					U[(Axis + 1) % 3] = 1;
					V[(Axis + 2) % 3] = 1;
					for (int Sign : {-1, 1}) {
						Normal[Axis] = Sign;
						if (Covered(Field, Cell, 2 * Axis + (Sign < 0))) continue;
						if (Budget < FaceBytes) { WithinBudget = false; return; }
						Budget -= FaceBytes;
						const auto Origin = Position + (Sign > 0 ? Normal : glm::ivec3(0));
						const auto First = static_cast<std::uint32_t>(Vertices->size());
						for (const auto &Offset : {glm::ivec3(0), U, U + V, V}) {
							RenderVertex Vertex;
							Vertex.Position = glm::vec3(Origin + Offset);
							Vertex.Normal = glm::vec3(Normal);
							Vertex.Tangent = glm::vec4(glm::vec3(U), float(Sign));
							Vertex.TextureCoordinate = {float(Offset[(Axis+1)%3]), float(Offset[(Axis+2)%3])};
							Vertex.Tint = Tint;
							Vertices->push_back(Vertex);
							Bounds.Minimum = glm::min(Bounds.Minimum, Vertex.Position);
							Bounds.Maximum = glm::max(Bounds.Maximum, Vertex.Position);
						}
						if (Sign > 0) Indices->insert(Indices->end(), {First, First+1, First+2, First, First+2, First+3});
						else Indices->insert(Indices->end(), {First, First+2, First+1, First, First+3, First+2});
					}
				}
			});
			if (!WithinBudget || Indices->empty()) return std::nullopt;
			static std::atomic<std::uint64_t> NextMesh{1};
			const RenderMeshIdentity Identity{NextMesh.fetch_add(1, std::memory_order_relaxed), 1, RenderMeshDomain::BlockGrid};
			return RenderMeshCreate{Identity, 1, 1, std::move(Vertices), std::move(Indices), Bounds};
		}
	}

	RenderPublisher::GridMeshes RenderPublisher::PrepareBlockGrids(const WorldRoot &World, RenderPublication &Result,
		std::size_t Budget) const {
		GridMeshes Next;
		World.ForEachBlockGrid([&](const BlockGrid &Grid) {
			const auto &Field = Grid.GetField();
			if (!Grid.GetVisible() || !Field.GetOccupiedCount()) return;
			const auto Id = Grid.GetObjectId();
			const auto Old = PublishedGrids.find(Id);
			const auto Pose = Grid.GetCFrame();
			RenderItem Item;
			Item.Object = Id;
			Item.ModelMatrix = glm::mat4(Pose.Rotation * static_cast<float>(Grid.GetCellSize()));
			Item.ModelMatrix[3] = glm::vec4(Pose.Position, 1);
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
			const bool Build = Old == PublishedGrids.end() || Old->second.Revision != Field.GetRevision();
			const bool Upload = Build || Result.FullResync;
			std::optional<RenderMeshCreate> Mesh;
			if (Build && Budget >= MeshRecordBytes) Mesh = MakeGridMesh(Field, Budget - MeshRecordBytes);
			else if (!Build) Mesh = Old->second.Resource;
			if (!Mesh || (Upload && (Budget < MeshRecordBytes || UploadBytes(*Mesh) > Budget - MeshRecordBytes))) {
				Result.Diagnostics.push_back({RenderExtractionIssue::PublicationOverflow, Id,
					"[BlockGrid:Render] Grid surface exceeds the available publication budget"});
				return;
			}
			if (Upload) {
				Budget -= UploadBytes(*Mesh) + MeshRecordBytes;
				Result.MeshCreates.push_back(*Mesh);
			}
			if (Old == PublishedGrids.end() || Result.FullResync) {
				Result.Creates.push_back({Item, Mesh->Mesh, {}});
			} else if (Build || Item != Old->second.Item) {
				auto Domains = RenderUpdateDomain::Transform;
				if (Build) {
					Domains = Domains | RenderUpdateDomain::Geometry;
					Result.MeshRemoves.push_back({Old->second.Resource.Mesh});
				}
				Result.Updates.push_back({Id, Domains, Item, Mesh->Mesh, {}});
			}
			Next.emplace(Id, PublishedGrid{std::move(*Mesh), Item, Field.GetRevision()});
		});
		if (!Result.FullResync) {
			for (const auto &[Id, Prior] : PublishedGrids) {
				if (Next.contains(Id)) continue;
				Result.MeshRemoves.push_back({Prior.Resource.Mesh});
				Result.Removes.push_back({Id});
			}
		}
		return Next;
	}
}
