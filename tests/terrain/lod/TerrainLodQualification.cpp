// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/Surface.hpp"
#include "terrain/VisualDensity.hpp"
#include "terrain/VisualDensityOverlap.hpp"
#include "TerrainVisualOverlapAudit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>

namespace {
    using namespace gargantuan::terrain;
    using Index = std::uint32_t;
    using Edge = std::array<Index, 2>;
    using Triangle = std::array<Index, 3>;
    using Clock = std::chrono::steady_clock;
    int Extent = 16;
    constexpr double MaximumMove = 1.25;
    constexpr std::size_t MaximumAttempts = 4096;
    constexpr float CollarWidth = 1.0f;

    void Require(bool Condition, std::string_view Explanation) {
        if (!Condition) {
            std::cerr << "[Terrain:LOD] " << Explanation << '\n';
            std::exit(2);
        }
    }

    enum class Scene { Flat, Rolling, Cave, ThinCave, Island, Saddle };

    constexpr std::array Names{"Flat", "Rolling", "Cave", "ThinCave", "Island", "Saddle"};

    Sample Evaluate(Scene Fixture, double Along, double Up, double Across, double EditOffset) {
        const double CaveFloor = 7.0 + 0.16 * std::sin(Along * 0.32);
        double Density = 0.0;
        switch (Fixture) {
        case Scene::Flat:
            Density = Up - 8.25;
            break;
        case Scene::Rolling:
            Density = Up - (8.11 + 1.3 * std::sin(Along * 0.34) + 0.8 * std::cos(Across * 0.39));
            break;
        case Scene::Cave:
            Density = std::max(Up - 12.67,
                2.13 - std::hypot(Up - CaveFloor, Across - 8.8 - 0.36 * std::cos(Along * 0.31)));
            break;
        case Scene::ThinCave:
            Density = std::max(Up - 13.21, 0.82 - std::hypot(Up - CaveFloor, Across - 9.0));
            break;
        case Scene::Island: {
            const double Ellipsoid = 4.0 * (std::hypot((Along - 16.08) / 13.0,
                (Up - 8.11) / 4.1, (Across - 7.88) / 5.1) - 1.0);
            Density = std::max(Ellipsoid, 1.43 - std::hypot(Up - 7.8, Across - 7.8));
            break;
        }
        case Scene::Saddle:
            Density = 0.29 * (Up - 6.51) * (Across - 8.11) + 0.15 * std::sin(Along * 0.63) + 0.065;
            break;
        }
        Density += EditOffset;
        const auto Magnitude = std::clamp(std::llround(std::abs(Density) * 901), 1LL, 32767LL);
        const auto Value = static_cast<std::int16_t>(Density < 0 ? -Magnitude : Magnitude);
        const MaterialId Material = Fixture == Scene::Island && Up > 8.0 ? 0x80000011u : 0xFEDCA901u;
        return {Value, Value < 0 ? Material : 0};
    }

    SurfaceMesh Extract(Scene Fixture, unsigned NormalAxis, int Part, int Step, double EditOffset = 0,
        int CrossPart = 0) {
        const auto Cells = Extent / Step;
        const auto Padded = Cells + 3;
        std::vector<Sample> Samples;
        Samples.reserve(static_cast<std::size_t>(Padded) * Padded * Padded);
        for (int Z = -1; Z <= Cells + 1; ++Z) {
            for (int Y = -1; Y <= Cells + 1; ++Y) {
                for (int X = -1; X <= Cells + 1; ++X) {
                    const std::array Local{X * Step, Y * Step, Z * Step};
                    const double Along = (Local[NormalAxis] + Part * Extent) * (16.0 / Extent);
                    Samples.push_back(Evaluate(Fixture, Along, Local[(NormalAxis + 1) % 3],
                        Local[(NormalAxis + 2) % 3] + CrossPart * Extent, EditOffset));
                }
            }
        }
        const auto CellCount = static_cast<std::uint32_t>(Cells);
        SurfaceMesh Mesh;
        Require(ExtractSurface({Samples, CellCount, CellCount, CellCount, static_cast<double>(Step)}, {}, Mesh) ==
            SurfaceStatus::Success, "MC33 extraction failed");
        return Mesh;
    }

    Edge MakeEdge(Index A, Index B) {
        return {std::min(A, B), std::max(A, B)};
    }

    struct MeshConnectivity {
        std::vector<Triangle> Faces;
        std::vector<std::vector<Index>> Fans;
        std::vector<std::vector<Index>> Neighbors;
        std::map<Edge, std::vector<Index>> Edges;

        explicit MeshConnectivity(const SurfaceMesh &Mesh)
            : Fans(Mesh.Vertices.size()), Neighbors(Mesh.Vertices.size()) {
            for (std::size_t I = 0; I < Mesh.Indices.size(); I += 3) {
                const Triangle Face{Mesh.Indices[I], Mesh.Indices[I + 1], Mesh.Indices[I + 2]};
                const auto FaceIndex = static_cast<Index>(Faces.size());
                Faces.push_back(Face);
                for (const auto Vertex : Face)
                    Fans[Vertex].push_back(FaceIndex);
                for (unsigned J = 0; J < 3; ++J)
                    Edges[MakeEdge(Face[J], Face[(J + 1) % 3])].push_back(FaceIndex);
            }
            for (const auto &[Pair, FacesAtEdge] : Edges) {
                Neighbors[Pair[0]].push_back(Pair[1]);
                Neighbors[Pair[1]].push_back(Pair[0]);
            }
            for (auto &List : Neighbors)
                std::ranges::sort(List);
        }
    };

