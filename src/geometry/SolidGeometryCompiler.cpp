// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/geometry/SolidGeometryCompiler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <manifold/manifold.h>

namespace gargantuan {
	namespace {
		constexpr std::size_t MaximumTrianglePairs = 1'000'000;
		constexpr float MaximumCoordinate = 100'000.0f;

		AssetDiagnostic Error(std::string Code, std::string Message) {
			return {std::move(Code), std::move(Message)};
		}

		bool Finite(glm::vec3 Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool Finite(glm::mat4 Value) {
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Value[Column][Row]) || std::abs(Value[Column][Row]) > MaximumCoordinate)
						return false;
			return true;
		}

		AssetDiagnostic KernelError(manifold::Manifold::Error Status, const std::string &NodeId) {
			switch (Status) {
			case manifold::Manifold::Error::Cancelled:
				return Error("Cancelled", "Solid compilation was cancelled at node " + NodeId);
			case manifold::Manifold::Error::NotManifold:
				return Error("NonManifold", "Solid operand is not a closed oriented manifold at node " + NodeId);
			case manifold::Manifold::Error::ResultTooLarge:
				return Error("GeometryLimit", "Boolean kernel rejected excessive geometry at node " + NodeId);
			default:
				return Error("InvalidGeometry", "Boolean kernel rejected geometry at node " + NodeId);
			}
		}

		struct Triangle {
			std::array<glm::vec3, 3> Positions;
		};

		bool Less(glm::vec3 A, glm::vec3 B) {
			if (A.x != B.x) return A.x < B.x;
			if (A.y != B.y) return A.y < B.y;
			return A.z < B.z;
		}

		bool Less(const Triangle &A, const Triangle &B) {
			for (std::size_t Corner = 0; Corner < 3; ++Corner) {
				if (Less(A.Positions[Corner], B.Positions[Corner])) return true;
				if (Less(B.Positions[Corner], A.Positions[Corner])) return false;
			}
			return false;
		}

