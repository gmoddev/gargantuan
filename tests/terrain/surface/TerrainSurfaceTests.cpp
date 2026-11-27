// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include "gargantuan/terrain/Surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
	using namespace gargantuan::terrain;
	using Coordinate = std::array<int, 3>;
	using Key = std::array<long long, 3>;
	using Edge = std::pair<std::uint32_t, std::uint32_t>;

	void Require(bool Condition, std::string_view Description) {
		if (!Condition)
			throw std::runtime_error(std::string(Description));
	}

	struct Field {
		Coordinate Origin;
		std::uint32_t Side;
		std::vector<Sample> Samples;

		template<typename Generator>
		Field(Coordinate Base, std::uint32_t Length, Generator Generate) : Origin(Base), Side(Length) {
			const int High = static_cast<int>(Length) + 1;
			Samples.reserve((Side + 3) * (Side + 3) * (Side + 3));
			for (int Z = -1; Z <= High; ++Z)
				for (int Y = -1; Y <= High; ++Y)
					for (int X = -1; X <= High; ++X) {
						const int Density = Generate(X + Origin[0], Y + Origin[1], Z + Origin[2]);
						Samples.push_back({static_cast<std::int16_t>(Density),
							Density < 0 ? static_cast<MaterialId>(1 + ((X + Y + Z + Origin[0] + Origin[1] + Origin[2]) & 3)) : 0});
					}
		}

		[[nodiscard]] SurfaceInput View(double Spacing = 1) const {
			return {Samples, Side, Side, Side, Spacing};
		}
	};

	SurfaceMesh Extract(const Field &Source, double Spacing = 1) {
		SurfaceMesh Mesh;
		const auto Side = Source.Side;
		const SurfaceLimits Limits{Side * Side * Side, 262144, Side * Side * Side * 36};
		const auto Status = ExtractSurface(Source.View(Spacing), Limits, Mesh);
		if (Status != SurfaceStatus::Success)
			throw std::runtime_error("extraction rejected " + std::to_string(static_cast<int>(Status)));
		return Mesh;
	}

	struct Topology {
		std::size_t Boundary{}, InteriorOpen{}, NonManifoldEdges{}, UnorientedEdges{};
		std::size_t NonManifoldVertices{}, WeldedDegenerate{}, Components{};
	};

	struct BoundaryEdge {
		std::uint32_t Count{};
		int Orientation{};
	};

	struct Welded {
		std::map<Key, std::uint32_t> Ids;
		std::vector<Key> Positions;
		std::vector<std::array<std::uint32_t, 3>> Triangles;

		void Add(const SurfaceMesh &Mesh, Coordinate Origin) {
			std::vector<std::uint32_t> VertexIds;
			VertexIds.reserve(Mesh.Vertices.size());
			for (const auto &Vertex : Mesh.Vertices) {
				const auto [X, Y, Z] = Vertex.Position;
				const Key Position{
					std::llround((X + static_cast<double>(Origin[0])) * 1000000.0),
					std::llround((Y + static_cast<double>(Origin[1])) * 1000000.0),
					std::llround((Z + static_cast<double>(Origin[2])) * 1000000.0)};
				const auto [Found, Inserted] = Ids.try_emplace(Position, static_cast<std::uint32_t>(Positions.size()));
				if (Inserted)
					Positions.push_back(Position);
				VertexIds.push_back(Found->second);
			}
			for (std::size_t Index = 0; Index < Mesh.Indices.size(); Index += 3)
				Triangles.push_back({VertexIds.at(Mesh.Indices[Index]), VertexIds.at(Mesh.Indices[Index + 1]),
					VertexIds.at(Mesh.Indices[Index + 2])});
		}
	};

	std::uint32_t Find(std::vector<std::uint32_t> &Parents, std::uint32_t Vertex) {
		while (Parents[Vertex] != Vertex) {
			Parents[Vertex] = Parents[Parents[Vertex]];
			Vertex = Parents[Vertex];
		}
		return Vertex;
	}

	Topology Audit(const Welded &Mesh, Coordinate Min, Coordinate Max) {
		Topology Result{};
		std::map<Edge, BoundaryEdge> Edges;
		std::vector<std::vector<Edge>> Links(Mesh.Positions.size());
		std::vector<std::uint32_t> Parents(Mesh.Positions.size());
		std::iota(Parents.begin(), Parents.end(), 0u);
		for (const auto &[A, B, C] : Mesh.Triangles) {
			if (A == B || A == C || B == C) {
				++Result.WeldedDegenerate;
				continue;
			}
			for (const auto &[First, Second] : std::array<Edge, 3>{{{A,B}, {B,C}, {C,A}}}) {
				auto &Entry = Edges[std::minmax(First, Second)];
				++Entry.Count;
				Entry.Orientation += First < Second ? 1 : -1;
				Parents[Find(Parents, First)] = Find(Parents, Second);
			}
			Links[A].emplace_back(B,C);
			Links[B].emplace_back(C,A);
			Links[C].emplace_back(A,B);
		}
		for (const auto &[Pair, Entry] : Edges) {
			if (Entry.Count > 2)
				++Result.NonManifoldEdges;
			if (Entry.Count == 2 && Entry.Orientation != 0)
				++Result.UnorientedEdges;
			if (Entry.Count != 1)
				continue;
			++Result.Boundary;
			const auto &A = Mesh.Positions[Pair.first], &B = Mesh.Positions[Pair.second];
			bool OnBoundary = false;
			for (int Axis = 0; Axis < 3; ++Axis)
				OnBoundary |= (A[Axis] == B[Axis]) &&
					(A[Axis] == Min[Axis] * 1000000ll || A[Axis] == Max[Axis] * 1000000ll);
			Result.InteriorOpen += !OnBoundary;
		}
		for (std::size_t Id = 0; Id < Links.size(); ++Id) {
			if (Links[Id].empty())
				continue;
			Result.Components += Parents[Id] == Id;
			// A manifold vertex link is one closed cycle, or one path at a boundary.
			std::map<std::uint32_t, std::vector<std::uint32_t>> Adjacency;
			for (const auto &[A, B] : Links[Id]) {
				Adjacency[A].push_back(B);
				Adjacency[B].push_back(A);
			}
			const auto Ends = std::ranges::count_if(Adjacency, [](const auto &Item) {
				return Item.second.size() == 1;
			});
			bool Bad = (Ends != 0 && Ends != 2) ||
				std::ranges::any_of(Adjacency, [](const auto &Item) { return Item.second.size() > 2; });
			std::vector Queue{Adjacency.begin()->first};
			std::set Seen{Queue.front()};
			for (std::size_t Index = 0; Index < Queue.size(); ++Index)
				for (const auto Neighbor : Adjacency.at(Queue[Index]))
					if (Seen.insert(Neighbor).second)
						Queue.push_back(Neighbor);
			Bad |= Seen.size() != Adjacency.size();
			Result.NonManifoldVertices += Bad;
		}
		return Result;
	}

	struct Vector {
		double X{}, Y{}, Z{};
		Vector operator-(Vector Other) const { return {X-Other.X, Y-Other.Y, Z-Other.Z}; }
	};

	Vector Cross(Vector A, Vector B) {
		return {A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X};
	}

	double Dot(Vector A, Vector B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }

	double AreaSquared(const SurfacePoint &A, const SurfacePoint &B, const SurfacePoint &C) {
		const auto Normal = Cross({B.X - A.X, B.Y - A.Y, B.Z - A.Z}, {C.X - A.X, C.Y - A.Y, C.Z - A.Z});
		return Dot(Normal, Normal);
	}

	std::size_t StrictIntersections(const SurfaceMesh &Mesh) {
		// Moller-style strict segment/triangle intersections, excluding boundary
		// endpoint contacts. A triangle belongs to its centroid's cell.
		using Triangle = std::array<Vector, 3>;
		const auto Intersects = [](const Triangle &A, const Triangle &B) {
			for (std::size_t Edge = 0; Edge < 3; ++Edge) {
				const auto Direction = A[(Edge + 1) % 3] - A[Edge];
				const auto E1 = B[1] - B[0], E2 = B[2] - B[0];
				const auto P = Cross(Direction, E2);
				const auto Det = Dot(E1, P);
				if (std::abs(Det) <= 1e-12)
					continue;
				const auto From = A[Edge] - B[0], Q = Cross(From, E1);
				const double U = Dot(From, P) / Det, V = Dot(Direction, Q) / Det, T = Dot(E2, Q) / Det;
				if (U > 1e-7 && V > 1e-7 && U + V < 1 - 1e-7 && T > 1e-7 && T < 1 - 1e-7)
					return true;
			}
			return false;
		};
		std::map<Coordinate, std::vector<Triangle>> Cells;
		for (std::size_t Index = 0; Index < Mesh.Indices.size(); Index += 3) {
			Triangle Points{};
			for (int Corner = 0; Corner < 3; ++Corner) {
				const auto [X,Y,Z] = Mesh.Vertices[Mesh.Indices[Index + Corner]].Position;
				Points[Corner] = {X,Y,Z};
			}
			const auto &[A,B,C] = Points;
			const auto Cell = [Spacing = Mesh.Spacing](double Sum) {
				return static_cast<int>(std::floor(Sum / (3 * Spacing)));
			};
			Cells[{Cell(A.X+B.X+C.X), Cell(A.Y+B.Y+C.Y), Cell(A.Z+B.Z+C.Z)}].push_back(Points);
		}
		std::size_t Count = 0;
		for (const auto &[Cell, Triangles] : Cells)
			for (std::size_t A = 0; A < Triangles.size(); ++A)
				for (std::size_t B = 0; B < A; ++B)
					Count += Intersects(Triangles[A], Triangles[B]) || Intersects(Triangles[B], Triangles[A]);
		return Count;
	}

	void TestCases() {
		// Indexing intentionally differs from the implementation's table.
		std::size_t Intersections = 0;
		std::uint32_t Seed = 0x57a39d22u;
		for (int Mask = 0; Mask < 256; ++Mask)
			for (int Variant = 0; Variant < 16; ++Variant) {
				std::array<int, 8> Densities{};
				for (int Corner = 0; Corner < 8; ++Corner) {
					Seed = Seed * 1664525u + 1013904223u;
					const int Magnitude = Variant == 0 ? 1 : 1 + static_cast<int>((Seed >> 16) % 32767);
					Densities[Corner] = (Mask & (1 << Corner)) ? -Magnitude : Magnitude;
				}
				const Field Source{{0,0,0}, 1, [&Densities](int X, int Y, int Z) {
					if (X < 0 || Y < 0 || Z < 0 || X > 1 || Y > 1 || Z > 1)
						return 900;
					return Densities[X + 2 * Y + 4 * Z];
				}};
				const auto Mesh = Extract(Source);
				Welded Whole;
				Whole.Add(Mesh, {0,0,0});
				const auto Topo = Audit(Whole, {0,0,0}, {1,1,1});
				if (Topo.InteriorOpen || Topo.NonManifoldEdges || Topo.UnorientedEdges ||
					Topo.NonManifoldVertices || Topo.WeldedDegenerate)
					throw std::runtime_error("bad cell topology at mask " + std::to_string(Mask) +
						", magnitudes variant " + std::to_string(Variant));
				Intersections += StrictIntersections(Mesh);
			}
		std::cout << "cell_cases=4096 strict_same_cell_intersections=" << Intersections << '\n';

		const auto Case4 = [](int Strength) {
			const Field Source{{0,0,0}, 1, [Strength](int X, int Y, int Z) {
				return (X == 0 && Y == 0 && Z == 0) || (X == 1 && Y == 1 && Z == 1) ? -Strength : 10;
			}};
			const auto Mesh = Extract(Source);
			Welded Whole;
			Whole.Add(Mesh, {0,0,0});
			return Audit(Whole, {0,0,0}, {1,1,1}).Components;
		};
		Require(Case4(1) == 2 && Case4(100) == 1, "interior saddle should change patch connectivity");
	}

	void TestSeams() {
		for (int Axis = 0; Axis < 3; ++Axis) {
			const auto U = (Axis + 1) % 3, V = (Axis + 2) % 3;
			const Coordinate Low{-36,-36,-36};
			auto High = Low;
			High[Axis] += 4;
			for (int Tie = 0; Tie < 2; ++Tie) {
				const auto Checker = [&](int X, int Y, int Z) {
					const Coordinate P{X,Y,Z};
					if (P[Axis] != High[Axis])
						return P[Axis] < High[Axis] ? -940 : 777;
					const bool Solid = (P[U] + P[V]) % 2 != 0;
					return Solid ? -100 : 100 + Tie;
				};
				const auto Left = Extract(Field{Low, 4, Checker}), Right = Extract(Field{High, 4, Checker});
				const auto Capture = [&](const SurfaceMesh &Mesh, float Plane) {
					std::map<std::array<float, 2>, std::pair<SurfacePoint, MaterialId>> Result;
					for (const auto &Vertex : Mesh.Vertices) {
						const std::array P{Vertex.Position.X, Vertex.Position.Y, Vertex.Position.Z};
						if (P[Axis] == Plane)
							Result[{P[U], P[V]}] = {Vertex.Normal, Vertex.Material};
					}
					return Result;
				};
				const auto LeftFace = Capture(Left, 4);
				Require(!LeftFace.empty() && LeftFace == Capture(Right, 0), "seam vertex, normal, or material");
				Welded Joined;
				Joined.Add(Left, Low);
				Joined.Add(Right, High);
				Coordinate Max{-32,-32,-32};
				Max[Axis] += 4;
				const auto Topo = Audit(Joined, Low, Max);
				Require(!Topo.InteriorOpen && !Topo.NonManifoldEdges && !Topo.NonManifoldVertices &&
					!Topo.UnorientedEdges, "seam topology");
			}
		}
	}

	void TestNormalsAndFailures() {
		const Field Source{{0,0,0}, 4, [](int X, int, int) { return (X - 2) * 200; }};
		auto Mesh = Extract(Source);
		Require(!Mesh.Indices.empty(), "zero-on-plane should use the air-side rule");
		for (const auto &Vertex : Mesh.Vertices)
			Require(Vertex.Normal == SurfacePoint{1,0,0} && Vertex.Position.X < 2 && Vertex.Position.X > 1.99f,
				"outward halo normal or explicit zero crossing");
		for (std::size_t I = 0; I < Mesh.Indices.size(); I += 3) {
			const auto A = Mesh.Vertices[Mesh.Indices[I]].Position;
			const auto B = Mesh.Vertices[Mesh.Indices[I + 1]].Position;
			const auto C = Mesh.Vertices[Mesh.Indices[I + 2]].Position;
			Require((B.Y-A.Y)*(C.Z-A.Z)-(B.Z-A.Z)*(C.Y-A.Y) > 0, "positive-area outward winding");
		}
		const auto Again = Extract(Source);
		Require(Mesh.Vertices == Again.Vertices && Mesh.Indices == Again.Indices, "deterministic replay");
		auto Paint = Source.Samples;
		for (auto &Value : Paint)
			if (Value.Density < 0)
				Value.Material += 100;
		Require(RefreshSurfaceMaterials(Paint, Mesh) == SurfaceStatus::Success, "paint with stable provenance");
		for (std::size_t V = 0; V < Mesh.Vertices.size(); ++V)
			Require(Mesh.Vertices[V].Material == Again.Vertices[V].Material + 100 &&
				Mesh.Vertices[V].Position == Again.Vertices[V].Position &&
				Mesh.Vertices[V].Normal == Again.Vertices[V].Normal, "paint preserves geometry");

		const auto Prior = Mesh;
		auto Invalid = Source.View();
		Invalid.Samples = Invalid.Samples.first(1);
		Require(ExtractSurface(Invalid, {}, Mesh) == SurfaceStatus::InvalidInput, "reject invalid halo");
		Invalid = Source.View();
		Invalid.Spacing = 1.1;
		Require(ExtractSurface(Invalid, {}, Mesh) == SurfaceStatus::InvalidInput, "reject unsupported spacing");
		Require(ExtractSurface(Source.View(), {64,1,6}, Mesh) == SurfaceStatus::WorkLimit, "vertex work admission");
		Require(ExtractSurface(Source.View(), {64,64,3}, Mesh) == SurfaceStatus::WorkLimit, "index work admission");
		std::stop_source Stop;
		Stop.request_stop();
		Require(ExtractSurface(Source.View(), {}, Mesh, Stop.get_token()) == SurfaceStatus::Canceled, "pre-cancel");
		Require(RefreshSurfaceMaterials(Invalid.Samples.first(0), Mesh) == SurfaceStatus::InvalidInput,
			"invalid rebind atomically denied");
		Require(Mesh.Vertices == Prior.Vertices && Mesh.Indices == Prior.Indices, "denial preserves prepared output");
	}

	void TestCollision() {
		for (const double Spacing : {0.5, 1.0, 2.0}) {
			const Field Source{{0,0,0}, 2, [](int X, int Y, int Z) {
				return X == 1 && Y == 1 && Z == 1 ? -1 : 32767;
			}};
			const auto Mesh = Extract(Source, Spacing);
			CollisionSurfaceMesh Collision;
			Require(BuildCollisionSurface(Mesh, {0,1.0e-10f}, Collision) == SurfaceStatus::Degenerate,
				"provider predicate rejects unconditioned near-corner triangles");
			Require(BuildCollisionSurface(Mesh, {}, Collision) == SurfaceStatus::Success, "inset admits closed sliver fixture");
			Require(Mesh.Indices == Collision.Indices && Collision.MaximumDisplacement <= Spacing / 64 &&
				Collision.TriangleMaterials.size() == Mesh.Indices.size() / 3, "bounded conditioning preserves faces");
			const auto Prior = Collision;
			Require(BuildCollisionSurface(Mesh, {0,1.0e-10f}, Collision) == SurfaceStatus::Degenerate,
				"second rejection");
			Require(Collision.Positions == Prior.Positions && Collision.Indices == Prior.Indices,
				"collision conditioning fails without publishing partial geometry");
		}
	}

	void TestThinSampling() {
		const auto Slab = [](int Center) {
			return Field{{0,0,0}, 4, [Center](int X, int, int) {
				return std::abs(10 * X - Center) - 4;
			}};
		};
		Require(Extract(Slab(10)).Indices.size() != 0 && Extract(Slab(15)).Indices.empty(),
			"subsample slab exhibits expected alignment limit");
	}

	void CompareResearch(const std::filesystem::path &Directory) {
		constexpr int Extent = 64;
		constexpr int Width = Extent + 1;
		std::cout << "scene,side,triangles,components,interior_open,nonmanifold_edges,unoriented_edges,"
			"nonmanifold_vertices,weld_degenerate,strict_intersections,raw_provider_slivers,conditioned_denials,max_inset\n";
		for (const std::string_view Scene : {"rolling","mountain","cave","island","sparse","dense","collision"}) {
			std::ifstream In(Directory / (std::string(Scene) + ".f32"), std::ios::binary);
			std::vector<float> Densities(Width * Width * Width);
			In.read(reinterpret_cast<char *>(Densities.data()), Densities.size() * sizeof(float));
			Require(In.good(), "retained field bytes");
			const auto Read = [&](int X, int Y, int Z) {
				X = std::clamp(X, 0, Extent); Y = std::clamp(Y, 0, Extent); Z = std::clamp(Z, 0, Extent);
				return static_cast<int>(std::lround(Densities[(Z * Width + Y) * Width + X] * (32767.0 / 4.0)));
			};
			for (const int Side : {32,64}) {
				Welded Volume;
				std::size_t SliverCount = 0, Denials = 0, Intersections = 0;
				float MaximumInset = 0;
				for (int Z = 0; Z < Extent; Z += Side)
					for (int Y = 0; Y < Extent; Y += Side)
						for (int X = 0; X < Extent; X += Side) {
							const Field Current{{X,Y,Z}, static_cast<std::uint32_t>(Side), Read};
							const auto Mesh = Extract(Current);
							Volume.Add(Mesh, {X,Y,Z});
							for (std::size_t I = 0; I < Mesh.Indices.size(); I += 3)
								SliverCount += AreaSquared(Mesh.Vertices[Mesh.Indices[I]].Position,
									Mesh.Vertices[Mesh.Indices[I + 1]].Position,
									Mesh.Vertices[Mesh.Indices[I + 2]].Position) < 1.0e-10;
							Intersections += StrictIntersections(Mesh);
							CollisionSurfaceMesh Collision;
							if (BuildCollisionSurface(Mesh, {}, Collision) == SurfaceStatus::Success)
								MaximumInset = std::max(MaximumInset, Collision.MaximumDisplacement);
							else
								++Denials;
						}
				const auto Topo = Audit(Volume, {0,0,0}, {Extent,Extent,Extent});
				std::cout << Scene << ',' << Side << ',' << Volume.Triangles.size() << ',' << Topo.Components << ','
					<< Topo.InteriorOpen << ',' << Topo.NonManifoldEdges << ',' << Topo.UnorientedEdges << ','
					<< Topo.NonManifoldVertices << ',' << Topo.WeldedDegenerate << ',' << Intersections << ','
					<< SliverCount << ',' << Denials << ',' << MaximumInset << '\n';
			}
		}
	}
}

int main(int Argc, char **Argv) {
	try {
		TestCases();
		TestSeams();
		TestNormalsAndFailures();
		TestCollision();
		TestThinSampling();
		if (Argc == 2)
			CompareResearch(Argv[1]);
		std::cout << "Terrain surface checks passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Terrain:SurfaceTests] " << Error.what() << '\n';
		return 1;
	}
}