    std::array<double, 3> Sub(SurfacePoint A, SurfacePoint B) {
        return {static_cast<double>(A.X) - B.X, static_cast<double>(A.Y) - B.Y, static_cast<double>(A.Z) - B.Z};
    }

    double Dot(std::array<double, 3> A, std::array<double, 3> B) {
        return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
    }

    std::array<double, 3> FaceNormal(const SurfaceMesh &Mesh, Triangle Face) {
        const auto A = Sub(Mesh.Vertices[Face[1]].Position, Mesh.Vertices[Face[0]].Position);
        const auto B = Sub(Mesh.Vertices[Face[2]].Position, Mesh.Vertices[Face[0]].Position);
        return {A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0]};
    }

    bool FaceHasLinkEdge(const MeshConnectivity &Mesh, Index Vertex, Edge LinkEdge) {
        return std::ranges::any_of(Mesh.Fans[Vertex], [&](Index Face) {
            return std::ranges::find(Mesh.Faces[Face], LinkEdge[0]) != Mesh.Faces[Face].end() &&
                std::ranges::find(Mesh.Faces[Face], LinkEdge[1]) != Mesh.Faces[Face].end();
        });
    }

    bool LinkCondition(const MeshConnectivity &Mesh, Edge Pair, std::span<const Index> IncidentFaces) {
        if (IncidentFaces.size() != 2)
            return false;

        Edge Common{};
        std::size_t Count = 0;
        for (const auto A : Mesh.Neighbors[Pair[0]]) {
            if (!std::ranges::binary_search(Mesh.Neighbors[Pair[1]], A))
                continue;
            if (Count == Common.size())
                return false;
            Common[Count++] = A;
        }
        if (Count != Common.size())
            return false;

        const auto Contains = [&](std::size_t Face, Index Vertex) {
            const auto &Triangle = Mesh.Faces[IncidentFaces[Face]];
            return std::ranges::find(Triangle, Vertex) != Triangle.end();
        };
        if (!(Contains(0, Common[0]) && Contains(1, Common[1])) &&
            !(Contains(0, Common[1]) && Contains(1, Common[0])))
            return false;

        // A shared link edge would collapse the two remaining faces of a
        // tetrahedral component onto one triangle.
        return !(FaceHasLinkEdge(Mesh, Pair[0], Common) && FaceHasLinkEdge(Mesh, Pair[1], Common));
    }

    bool TouchesCollar(SurfacePoint Point) {
        const auto Near = [](float Value) { return Value <= CollarWidth || Value >= Extent - CollarWidth; };
        return Near(Point.X) || Near(Point.Y) || Near(Point.Z);
    }

    bool ValidContraction(const SurfaceMesh &Mesh, const MeshConnectivity &Connections, Edge Pair,
        std::span<const std::uint8_t> Blocked) {
        const auto Removed = Pair[0];
        const auto Retained = Pair[1];
        const auto Material = Mesh.Vertices[Retained].Material;
        const auto Suitable = [&](Index Vertex) { return Blocked[Vertex] == 0 && Mesh.Vertices[Vertex].Material == Material; };

        for (Index Center : Pair) {
            if (!Suitable(Center) || !std::ranges::all_of(Connections.Neighbors[Center], Suitable))
                return false;
        }
        const auto KeepNormal = Mesh.Vertices[Retained].Normal;
        const auto OldNormal = Mesh.Vertices[Removed].Normal;
        if (Dot({KeepNormal.X, KeepNormal.Y, KeepNormal.Z}, {OldNormal.X, OldNormal.Y, OldNormal.Z}) < 0.975)
            return false;

        for (const auto FaceIndex : Connections.Fans[Removed]) {
            const auto Face = Connections.Faces[FaceIndex];
            if (std::ranges::find(Face, Retained) != Face.end())
                continue; // exactly the two faces removed with the edge
            auto Replacement = Face;
            std::ranges::replace(Replacement, Removed, Retained);
            const auto Before = FaceNormal(Mesh, Face);
            const auto After = FaceNormal(Mesh, Replacement);
            const double Area = Dot(After, After);
            if (Area < 1.0e-12 || Dot(Before, After) < 0.95 * std::sqrt(Area * Dot(Before, Before)))
                return false;
        }
        return true;
    }

    struct Reduction {
        SurfaceMesh Mesh;
        std::vector<double> Displacement;
        std::size_t Collapses{};
        double MaximumDisplacement{};
    };

    struct ReducerProfile {
        double ConnectivityUs{};
        double CandidatesUs{};
        double ContractionUs{};
        double OutputUs{};
    };

    double Micros(Clock::time_point Begin, Clock::time_point End) {
        return std::chrono::duration<double, std::micro>(End - Begin).count();
    }

    Reduction ReduceInterior(const SurfaceMesh &Input, std::span<const double> PreviousDisplacement = {},
        ReducerProfile *Profile = nullptr) {
        const auto Started = Clock::now();
        const MeshConnectivity Connections(Input);
        const auto Connected = Clock::now();
        std::vector<std::uint8_t> Blocked(Input.Vertices.size(), 0);
        for (std::size_t I = 0; I < Input.Vertices.size(); ++I)
            Blocked[I] = TouchesCollar(Input.Vertices[I].Position);
        for (const auto &[Pair, IncidentFaces] : Connections.Edges) {
            if (IncidentFaces.size() != 2) {
                Blocked[Pair[0]] = true;
                Blocked[Pair[1]] = true;
            }
        }

        std::vector<std::pair<double, Edge>> Candidates;
        for (const auto &[Pair, IncidentFaces] : Connections.Edges) {
            if (Blocked[Pair[0]] || Blocked[Pair[1]])
                continue;
            const auto Delta = Sub(Input.Vertices[Pair[0]].Position, Input.Vertices[Pair[1]].Position);
            const auto LengthSquared = Dot(Delta, Delta);
            if (LengthSquared <= MaximumMove * MaximumMove && LinkCondition(Connections, Pair, IncidentFaces))
                Candidates.emplace_back(LengthSquared, Pair);
        }
        std::ranges::sort(Candidates);
        const auto Ranked = Clock::now();
        std::vector<Index> Contraction(Input.Vertices.size());
        std::iota(Contraction.begin(), Contraction.end(), 0u);
        std::vector<double> Bounds(Input.Vertices.size());
        if (!PreviousDisplacement.empty())
            std::ranges::copy(PreviousDisplacement, Bounds.begin());

        Reduction Result;
        Result.Mesh.Spacing = Input.Spacing;
        for (const auto &[LengthSquared, Pair] : Candidates | std::views::take(MaximumAttempts)) {
            const auto NextBound = Bounds[Pair[0]] + std::sqrt(LengthSquared);
            if (NextBound > MaximumMove) continue;
            if (!ValidContraction(Input, Connections, Pair, Blocked))
                continue;
            for (const auto Vertex : Pair) {
                Blocked[Vertex] = true;
                for (const auto Neighbor : Connections.Neighbors[Vertex])
                    Blocked[Neighbor] = true;
            }
            Contraction[Pair[0]] = Pair[1];
            Bounds[Pair[1]] = std::max(Bounds[Pair[1]], NextBound);
            ++Result.Collapses;
        }
        const auto Contracted = Clock::now();

        constexpr auto Unassigned = ~Index{0};
        std::vector<Index> Remap(Input.Vertices.size(), Unassigned);
        for (auto Face : Connections.Faces) {
            for (auto &Vertex : Face)
                Vertex = Contraction[Vertex];
            if (Face[0] == Face[1] || Face[1] == Face[2] || Face[2] == Face[0])
                continue;
            for (const auto Vertex : Face) {
                if (Remap[Vertex] == Unassigned) {
                    Remap[Vertex] = static_cast<Index>(Result.Mesh.Vertices.size());
                    Result.Mesh.Vertices.push_back(Input.Vertices[Vertex]);
                    Result.Displacement.push_back(Bounds[Vertex]);
                    Result.MaximumDisplacement = std::max(Result.MaximumDisplacement, Bounds[Vertex]);
                }
                Result.Mesh.Indices.push_back(Remap[Vertex]);
            }
        }
        if (Profile) {
            Profile->ConnectivityUs += Micros(Started, Connected);
            Profile->CandidatesUs += Micros(Connected, Ranked);
            Profile->ContractionUs += Micros(Ranked, Contracted);
            Profile->OutputUs += Micros(Contracted, Clock::now());
        }
        return Result;
    }

    Reduction ReduceInteriorPasses(const SurfaceMesh &Input, unsigned Passes, ReducerProfile *Profile = nullptr) {
        auto Result = ReduceInterior(Input, {}, Profile);
        for (unsigned Pass = 1; Pass < Passes && Result.Collapses; ++Pass) {
            auto Next = ReduceInterior(Result.Mesh, Result.Displacement, Profile);
            if (!Next.Collapses) break;
            Next.Collapses += Result.Collapses;
            Result = std::move(Next);
        }
        return Result;
    }

    struct Topology {
        std::size_t Components{};
        std::int64_t Euler{};
        std::size_t BoundaryEdges{};
        std::size_t NonManifoldEdges{};
        std::size_t NonManifoldVertices{};
        bool operator==(const Topology &) const = default;
    };

    bool HasSingleVertexLink(const MeshConnectivity &Connections, Index Center) {
        const auto &Adjacent = Connections.Neighbors[Center];
        std::vector<std::vector<std::size_t>> Link(Adjacent.size());
        for (const auto FaceIndex : Connections.Fans[Center]) {
            std::array<std::size_t, 2> Other{};
            std::size_t At = 0;
            for (const auto Vertex : Connections.Faces[FaceIndex]) {
                if (Vertex == Center)
                    continue;
                const auto Neighbor = std::ranges::lower_bound(Adjacent, Vertex);
                Other[At++] = static_cast<std::size_t>(Neighbor - Adjacent.begin());
            }
            if (At != 2 || std::ranges::find(Link[Other[0]], Other[1]) != Link[Other[0]].end())
                return false;
            Link[Other[0]].push_back(Other[1]);
            Link[Other[1]].push_back(Other[0]);
        }
        std::size_t Endpoints = 0;
        for (const auto &Neighbors : Link) {
            if (Neighbors.empty() || Neighbors.size() > 2)
                return false;
            Endpoints += Neighbors.size() == 1;
        }
        if (Endpoints != 0 && Endpoints != 2)
            return false;

        std::vector<std::uint8_t> Seen(Link.size(), 0);
        std::vector<std::size_t> Open{0};
        Seen[0] = true;
        std::size_t SeenCount = 0;
        while (!Open.empty()) {
            const auto Position = Open.back();
            Open.pop_back();
            ++SeenCount;
            for (auto Neighbor : Link[Position]) {
                if (!Seen[Neighbor]) {
                    Seen[Neighbor] = true;
                    Open.push_back(Neighbor);
                }
            }
        }
        return SeenCount == Link.size();
    }

    Topology ExamineTopology(const SurfaceMesh &Mesh) {
        const MeshConnectivity Connections(Mesh);
        Topology Result{};
        std::vector<std::uint8_t> Visited(Mesh.Vertices.size(), 0);
        std::vector<Index> Stack;
        std::size_t Used = 0;
        for (Index I = 0; I < Mesh.Vertices.size(); ++I) {
            if (Connections.Fans[I].empty())
                continue;
            ++Used;
            Result.NonManifoldVertices += !HasSingleVertexLink(Connections, I);
            if (Visited[I])
                continue;
            ++Result.Components;
            Stack.push_back(I);
            Visited[I] = true;
            while (!Stack.empty()) {
                const auto At = Stack.back();
                Stack.pop_back();
                for (Index Other : Connections.Neighbors[At]) {
                    if (!Visited[Other]) {
                        Visited[Other] = true;
                        Stack.push_back(Other);
                    }
                }
            }
        }
        Result.Euler = static_cast<std::int64_t>(Used) - static_cast<std::int64_t>(Connections.Edges.size()) +
            static_cast<std::int64_t>(Connections.Faces.size());
        for (const auto &[Key, Faces] : Connections.Edges) {
            Result.BoundaryEdges += Faces.size() == 1;
            Result.NonManifoldEdges += Faces.size() > 2;
        }
        return Result;
    }

    using FinePoint = std::array<std::int64_t, 2>;
    using SeamEdge = std::array<FinePoint, 2>;

    struct SeamAttributes {
        MaterialId Material;
        std::array<std::int64_t, 3> Normal;
        auto operator<=>(const SeamAttributes &) const = default;
    };

    struct OrientedSeamEdge {
        std::array<SeamAttributes, 2> Attributes;
        bool Forward;
    };

    using SeamReceipt = std::map<SeamEdge, std::vector<OrientedSeamEdge>>;

    SeamReceipt CaptureSeam(const SurfaceMesh &Mesh, unsigned Axis, double Plane) {
        const MeshConnectivity Connections(Mesh);
        SeamReceipt Result;
        const auto GetPoint = [Axis](SurfacePoint Point) -> FinePoint {
            const std::array Coordinates{Point.X, Point.Y, Point.Z};
            return {std::llround(Coordinates[(Axis + 1) % 3] * 100'000.0),
                std::llround(Coordinates[(Axis + 2) % 3] * 100'000.0)};
        };
        const auto OnPlane = [Axis, Plane](SurfacePoint Point) {
            const std::array Coordinates{Point.X, Point.Y, Point.Z};
            return std::abs(Coordinates[Axis] - Plane) < 1.0e-5;
        };
        const auto Attributes = [](const SurfaceVertex &Vertex) {
            return SeamAttributes{Vertex.Material, {std::llround(Vertex.Normal.X * 1'000.0),
                std::llround(Vertex.Normal.Y * 1'000.0), std::llround(Vertex.Normal.Z * 1'000.0)}};
        };
        for (const auto &[Pair, Faces] : Connections.Edges) {
            const auto &A = Mesh.Vertices[Pair[0]];
            const auto &B = Mesh.Vertices[Pair[1]];
            if (Faces.size() != 1 || !OnPlane(A.Position) || !OnPlane(B.Position))
                continue;
            const auto First = GetPoint(A.Position);
            const auto Second = GetPoint(B.Position);
            const auto Face = Connections.Faces[Faces.front()];
            const auto At = static_cast<std::size_t>(std::ranges::find(Face, Pair[0]) - Face.begin());
            const bool Forward = Pair[1] == Face[(At + 1) % 3];
            if (First < Second)
                Result[{First, Second}].push_back({{Attributes(A), Attributes(B)}, Forward});
            else
                Result[{Second, First}].push_back({{Attributes(B), Attributes(A)}, !Forward});
        }
        return Result;
    }

    bool MatchOrientedSeam(const SeamReceipt &Left, const SeamReceipt &Right) {
        return Left.size() == Right.size() && std::ranges::equal(Left, Right, [](const auto &A, const auto &B) {
            return A.first == B.first && A.second.size() == 1 && B.second.size() == 1 &&
                A.second.front().Attributes == B.second.front().Attributes &&
                A.second.front().Forward != B.second.front().Forward;
        });
    }

    std::size_t DifferingEdges(const SeamReceipt &A, const SeamReceipt &B) {
        const auto Unique = [](const SeamReceipt &First, const SeamReceipt &Second) {
            return std::ranges::count_if(First, [&](const auto &Entry) {
                return !Second.contains(Entry.first);
            });
        };
        return static_cast<std::size_t>(Unique(A, B) + Unique(B, A));
    }

    void RequireParity(const SurfaceMesh &Expected, const SurfaceMesh &Actual) {
        Require(Expected.Spacing == Actual.Spacing && Expected.Vertices == Actual.Vertices &&
            Expected.Indices == Actual.Indices, "packed reducer differs from reference mesh");
    }

    void CompareIndexed(Scene Fixture, unsigned Axis, const SurfaceMesh &Input, const Reduction &Expected) {
        constexpr std::size_t Repetitions = 7;
        std::array<double, Repetitions> Reference{}, Indexed{};
        SurfaceMesh Output;
        VisualDensityMetrics Metrics;
        const auto RunIndexed = [&](std::size_t Index) {
            const auto Started = Clock::now();
            const auto Status = BuildVisualDensity(Input, ChunkSide, 4, Output, Metrics);
            Indexed[Index] = Micros(Started, Clock::now());
            Require(Status == SurfaceStatus::Success, "packed candidate input refused");
        };
        for (std::size_t I = 0; I < Repetitions; ++I) {
            if ((I & 1u) != 0) RunIndexed(I);
            const auto Started = Clock::now();
            const auto Ref = ReduceInteriorPasses(Input, 4);
            Reference[I] = Micros(Started, Clock::now());
            if ((I & 1u) == 0) RunIndexed(I);
            RequireParity(Ref.Mesh, Output);
        }
        RequireParity(Expected.Mesh, Output);
        Require(Metrics.MaximumDisplacement == Expected.MaximumDisplacement && Metrics.Collapses == Expected.Collapses,
            "packed collapse and displacement receipts differ");
        Require(Metrics.OverlapPairs > 0, "changed faces bypassed spatial guard");
        std::ranges::sort(Reference);
        std::ranges::sort(Indexed);
        std::cout << "Indexed," << Names[static_cast<std::size_t>(Fixture)] << ',' << Axis << ','
            << Repetitions << ',' << Reference[3] << ',' << Indexed[3] << ',' << Reference.back() << ',' << Indexed.back()
            << ',' << Output.Indices.size() / 3 << ',' << Output.Vertices.size() << ',' << Metrics.OverlapPairs << '\n';
    }

    void Compare(Scene Fixture, unsigned Axis, unsigned Passes, bool ProfileReference, bool IndexedComparison) {
        const auto First = Extract(Fixture, Axis, 0, 1);
        const auto Second = Extract(Fixture, Axis, 1, 1);
        const auto Coarse = Extract(Fixture, Axis, 1, 2);
        const auto LeftSeam = CaptureSeam(First, Axis, Extent);
        const auto RightSeam = CaptureSeam(Second, Axis, 0);
        const auto RawSeam = CaptureSeam(Coarse, Axis, 0);
        Require(!LeftSeam.empty() && MatchOrientedSeam(LeftSeam, RightSeam), "native equal resolution control failed");
        const auto Start = Clock::now();
        ReducerProfile Profile;
        const auto Reduced = ReduceInteriorPasses(Second, Passes, ProfileReference ? &Profile : nullptr);
        const auto Duration = std::chrono::duration<double, std::micro>(Clock::now() - Start).count();
        const auto CandidateSeam = CaptureSeam(Reduced.Mesh, Axis, 0);
        const auto OriginalTopology = ExamineTopology(Second);
        const auto ReducedTopology = ExamineTopology(Reduced.Mesh);
        const auto RawTopology = ExamineTopology(Coarse);
        Require(MatchOrientedSeam(LeftSeam, CandidateSeam), "border-locked candidate changed a seam attribute or segment");
        Require(OriginalTopology.NonManifoldEdges == 0 && OriginalTopology.NonManifoldVertices == 0,
            "fine reference requires manifold indexed vertex links and edge incidence");
        Require(OriginalTopology == ReducedTopology, "border-locked candidate changed measured topology");
        Require(DifferingEdges(LeftSeam, RawSeam) > 0, "raw 2:1 negative control lost distinction");
        if (Fixture == Scene::ThinCave)
            Require(RawTopology.Components < OriginalTopology.Components, "raw control should lose the thin cave");

        std::cout << Names[static_cast<std::size_t>(Fixture)] << ',' << Axis << ',' << Second.Indices.size() / 3
            << ',' << Coarse.Indices.size() / 3 << ',' << Reduced.Mesh.Indices.size() / 3
            << ',' << LeftSeam.size() << ',' << DifferingEdges(LeftSeam, RawSeam)
            << ',' << DifferingEdges(LeftSeam, CandidateSeam) << ',' << OriginalTopology.Components
            << ',' << RawTopology.Components << ',' << ReducedTopology.Components << ',' << Reduced.Collapses
            << ',' << Reduced.MaximumDisplacement << ',' << Duration << '\n';
        if (ProfileReference)
            std::cout << "Profile," << Names[static_cast<std::size_t>(Fixture)] << ',' << Axis << ','
                << Profile.ConnectivityUs << ',' << Profile.CandidatesUs << ',' << Profile.ContractionUs
                << ',' << Profile.OutputUs << '\n';
        if (IndexedComparison) CompareIndexed(Fixture, Axis, Second, Reduced);
    }

    void CheckChangedBoundary(bool IndexedComparison) {
        const auto Left = Extract(Scene::Cave, 0, 0, 1);
        const auto NewLeft = Extract(Scene::Cave, 0, 0, 1, 0.26);
        const auto Source = Extract(Scene::Cave, 0, 1, 1, 0.26);
        const auto NewRight = ReduceInterior(Source);
        const auto OldBorder = CaptureSeam(Left, 0, Extent);
        const auto NewBorder = CaptureSeam(NewLeft, 0, Extent);
        const auto Prepared = CaptureSeam(NewRight.Mesh, 0, 0);
        Require(MatchOrientedSeam(NewBorder, Prepared) && DifferingEdges(OldBorder, Prepared) > 0,
            "a stale neighbor must not admit a transition across changed halo samples");
        if (IndexedComparison) {
            SurfaceMesh Output;
            VisualDensityMetrics Metrics;
            Require(BuildVisualDensity(Source, ChunkSide, 1, Output, Metrics) == SurfaceStatus::Success,
                "changed field packed candidate failed");
            RequireParity(NewRight.Mesh, Output);
        }
    }

    struct VisualSource {
        std::uint64_t Generation;
        std::uint64_t GeometryRevision;
        std::uint64_t MaterialRevision;
        std::array<std::uint64_t, 6> HaloRevisions;
        bool operator==(const VisualSource &) const = default;
    };

    class ViewSelection {
      public:
        bool Select(double DistanceInChunks, const VisualSource &Current, std::optional<VisualSource> Reduced) {
            if (!Reduced || *Reduced != Current || !std::isfinite(DistanceInChunks) || DistanceInChunks <= 2.0)
                UsingReduced = false;
            else if (DistanceInChunks >= 3.0)
                UsingReduced = true;
            return UsingReduced;
        }
      private:
        bool UsingReduced = false;
    };

    void CheckCornerAndViewSequence(bool IndexedComparison) {
        std::array<SurfaceMesh, 4> Fine;
        std::array<Reduction, 4> Reduced;
        for (int Row = 0; Row < 2; ++Row)
            for (int Column = 0; Column < 2; ++Column) {
                const auto Index = static_cast<std::size_t>(2 * Row + Column);
                Fine[Index] = Extract(Scene::Flat, 0, Column - 1, 1, 0, Row - 1);
                Reduced[Index] = ReduceInteriorPasses(Fine[Index], 4);
                if (IndexedComparison) {
                    SurfaceMesh Output;
                    VisualDensityMetrics Metrics;
                    Require(BuildVisualDensity(Fine[Index], ChunkSide, 4, Output, Metrics) == SurfaceStatus::Success,
                        "packed corner candidate failed");
                    RequireParity(Reduced[Index].Mesh, Output);
                    Reduced[Index].Mesh = std::move(Output);
                }
                Require(ExamineTopology(Fine[Index]) == ExamineTopology(Reduced[Index].Mesh),
                    "four-pass negative-coordinate tile changes indexed topology");
                Require(Reduced[Index].MaximumDisplacement <= MaximumMove,
                    "multi-pass displacement exceeds the authored bound");
            }

        for (unsigned Mask = 0; Mask < 16; ++Mask) {
            const auto Mesh = [&](unsigned Index) -> const SurfaceMesh & {
                return Mask & (1u << Index) ? Reduced[Index].Mesh : Fine[Index];
            };
            for (const auto &Pair : {std::array{0u, 1u, 0u}, std::array{2u, 3u, 0u},
                     std::array{0u, 2u, 2u}, std::array{1u, 3u, 2u}}) {
                const auto Left = CaptureSeam(Mesh(Pair[0]), Pair[2], Extent);
                const auto Right = CaptureSeam(Mesh(Pair[1]), Pair[2], 0);
                Require(!Left.empty() && MatchOrientedSeam(Left, Right),
                    "neighbor refinement changed a boundary at a four-tile corner");
            }
        }

        const VisualSource Source{7, 10, 11, {20, 21, 22, 23, 24, 25}};
        ViewSelection View;
        Require(!View.Select(2.8, Source, Source) && View.Select(3.2, Source, Source) &&
            View.Select(2.9, Source, Source) && View.Select(2.1, Source, Source) &&
            !View.Select(1.95, Source, Source) && !View.Select(2.85, Source, Source),
            "camera traversal oscillated inside the hysteresis band");
        for (int Changed = 0; Changed < 4; ++Changed) {
            auto New = Source;
            if (Changed == 0) ++New.Generation;
            if (Changed == 1) ++New.GeometryRevision;
            if (Changed == 2) ++New.MaterialRevision;
            if (Changed == 3) ++New.HaloRevisions[2];
            Require(View.Select(3.3, Source, Source) && !View.Select(3.3, New, Source) &&
                !View.Select(3.3, New, std::nullopt) && View.Select(3.3, New, New),
                "a missing or stale visual candidate did not fall back to the current fine mesh");
        }
        std::cout << "Checks,CornerCombinations,16\nChecks,ViewHysteresisAndRevisionFallback,Pass\n";
    }

    void CheckIndexedLimits() {
        const auto Fine = Extract(Scene::Flat, 0, 0, 1);
        SurfaceMesh Output;
        VisualDensityMetrics Metrics;
        Require(BuildVisualDensity(Fine, ChunkSide, 4, Output, Metrics) == SurfaceStatus::Success, "base reduction");
        const auto Previous = Output;
        const auto PreviousCount = Metrics.Collapses;
        auto Invalid = Fine;
        Invalid.Indices.front() = static_cast<std::uint32_t>(Invalid.Vertices.size());
        Require(BuildVisualDensity(Invalid, ChunkSide, 4, Output, Metrics) == SurfaceStatus::InvalidInput,
            "bad triangle index requires fine fallback");
        Require(BuildVisualDensity(Fine, ChunkSide, 5, Output, Metrics) == SurfaceStatus::InvalidInput,
            "too many passes denied");
        SurfaceMesh TooLarge;
        TooLarge.Vertices.resize(65'537);
        Require(BuildVisualDensity(TooLarge, ChunkSide, 4, Output, Metrics) == SurfaceStatus::WorkLimit,
            "vertex cap enforced before adjacency allocation");
        auto NonManifold = Fine;
        NonManifold.Indices.insert(NonManifold.Indices.end(), Fine.Indices.begin(), Fine.Indices.begin() + 3);
        NonManifold.Indices.insert(NonManifold.Indices.end(), Fine.Indices.begin(), Fine.Indices.begin() + 3);
        Require(BuildVisualDensity(NonManifold, ChunkSide, 4, Output, Metrics) == SurfaceStatus::Degenerate,
            "edges with three uses refused");
        std::stop_source Stop;
        Stop.request_stop();
        Require(BuildVisualDensity(Fine, ChunkSide, 4, Output, Metrics, Stop.get_token()) == SurfaceStatus::Canceled,
            "cancellation requires fine fallback");
        Require(Metrics.Collapses == PreviousCount, "failed reductions modify previous metrics");
        RequireParity(Previous, Output);
        for (const double Spacing : {0.5, 2.0}) {
            const auto Scale = [Spacing](SurfaceMesh Mesh) {
                Mesh.Spacing = Spacing;
                for (auto &Vertex : Mesh.Vertices) {
                    Vertex.Position.X *= static_cast<float>(Spacing);
                    Vertex.Position.Y *= static_cast<float>(Spacing);
                    Vertex.Position.Z *= static_cast<float>(Spacing);
                }
                return Mesh;
            };
            Require(BuildVisualDensity(Scale(Fine), ChunkSide, 4, Output, Metrics) == SurfaceStatus::Success,
                "supported alternate authored spacing denied");
            RequireParity(Scale(Previous), Output);
        }
        std::cout << "Checks,DenialAndAuthoredSpacing,Pass\n";
    }

    SurfaceMesh MakeOverlapWitness() {
        auto Synthetic = Extract(Scene::Rolling, 0, 1, 1);
        gargantuan::terrain::test::AppendOverlapWitness(Synthetic);
        return Synthetic;
    }

    void CheckOverlapGuard() {
        const auto CheckIncident = [](std::initializer_list<SurfacePoint> Points, std::initializer_list<Index> Indices,
            SurfaceStatus Expected) {
            SurfaceMesh Mesh;
            for (const auto P : Points) Mesh.Vertices.push_back({.Position = P});
            Mesh.Indices = Indices;
            constexpr std::array<std::uint8_t, 2> Changed{1, 0};
            std::size_t Pairs = 0;
            Require(CheckVisualDensityOverlap(Mesh, ChunkSide, Changed, Pairs, {}) == Expected,
                "guard misclassified shared-simplex fixture");
            if (Expected == SurfaceStatus::Success)
                Require(Pairs == 1, "control must test its incident pair");
        };
        constexpr SurfacePoint A{4, 4, 4}, B{6, 4, 4}, C{4, 6, 4};
        CheckIncident({A, B, C, {4, 3, 4}}, {0, 1, 2, 1, 0, 3}, SurfaceStatus::Success);
        CheckIncident({A, B, C, {4, 5, 4}}, {0, 1, 2, 1, 0, 3}, SurfaceStatus::Degenerate);
        CheckIncident({A, B, C, {4, 5, 4.00000048f}}, {0, 1, 2, 1, 0, 3}, SurfaceStatus::Degenerate);
        CheckIncident({A, B, C, {4, 5, 5}}, {0, 1, 2, 1, 0, 3}, SurfaceStatus::Success);
        CheckIncident({A, B, C, {3, 4, 4}, {4, 3, 4}}, {0, 1, 2, 0, 3, 4}, SurfaceStatus::Success);
        CheckIncident({A, B, C, {5, 4.25f, 4}, {4.25f, 5, 4}}, {0, 1, 2, 0, 3, 4}, SurfaceStatus::Degenerate);
        CheckIncident({A, B, C, {5, 4.25f, 3}, {5, 4.25f, 5}}, {0, 1, 2, 0, 3, 4}, SurfaceStatus::Degenerate);
        CheckIncident({A, B, C, {3, 4.25f, 3}, {3, 4.25f, 5}}, {0, 1, 2, 0, 3, 4}, SurfaceStatus::Success);

        SurfaceMesh Broad;
        for (const auto P : {SurfacePoint{0, 0, 0}, {32, 32, 0}, {0, 0, 32}})
            Broad.Vertices.push_back({.Position = P});
        std::vector<std::uint8_t> Changed(129, 1);
        for (std::size_t I = 0; I < Changed.size(); ++I)
            Broad.Indices.insert(Broad.Indices.end(), {0, 1, 2});
        std::size_t Pairs = 0;
        Require(CheckVisualDensityOverlap(Broad, ChunkSide, Changed, Pairs, {}) == SurfaceStatus::WorkLimit,
            "bounded grid admits excessive memberships");

        const auto Fine = std::make_shared<const SurfaceMesh>(MakeOverlapWitness());
        auto Output = Extract(Scene::Flat, 0, 0, 1);
        const auto Previous = Output;
        VisualDensityMetrics Metrics{42, 0.5, 27};
        Require(BuildVisualDensity(*Fine, ChunkSide, 4, Output, Metrics) == SurfaceStatus::Degenerate,
            "detached obstacle should deny reduced output");
        RequireParity(Previous, Output);
        Require(Metrics.Collapses == 42 && Metrics.MaximumDisplacement == 0.5 && Metrics.OverlapPairs == 27,
            "overlap denial overwrote the previous measurement");
        const auto Variant = VisualDensityVariant::Prepare(Fine);
        Require(!Variant && Variant.error() == SurfaceStatus::Degenerate,
            "overlap denial must leave current fine coverage as the only choice");
        std::cout << "Checks,IncidentFoldsAndDetachedWitnessFineFallback,Pass\n";
    }

    void RunOverlapAudit() {
        using gargantuan::terrain::test::AuditNonincidentOverlap;
        SurfaceMesh Control;
        Control.Indices = {0, 1, 2, 3, 4, 5};
        for (const SurfacePoint Point : {SurfacePoint{0, 0, 0}, {2, 0, 0}, {0, 2, 0},
                 {0.5f, 0.5f, -1}, {0.5f, 0.5f, 1}, {1.5f, 0.5f, 0}})
            Control.Vertices.push_back({.Position = Point});
        Require(AuditNonincidentOverlap(Control, 0.0) == 1, "audit did not detect crossing triangles");
        Control.Vertices[3].Position = {1.5f, 1.5f, 0};
        Control.Vertices[4].Position = {1.9f, 1.5f, 0};
        Control.Vertices[5].Position = {1.5f, 1.9f, 0};
        Require(AuditNonincidentOverlap(Control, 0.0) == 0, "audit reported separated coplanar triangles as crossing");
        Control.Vertices[3].Position = {0.5f, 0.5f, 0};
        Require(AuditNonincidentOverlap(Control, 0.0) == 1, "audit missed coplanar overlap");

        std::cout << "Scene,Axis,AuthoredNonIncidentContacts,ReducedNonIncidentContacts\n";
        const auto Examine = [](std::string_view Name, unsigned Axis, const SurfaceMesh &Fine) {
            constexpr double Tolerance = 1.0e-8;
            SurfaceMesh Reduced;
            VisualDensityMetrics Metrics;
            Require(BuildVisualDensity(Fine, ChunkSide, 4, Reduced, Metrics) == SurfaceStatus::Success,
                "audit reduction failed");
            const auto FineContacts = AuditNonincidentOverlap(Fine, Tolerance);
            const auto ReducedContacts = AuditNonincidentOverlap(Reduced, Tolerance);
            std::cout << Name << ',' << Axis << ',' << FineContacts << ',' << ReducedContacts << '\n';
            return std::array{FineContacts, ReducedContacts};
        };
        for (unsigned Axis = 0; Axis < 3; ++Axis)
            for (std::size_t I = 0; I < Names.size(); ++I)
                Require(Examine(Names[I], Axis, Extract(static_cast<Scene>(I), Axis, 1, 1)) == std::array<std::size_t, 2>{},
                    "qualified analytic field gave a nonincident surface contact");

        const auto Synthetic = MakeOverlapWitness();
        const auto Unguarded = ReduceInteriorPasses(Synthetic, 4);
        Require(AuditNonincidentOverlap(Synthetic, 1.0e-8) == 0 &&
            AuditNonincidentOverlap(Unguarded.Mesh, 1.0e-8) == 1, "unguarded synthetic witness contact count");
        std::cout << "SyntheticDetachedTriangleUnchecked,0,0,1\n";
        CheckOverlapGuard();
    }
}

int main(int ArgumentCount, char **Arguments) {
    if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--audit-overlap") {
        Extent = ChunkSide;
        RunOverlapAudit();
        return 0;
    }
    const bool ProfileReference = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--profile-reference";
    const bool IndexedComparison = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--compare-indexed";
    const bool RuntimeDensity = ProfileReference || IndexedComparison ||
        (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--runtime-density");
    if (ArgumentCount > 1 && !RuntimeDensity) return 2;
    if (RuntimeDensity) Extent = ChunkSide;
    std::cout << "Scene,Axis,FineTriangles,RawCoarseTriangles,LockedTriangles,ReferenceSeamEdges,"
                 "RawUnmatchedEdges,LockedUnmatchedEdges,FineComponents,RawComponents,LockedComponents,"
                 "Collapses,MaximumMove,ReducerUs\n";
    for (unsigned Axis = 0; Axis < 3; ++Axis)
        for (std::size_t I = 0; I < Names.size(); ++I)
            Compare(static_cast<Scene>(I), Axis, RuntimeDensity ? 4u : 1u, ProfileReference, IndexedComparison);
    CheckChangedBoundary(IndexedComparison);
    if (RuntimeDensity) CheckCornerAndViewSequence(IndexedComparison);
    if (IndexedComparison) {
        CheckIndexedLimits();
        CheckOverlapGuard();
    }
    std::cout << "Checks,SeamAndTopology,Pass\n";
}