		std::expected<SolidCompilation, AssetDiagnostic> BuildCandidate(
			const manifold::Manifold &Solid, std::size_t MaximumOutputTriangles
		) {
			const auto TriangleCount = Solid.NumTri();
			if (TriangleCount == 0) return std::unexpected(Error("EmptySolid", "Solid result has no surface"));
			if (TriangleCount > MaximumOutputTriangles || TriangleCount > AssetLimits::MaximumMeshIndices / 3)
				return std::unexpected(Error("GeometryLimit", "Solid output exceeds the triangle limit"));
			const auto Raw = Solid.GetMeshGL64();
			if (Raw.numProp < 3 || Raw.triVerts.size() % 3 != 0 ||
				Raw.vertProperties.size() % Raw.numProp != 0)
				return std::unexpected(Error("InvalidGeometry", "Boolean kernel returned malformed mesh arrays"));
			std::vector<Triangle> Triangles;
			Triangles.reserve(Raw.NumTri());
			for (std::size_t Index = 0; Index < Raw.triVerts.size(); Index += 3) {
				Triangle Next;
				for (std::size_t Corner = 0; Corner < 3; ++Corner) {
					const auto Vertex = Raw.triVerts[Index + Corner];
					if (Vertex >= Raw.NumVert())
						return std::unexpected(Error("InvalidGeometry", "Boolean kernel returned an invalid index"));
					const auto Offset = static_cast<std::size_t>(Vertex * Raw.numProp);
					glm::vec3 Position{
						static_cast<float>(Raw.vertProperties[Offset]),
						static_cast<float>(Raw.vertProperties[Offset + 1]),
						static_cast<float>(Raw.vertProperties[Offset + 2]),
					};
					if (!Finite(Position) || glm::any(glm::greaterThan(glm::abs(Position), glm::vec3(MaximumCoordinate))))
						return std::unexpected(Error("GeometryLimit", "Solid output coordinate is non-finite or out of range"));
					Position.x = Position.x == 0.0f ? 0.0f : Position.x;
					Position.y = Position.y == 0.0f ? 0.0f : Position.y;
					Position.z = Position.z == 0.0f ? 0.0f : Position.z;
					Next.Positions[Corner] = Position;
				}
				const auto Cross = glm::cross(Next.Positions[1] - Next.Positions[0],
					Next.Positions[2] - Next.Positions[0]);
				if (!Finite(Cross) || glm::dot(Cross, Cross) < 1.0e-10f)
					return std::unexpected(Error("DegenerateOutput", "Solid output contains a zero-area or too-thin triangle"));
				const auto First = std::min_element(Next.Positions.begin(), Next.Positions.end(),
					[](glm::vec3 A, glm::vec3 B) { return Less(A, B); });
				std::rotate(Next.Positions.begin(), First, Next.Positions.end());
				Triangles.push_back(Next);
			}
			std::sort(Triangles.begin(), Triangles.end(),
				[](const Triangle &A, const Triangle &B) { return Less(A, B); });
			if (std::adjacent_find(Triangles.begin(), Triangles.end(),
				[](const Triangle &A, const Triangle &B) { return !Less(A, B) && !Less(B, A); }) != Triangles.end())
				return std::unexpected(Error("InvalidGeometry", "Solid output contains duplicate triangles"));

			std::vector<RenderVertex> Vertices;
			std::vector<glm::vec3> CollisionPositions;
			std::vector<std::uint32_t> Indices;
			Vertices.reserve(Triangles.size() * 3);
			CollisionPositions.reserve(Triangles.size() * 3);
			Indices.reserve(Triangles.size() * 3);
			RenderBounds Bounds{glm::vec3(std::numeric_limits<float>::max()),
				glm::vec3(std::numeric_limits<float>::lowest())};
			for (const auto &Face : Triangles) {
				const auto Normal = glm::normalize(glm::cross(Face.Positions[1] - Face.Positions[0],
					Face.Positions[2] - Face.Positions[0]));
				const auto Absolute = glm::abs(Normal);
				const auto UAxis = Absolute.x >= Absolute.y && Absolute.x >= Absolute.z
					? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
				const auto VAxis = Absolute.x >= Absolute.y && Absolute.x >= Absolute.z
					? glm::vec3(0, 1, 0) : Absolute.y >= Absolute.z
						? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
				const auto Tangent = glm::normalize(UAxis - Normal * glm::dot(UAxis, Normal));
				const float Handedness = glm::dot(glm::cross(Normal, Tangent), VAxis) < 0 ? -1.0f : 1.0f;
				for (const auto Position : Face.Positions) {
					RenderVertex Vertex;
					Vertex.Position = Position;
					Vertex.Normal = Normal;
					Vertex.Tangent = glm::vec4(Tangent, Handedness);
					Vertex.TextureCoordinate = {glm::dot(Position, UAxis), glm::dot(Position, VAxis)};
					Vertices.push_back(Vertex);
					CollisionPositions.push_back(Position);
					Indices.push_back(static_cast<std::uint32_t>(Indices.size()));
					Bounds.Minimum = glm::min(Bounds.Minimum, Position);
					Bounds.Maximum = glm::max(Bounds.Maximum, Position);
				}
			}
			SolidCompilation Result;
			Result.Mesh.Vertices = std::make_shared<const std::vector<RenderVertex>>(std::move(Vertices));
			Result.Mesh.Indices = std::make_shared<const std::vector<std::uint32_t>>(Indices);
			Result.Mesh.Bounds = Bounds;
			Result.Mesh.SubmeshCount = 1;
			Result.Mesh.Primitives = std::make_shared<const std::vector<ImportedMeshPrimitive>>(
				std::vector<ImportedMeshPrimitive>{{0, static_cast<std::uint32_t>(Indices.size()), std::nullopt}});
			Result.Collision.Positions =
				std::make_shared<const std::vector<glm::vec3>>(std::move(CollisionPositions));
			Result.Collision.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(Indices));
			Result.Collision.Bounds = Bounds;
			return Result;
		}
	}

	std::expected<SolidCompilation, AssetDiagnostic> CompileSolidGeometry(
		const SolidGraph &Graph, const AssetCancellationToken &Cancellation,
		std::chrono::steady_clock::time_point Deadline, const SolidCompileLimits &Limits,
		SolidCompileProbe *Probe
	) {
		try {
			const auto IsCancelled = [&] {
				return Cancellation.IsCancelled() || std::chrono::steady_clock::now() >= Deadline;
			};
			if (IsCancelled()) return std::unexpected(Error("Cancelled", "Solid compilation was cancelled before validation"));
			if (Graph.Version != 1) return std::unexpected(Error("UnsupportedSolidVersion", "Unknown solid graph version"));
			if (Graph.Nodes.empty() || Graph.Nodes.size() > std::min(Limits.MaximumNodes, std::size_t{128}))
				return std::unexpected(Error("GeometryLimit", "Solid graph node count is outside the supported range"));
			std::unordered_map<std::string, std::size_t> NodeById;
			for (std::size_t Index = 0; Index < Graph.Nodes.size(); ++Index) {
				const auto &Node = Graph.Nodes[Index];
				if (Node.Id.empty() || Node.Id.size() > 64 || !NodeById.emplace(Node.Id, Index).second)
					return std::unexpected(Error("InvalidSolidGraph", "Solid node IDs must be unique and 1-64 bytes"));
			}
			if (Graph.Root.empty() || Graph.Root.size() > 64 || !NodeById.contains(Graph.Root))
				return std::unexpected(Error("InvalidSolidGraph", "Solid root does not name a node"));
			std::size_t InputTriangles = 0;
			std::size_t InputVertices = 0;
			for (const auto &Node : Graph.Nodes) {
				if (IsCancelled()) return std::unexpected(Error("Cancelled", "Solid compilation was cancelled during validation"));
				if (Node.Inputs.size() > 16)
					return std::unexpected(Error("InvalidSolidGraph", "Solid node has too many input references"));
				for (const auto &Input : Node.Inputs)
					if (Input.empty() || Input.size() > 64 || !NodeById.contains(Input))
						return std::unexpected(Error("InvalidSolidGraph", "Solid node references a missing input"));
				switch (Node.Kind) {
				case SolidNodeKind::Box:
					if (!Node.Inputs.empty() || !Finite(Node.Size) ||
						glm::any(glm::lessThanEqual(Node.Size, glm::vec3(0))) ||
						glm::any(glm::greaterThan(Node.Size, glm::vec3(10'000))))
						return std::unexpected(Error("InvalidSolidGraph", "Box dimensions are invalid"));
					InputTriangles += 12;
					break;
				case SolidNodeKind::Sphere:
					if (!Node.Inputs.empty() || !std::isfinite(Node.Radius) ||
						Node.Radius <= 0 || Node.Radius > 10'000)
						return std::unexpected(Error("InvalidSolidGraph", "Sphere radius is invalid"));
					// Manifold's 24-segment geodesic sphere refines eight octahedron faces
					// into six-by-six triangles per face.
					InputTriangles += 288;
					break;
				case SolidNodeKind::ImportedMesh:
					if (!Node.Inputs.empty() || !Node.Mesh || !Node.Mesh->Vertices || !Node.Mesh->Indices ||
						Node.Mesh->Vertices->empty() || Node.Mesh->Indices->empty() ||
						Node.Mesh->Indices->size() % 3 != 0 ||
						Node.Mesh->Vertices->size() > AssetLimits::MaximumMeshVertices ||
						Node.Mesh->Indices->size() > AssetLimits::MaximumMeshIndices)
						return std::unexpected(Error("InvalidSolidMesh", "Imported operand arrays are invalid"));
					InputTriangles += Node.Mesh->Indices->size() / 3;
					if (InputTriangles > std::min(Limits.MaximumInputTriangles, std::size_t{8192}))
						return std::unexpected(Error("GeometryLimit", "Solid graph exceeds the input triangle limit"));
					if (Node.Mesh->Vertices->size() > std::min(Limits.MaximumInputVertices, std::size_t{24576}) ||
						InputVertices > std::min(Limits.MaximumInputVertices, std::size_t{24576}) - Node.Mesh->Vertices->size())
						return std::unexpected(Error("GeometryLimit", "Solid graph exceeds the aggregate imported vertex limit"));
					InputVertices += Node.Mesh->Vertices->size();
					for (std::size_t VertexIndex = 0; VertexIndex < Node.Mesh->Vertices->size(); ++VertexIndex) {
						if (VertexIndex % 256 == 0 && IsCancelled())
							return std::unexpected(Error("Cancelled", "Solid compilation was cancelled during vertex validation"));
						const auto &Vertex = (*Node.Mesh->Vertices)[VertexIndex];
						if (!Finite(Vertex.Position) ||
							glm::any(glm::greaterThan(glm::abs(Vertex.Position), glm::vec3(MaximumCoordinate))))
							return std::unexpected(Error("InvalidSolidMesh", "Imported operand has invalid coordinates"));
					}
					for (const auto Index : *Node.Mesh->Indices)
						if (Index >= Node.Mesh->Vertices->size())
							return std::unexpected(Error("InvalidSolidMesh", "Imported operand has an invalid index"));
					break;
				case SolidNodeKind::Transform:
					if (Node.Inputs.size() != 1 || !Finite(Node.Matrix) ||
						std::abs(glm::determinant(glm::mat3(Node.Matrix))) < 1.0e-8f ||
						Node.Matrix[0][3] != 0 || Node.Matrix[1][3] != 0 ||
						Node.Matrix[2][3] != 0 || Node.Matrix[3][3] != 1)
						return std::unexpected(Error("InvalidSolidGraph", "Transform must be finite, affine, and invertible"));
					break;
				case SolidNodeKind::Union:
				case SolidNodeKind::Intersection:
				case SolidNodeKind::Subtraction:
					if (Node.Inputs.size() < 2 || Node.Inputs.size() > 16)
						return std::unexpected(Error("InvalidSolidGraph", "Boolean node requires 2-16 ordered inputs"));
					break;
				default:
					return std::unexpected(Error("InvalidSolidGraph", "Unknown solid node kind"));
			}
		}
			if (InputTriangles > std::min(Limits.MaximumInputTriangles, std::size_t{8192}))
				return std::unexpected(Error("GeometryLimit", "Solid graph exceeds the input triangle limit"));
			std::vector<std::uint8_t> Visit(Graph.Nodes.size(), 0);
			std::vector<std::size_t> LongestPath(Graph.Nodes.size(), 0);
			const auto CheckCycles = [&](const auto &Self, std::size_t Index) -> std::optional<std::size_t> {
				if (Visit[Index] == 1) return std::nullopt;
				if (Visit[Index] == 2) return LongestPath[Index];
				Visit[Index] = 1;
				std::size_t MaximumChildPath = 0;
				for (const auto &Input : Graph.Nodes[Index].Inputs) {
					const auto ChildPath = Self(Self, NodeById.at(Input));
					if (!ChildPath) return std::nullopt;
					MaximumChildPath = std::max(MaximumChildPath, *ChildPath);
				}
				LongestPath[Index] = MaximumChildPath + 1;
				Visit[Index] = 2;
				return LongestPath[Index];
			};
			for (std::size_t Index = 0; Index < Graph.Nodes.size(); ++Index) {
				const auto Path = CheckCycles(CheckCycles, Index);
				if (!Path || *Path > std::min(Limits.MaximumDepth, std::size_t{24}))
					return std::unexpected(Error("InvalidSolidGraph", "Solid graph contains a cycle or exceeds depth limit"));
			}
			if (Cancellation.IsCancelled() || std::chrono::steady_clock::now() >= Deadline)
				return std::unexpected(Error("Cancelled", "Solid compilation was cancelled before evaluation"));

			manifold::ExecutionContext Execution;
			std::jthread Observer([&](std::stop_token Stop) {
				while (!Stop.stop_requested()) {
					if (Cancellation.IsCancelled() || std::chrono::steady_clock::now() >= Deadline) {
						Execution.Cancel();
						return;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			});
			std::unordered_map<std::size_t, manifold::Manifold> Compiled;
			std::size_t BooleanSteps = 0;
			std::size_t TrianglePairs = 0;
			std::size_t PeakIntermediateTriangles = 0;
			const auto Evaluate = [&](const auto &Self, std::size_t Index)
				-> std::expected<manifold::Manifold, AssetDiagnostic> {
				if (Execution.Cancelled()) return std::unexpected(Error("Cancelled", "Solid compilation was cancelled"));
				if (const auto Found = Compiled.find(Index); Found != Compiled.end()) return Found->second;
				const auto &Node = Graph.Nodes[Index];
				manifold::Manifold Result;
				if (Node.Kind == SolidNodeKind::Box)
					Result = manifold::Manifold::Cube({Node.Size.x, Node.Size.y, Node.Size.z}, true);
				else if (Node.Kind == SolidNodeKind::Sphere)
					Result = manifold::Manifold::Sphere(Node.Radius, 24);
				else if (Node.Kind == SolidNodeKind::ImportedMesh) {
					manifold::MeshGL64 Input;
					Input.numProp = 3;
					Input.vertProperties.reserve(Node.Mesh->Vertices->size() * 3);
					std::map<std::array<std::uint32_t, 3>, std::uint64_t> ExactVertices;
					std::vector<std::uint64_t> Remap;
					Remap.reserve(Node.Mesh->Vertices->size());
					for (const auto &Vertex : *Node.Mesh->Vertices) {
						const auto X = Vertex.Position.x == 0.0f ? 0.0f : Vertex.Position.x;
						const auto Y = Vertex.Position.y == 0.0f ? 0.0f : Vertex.Position.y;
						const auto Z = Vertex.Position.z == 0.0f ? 0.0f : Vertex.Position.z;
						const std::array<std::uint32_t, 3> Key{
							std::bit_cast<std::uint32_t>(X), std::bit_cast<std::uint32_t>(Y), std::bit_cast<std::uint32_t>(Z)};
						const auto [Found, Inserted] = ExactVertices.try_emplace(Key, Input.vertProperties.size() / 3);
						if (Inserted) {
							Input.vertProperties.push_back(X);
							Input.vertProperties.push_back(Y);
							Input.vertProperties.push_back(Z);
						}
						Remap.push_back(Found->second);
					}
					Input.triVerts.reserve(Node.Mesh->Indices->size());
					for (const auto OldIndex : *Node.Mesh->Indices) Input.triVerts.push_back(Remap[OldIndex]);
					for (std::size_t Corner = 0; Corner < Input.triVerts.size(); Corner += 3)
						if (Input.triVerts[Corner] == Input.triVerts[Corner + 1] ||
							Input.triVerts[Corner + 1] == Input.triVerts[Corner + 2] ||
							Input.triVerts[Corner + 2] == Input.triVerts[Corner])
							return std::unexpected(Error("InvalidSolidMesh", "Imported operand has a triangle collapsed by exact position welding"));
					Result = Execution.FromMeshGL(Input);
				} else {
					auto First = Self(Self, NodeById.at(Node.Inputs.front()));
					if (!First) return std::unexpected(First.error());
					Result = std::move(*First);
					if (Node.Kind == SolidNodeKind::Transform) {
						manifold::mat3x4 Matrix;
						for (int Column = 0; Column < 4; ++Column)
							for (int Row = 0; Row < 3; ++Row) Matrix[Column][Row] = Node.Matrix[Column][Row];
						Result = Result.Transform(Matrix);
					} else {
						const auto Operation = Node.Kind == SolidNodeKind::Union ? manifold::OpType::Add :
							Node.Kind == SolidNodeKind::Intersection ? manifold::OpType::Intersect : manifold::OpType::Subtract;
						for (std::size_t Child = 1; Child < Node.Inputs.size(); ++Child) {
							auto Next = Self(Self, NodeById.at(Node.Inputs[Child]));
							if (!Next) return std::unexpected(Next.error());
							if (++BooleanSteps > std::min(Limits.MaximumBooleanSteps, std::size_t{32}) ||
								Result.NumTri() > MaximumTrianglePairs / std::max(std::size_t{1}, Next->NumTri()) ||
								TrianglePairs > MaximumTrianglePairs - Result.NumTri() * Next->NumTri())
								return std::unexpected(Error("GeometryLimit", "Solid Boolean work estimate exceeds limit"));
							TrianglePairs += Result.NumTri() * Next->NumTri();
							Result = Result.Boolean(*Next, Operation);
							struct BooleanScope {
								SolidCompileProbe *Probe;
								explicit BooleanScope(SolidCompileProbe *Value) : Probe(Value) {
									if (Probe) Probe->InsideBoolean.store(true, std::memory_order_release);
								}
								~BooleanScope() {
									if (Probe) Probe->InsideBoolean.store(false, std::memory_order_release);
								}
							};
							const auto Status = [&] {
								BooleanScope Scope(Probe);
								return Result.WithContext(Execution).Status();
							}();
							if (Status != manifold::Manifold::Error::NoError)
								return std::unexpected(KernelError(Status, Node.Id));
							if (Result.NumTri() > std::min(Limits.MaximumIntermediateTriangles, std::size_t{32768}))
								return std::unexpected(Error("GeometryLimit", "Solid intermediate exceeds triangle limit"));
							PeakIntermediateTriangles = std::max(PeakIntermediateTriangles, Result.NumTri());
						}
					}
				}
				const auto Status = Result.WithContext(Execution).Status();
				if (Status != manifold::Manifold::Error::NoError)
					return std::unexpected(KernelError(Status, Node.Id));
				if (Result.NumTri() > std::min(Limits.MaximumIntermediateTriangles, std::size_t{32768}))
					return std::unexpected(Error("GeometryLimit", "Solid node exceeds intermediate triangle limit"));
				PeakIntermediateTriangles = std::max(PeakIntermediateTriangles, Result.NumTri());
				if (!Result.IsEmpty()) {
					const auto Bounds = Result.BoundingBox();
					const auto Valid = [](manifold::vec3 Value) {
						return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) &&
							std::abs(Value.x) <= MaximumCoordinate && std::abs(Value.y) <= MaximumCoordinate &&
							std::abs(Value.z) <= MaximumCoordinate;
					};
					if (!Valid(Bounds.min) || !Valid(Bounds.max))
						return std::unexpected(Error("GeometryLimit", "Evaluated solid node exceeds coordinate limit"));
				}
				Compiled.emplace(Index, Result);
				return Result;
			};
			auto Root = Evaluate(Evaluate, NodeById.at(Graph.Root));
			if (!Root) return std::unexpected(Root.error());
			if (Execution.Cancelled()) return std::unexpected(Error("Cancelled", "Solid compilation was cancelled"));
			auto Candidate = BuildCandidate(*Root, std::min(Limits.MaximumOutputTriangles, std::size_t{32768}));
			if (Execution.Cancelled() || Cancellation.IsCancelled() || std::chrono::steady_clock::now() >= Deadline)
				return std::unexpected(Error("Cancelled", "Solid compilation was cancelled before candidate publication"));
			if (Candidate) {
				Candidate->InputTriangles = InputTriangles;
				Candidate->EvaluatedTrianglePairs = TrianglePairs;
				Candidate->PeakIntermediateTriangles = PeakIntermediateTriangles;
			}
			return Candidate;
		} catch (const std::bad_alloc &) {
			return std::unexpected(Error("GeometryMemory", "Solid compiler allocation failed"));
		} catch (const std::exception &) {
			return std::unexpected(Error("GeometryFailure", "Solid compiler failed without publishing assets"));
		}
	}
}
