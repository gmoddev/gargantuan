// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/VisualDensity.hpp"
#include "terrain/VisualDensityOverlap.hpp"

#include "gargantuan/Profiler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <ranges>

namespace gargantuan::terrain {
    namespace {
        using Index = std::uint32_t;
        using Edge = std::array<Index, 2>;
        using Triangle = std::array<Index, 3>;
        using DVec = std::array<double, 3>;

        constexpr std::size_t MaximumAttempts = 4096;
        constexpr unsigned MaximumPasses = 4;

        Edge GetEdge(Index A, Index B) noexcept {
            return {std::min(A, B), std::max(A, B)};
        }

        Triangle GetFace(const SurfaceMesh &Mesh, Index Face) noexcept {
            const auto Start = static_cast<std::size_t>(Face) * 3;
            return {Mesh.Indices[Start], Mesh.Indices[Start + 1], Mesh.Indices[Start + 2]};
        }

        struct EdgeUse {
            Index Other;
            Index Face;
            auto operator<=>(const EdgeUse &) const = default;
        };

        template <typename Value>
        class Rows {
        public:
            explicit Rows(std::size_t Count) : Offsets(Count + 1, 0) {}
            void Count(Index Row) { ++Offsets[Row + 1u]; }
            std::vector<Index> Allocate() {
                std::partial_sum(Offsets.begin(), Offsets.end(), Offsets.begin());
                Values.resize(Offsets.back());
                return Offsets;
            }
            void Put(std::span<Index> Cursors, Index Row, Value Entry) { Values[Cursors[Row]++] = Entry; }
            [[nodiscard]] std::size_t CountRows() const noexcept { return Offsets.size() - 1; }
            [[nodiscard]] std::size_t CountValues() const noexcept { return Values.size(); }
            std::span<Value> Get(Index Row) {
                return std::span(Values).subspan(Offsets[Row], Offsets[Row + 1u] - Offsets[Row]);
            }
            std::span<const Value> Get(Index Row) const {
                return std::span(Values).subspan(Offsets[Row], Offsets[Row + 1u] - Offsets[Row]);
            }

        private:
            std::vector<Index> Offsets;
            std::vector<Value> Values;
        };

        struct Connectivity {
            Rows<Index> Fans;
            Rows<Index> Neighbors;
            Rows<EdgeUse> Edges;

            explicit Connectivity(std::size_t VertexCount) : Fans(VertexCount), Neighbors(VertexCount), Edges(VertexCount) {}

            template <typename Visit>
            void ForEdges(Visit Function) const {
                for (Index Vertex = 0; Vertex < Edges.CountRows(); ++Vertex) {
                    const auto Row = Edges.Get(Vertex);
                    for (std::size_t At = 0; At < Row.size();) {
                        const auto Neighbor = Row[At].Other;
                        auto End = At + 1;
                        while (End < Row.size() && Row[End].Other == Neighbor)
                            ++End;
                        Function(Edge{Vertex, Neighbor}, Row.subspan(At, End - At));
                        At = End;
                    }
                }
            }

            SurfaceStatus Build(const SurfaceMesh &Mesh, std::span<std::uint8_t> Blocked, std::stop_token Stop) {
                G_PROFILE("Terrain::VisualDensityConnectivity");
                const auto FaceCount = static_cast<Index>(Mesh.Indices.size() / 3);
                for (Index I = 0; I < FaceCount; ++I) {
                    if ((I & 255u) == 0 && Stop.stop_requested())
                        return SurfaceStatus::Canceled;
                    const auto Face = GetFace(Mesh, I);
                    if (Face[0] == Face[1] || Face[0] == Face[2] || Face[1] == Face[2] ||
                        std::ranges::any_of(Face, [&](Index Vertex) { return Vertex >= Mesh.Vertices.size(); }))
                        return SurfaceStatus::InvalidInput;
                    for (unsigned J = 0; J < 3; ++J) {
                        Fans.Count(Face[J]);
                        Edges.Count(std::min(Face[J], Face[(J + 1) % 3]));
                    }
                }
                {
                    auto FanCursors = Fans.Allocate();
                    auto EdgeCursors = Edges.Allocate();
                    for (Index I = 0; I < FaceCount; ++I) {
                        const auto Face = GetFace(Mesh, I);
                        for (unsigned J = 0; J < 3; ++J) {
                            const auto Pair = GetEdge(Face[J], Face[(J + 1) % 3]);
                            Fans.Put(FanCursors, Face[J], I);
                            Edges.Put(EdgeCursors, Pair[0], {Pair[1], I});
                        }
                    }
                }
                for (Index I = 0; I < Edges.CountRows(); ++I)
                    std::ranges::sort(Edges.Get(I));
                bool Manifold = true;
                ForEdges([&](Edge Pair, std::span<const EdgeUse> Uses) {
                    Neighbors.Count(Pair[0]);
                    Neighbors.Count(Pair[1]);
                    if (Uses.size() == 1) {
                        Blocked[Pair[0]] = true;
                        Blocked[Pair[1]] = true;
                    } else if (Uses.size() != 2) {
                        Manifold = false;
                    }
                });
                if (!Manifold)
                    return SurfaceStatus::Degenerate;
                if (Stop.stop_requested())
                    return SurfaceStatus::Canceled;

                auto Cursors = Neighbors.Allocate();
                ForEdges([&](Edge Pair, std::span<const EdgeUse>) {
                    // Global pair order visits a vertex's smaller neighbors
                    // first in order, then its larger neighbors in order.
                    Neighbors.Put(Cursors, Pair[0], Pair[1]);
                    Neighbors.Put(Cursors, Pair[1], Pair[0]);
                });
                return SurfaceStatus::Success;
            }
        };

        DVec Sub(SurfacePoint A, SurfacePoint B) noexcept {
            return {static_cast<double>(A.X) - B.X, static_cast<double>(A.Y) - B.Y, static_cast<double>(A.Z) - B.Z};
        }

        double Dot(DVec A, DVec B) noexcept {
            return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
        }

        DVec FaceNormal(const SurfaceMesh &Mesh, Triangle Face) noexcept {
            const auto A = Sub(Mesh.Vertices[Face[1]].Position, Mesh.Vertices[Face[0]].Position);
            const auto B = Sub(Mesh.Vertices[Face[2]].Position, Mesh.Vertices[Face[0]].Position);
            return {A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0]};
        }

        bool Contains(Triangle Face, Index Vertex) noexcept {
            return Face[0] == Vertex || Face[1] == Vertex || Face[2] == Vertex;
        }

        bool HasLinkEdge(const SurfaceMesh &Mesh, std::span<const Index> Fan, Edge Pair) {
            return std::ranges::any_of(Fan, [&](Index FaceId) {
                const auto Face = GetFace(Mesh, FaceId);
                return Contains(Face, Pair[0]) && Contains(Face, Pair[1]);
            });
        }

        bool LinkCondition(const SurfaceMesh &Mesh, const Connectivity &Connections, Edge Pair,
            std::span<const EdgeUse> Uses) {
            if (Uses.size() != 2)
                return false;
            const auto Left = Connections.Neighbors.Get(Pair[0]);
            const auto Right = Connections.Neighbors.Get(Pair[1]);
            Edge Common{};
            unsigned Count = 0;
            auto A = Left.begin();
            auto B = Right.begin();
            while (A != Left.end() && B != Right.end()) {
                if (*A < *B) {
                    ++A;
                } else if (*B < *A) {
                    ++B;
                } else {
                    if (Count == Common.size())
                        return false;
                    Common[Count++] = *A++;
                    ++B;
                }
            }
            if (Count != Common.size())
                return false;
            const auto First = GetFace(Mesh, Uses[0].Face);
            const auto Second = GetFace(Mesh, Uses[1].Face);
            if (!(Contains(First, Common[0]) && Contains(Second, Common[1])) &&
                !(Contains(First, Common[1]) && Contains(Second, Common[0])))
                return false;

            return !(HasLinkEdge(Mesh, Connections.Fans.Get(Pair[0]), Common) &&
                HasLinkEdge(Mesh, Connections.Fans.Get(Pair[1]), Common));
        }

        bool ValidContraction(const SurfaceMesh &Mesh, const Connectivity &Connections, Edge Pair,
            std::span<const std::uint8_t> Blocked, double AreaFloor) {
            const auto Material = Mesh.Vertices[Pair[1]].Material;
            const auto Safe = [&](Index Vertex) { return Blocked[Vertex] == 0 && Mesh.Vertices[Vertex].Material == Material; };
            for (const auto Center : Pair)
                if (!Safe(Center) || !std::ranges::all_of(Connections.Neighbors.Get(Center), Safe))
                    return false;
            const auto A = Mesh.Vertices[Pair[0]].Normal;
            const auto B = Mesh.Vertices[Pair[1]].Normal;
            if (Dot({A.X, A.Y, A.Z}, {B.X, B.Y, B.Z}) < 0.975)
                return false;

            for (const auto FaceId : Connections.Fans.Get(Pair[0])) {
                const auto Face = GetFace(Mesh, FaceId);
                if (Contains(Face, Pair[1]))
                    continue;
                auto Replacement = Face;
                std::ranges::replace(Replacement, Pair[0], Pair[1]);
                const auto Before = FaceNormal(Mesh, Face);
                const auto After = FaceNormal(Mesh, Replacement);
                const auto Area = Dot(After, After);
                if (Area < AreaFloor || Dot(Before, After) < 0.95 * std::sqrt(Area * Dot(Before, Before)))
                    return false;
            }
            return true;
        }

        struct PassResult {
            SurfaceMesh Mesh;
            std::vector<double> Displacement;
            std::vector<std::uint8_t> ChangedFaces;
            VisualDensityMetrics Metrics;
        };

        SurfaceStatus RunPass(const SurfaceMesh &Input, unsigned Cells, const PassResult &Previous,
            PassResult &Result, std::stop_token Stop) {
            auto Blocked = std::vector<std::uint8_t>(Input.Vertices.size());
            const auto Spacing = Input.Spacing;
            const auto Extent = Cells * Spacing;
            const auto Near = [Spacing, Extent](float V) { return V <= Spacing || V >= Extent - Spacing; };
            for (std::size_t I = 0; I < Input.Vertices.size(); ++I) {
                const auto P = Input.Vertices[I].Position;
                Blocked[I] = Near(P.X) || Near(P.Y) || Near(P.Z);
            }
            Connectivity Connections(Input.Vertices.size());
            const auto Status = Connections.Build(Input, Blocked, Stop);
            if (Status != SurfaceStatus::Success)
                return Status;

            const auto MaximumMove = MaximumVisualDensityMoveInSamples * Spacing;
            const auto AreaFloor = 1.0e-12 * Spacing * Spacing * Spacing * Spacing;
            std::vector<std::pair<double, Edge>> Candidates;
            Candidates.reserve(Connections.Edges.CountValues() / 2);
            Connections.ForEdges([&](Edge Pair, std::span<const EdgeUse> Uses) {
                if (Blocked[Pair[0]] || Blocked[Pair[1]])
                    return;
                const auto Delta = Sub(Input.Vertices[Pair[0]].Position, Input.Vertices[Pair[1]].Position);
                const auto LengthSquared = Dot(Delta, Delta);
                if (LengthSquared <= MaximumMove * MaximumMove && LinkCondition(Input, Connections, Pair, Uses))
                    Candidates.emplace_back(LengthSquared, Pair);
            });
            std::ranges::sort(Candidates);
            if (Stop.stop_requested())
                return SurfaceStatus::Canceled;

            std::vector<Index> Contraction(Input.Vertices.size());
            std::iota(Contraction.begin(), Contraction.end(), 0u);
            std::vector<double> Bounds(Input.Vertices.size(), 0.0);
            if (!Previous.Displacement.empty())
                std::ranges::copy(Previous.Displacement, Bounds.begin());
            for (const auto &[DistanceSquared, Pair] : Candidates | std::views::take(MaximumAttempts)) {
                const auto Consequence = Bounds[Pair[0]] + std::sqrt(DistanceSquared);
                if (Consequence > MaximumMove || !ValidContraction(Input, Connections, Pair, Blocked, AreaFloor))
                    continue;
                for (const auto Vertex : Pair) {
                    Blocked[Vertex] = true;
                    for (const auto Neighbor : Connections.Neighbors.Get(Vertex))
                        Blocked[Neighbor] = true;
                }
                Contraction[Pair[0]] = Pair[1];
                Bounds[Pair[1]] = std::max(Bounds[Pair[1]], Consequence);
                ++Result.Metrics.Collapses;
            }

            constexpr auto Unassigned = std::numeric_limits<Index>::max();
            std::vector<Index> Remap(Input.Vertices.size(), Unassigned);
            Result.Mesh.Spacing = Spacing;
            Result.Mesh.Vertices.reserve(Input.Vertices.size() - Result.Metrics.Collapses);
            Result.Mesh.Indices.reserve(Input.Indices.size() - 6 * Result.Metrics.Collapses);
            Result.Displacement.reserve(Input.Vertices.size() - Result.Metrics.Collapses);
            Result.ChangedFaces.reserve((Input.Indices.size() - 6 * Result.Metrics.Collapses) / 3);
            const auto FaceCount = static_cast<Index>(Input.Indices.size() / 3);
            for (Index I = 0; I < FaceCount; ++I) {
                const auto SourceFace = GetFace(Input, I);
                auto Face = SourceFace;
                for (auto &Vertex : Face)
                    Vertex = Contraction[Vertex];
                if (Face[0] == Face[1] || Face[0] == Face[2] || Face[1] == Face[2])
                    continue;
                // Renumbering after compaction preserves provenance.
                // A substituted endpoint propagates to every later pass.
                Result.ChangedFaces.push_back(Face != SourceFace ||
                    (!Previous.ChangedFaces.empty() && Previous.ChangedFaces[I]));
                for (const auto Vertex : Face) {
                    if (Remap[Vertex] == Unassigned) {
                        Remap[Vertex] = static_cast<Index>(Result.Mesh.Vertices.size());
                        Result.Mesh.Vertices.push_back(Input.Vertices[Vertex]);
                        Result.Displacement.push_back(Bounds[Vertex]);
                        Result.Metrics.MaximumDisplacement = std::max(Result.Metrics.MaximumDisplacement, Bounds[Vertex]);
                    }
                    Result.Mesh.Indices.push_back(Remap[Vertex]);
                }
            }
            return Stop.stop_requested() ? SurfaceStatus::Canceled : SurfaceStatus::Success;
        }

        bool Finite(SurfacePoint P) noexcept {
            return std::isfinite(P.X) && std::isfinite(P.Y) && std::isfinite(P.Z);
        }
    }

    SurfaceStatus BuildVisualDensity(const SurfaceMesh &Source, unsigned CellsPerAxis, unsigned Passes,
        SurfaceMesh &Output, VisualDensityMetrics &Metrics, std::stop_token Stop) {
        G_PROFILE("Terrain::BuildVisualDensity");
        if (Stop.stop_requested())
            return SurfaceStatus::Canceled;
        if (CellsPerAxis == 0 || CellsPerAxis > ChunkSide || Passes == 0 || Passes > MaximumPasses ||
            (Source.Spacing != 0.5 && Source.Spacing != 1.0 && Source.Spacing != 2.0) || Source.Indices.size() % 3 != 0)
            return SurfaceStatus::InvalidInput;
        if (Source.Vertices.size() > MaximumVisualDensityVertices || Source.Indices.size() > MaximumVisualDensityIndices)
            return SurfaceStatus::WorkLimit;
        if (!std::ranges::all_of(Source.Vertices, [](const auto &Vertex) {
            return Finite(Vertex.Position) && Finite(Vertex.Normal);
        }))
            return SurfaceStatus::InvalidInput;
        try {
            PassResult Current;
            for (unsigned Pass = 0; Pass < Passes; ++Pass) {
                PassResult Candidate;
                const auto &Input = Pass == 0 ? Source : Current.Mesh;
                const auto Status = RunPass(Input, CellsPerAxis, Current, Candidate, Stop);
                if (Status != SurfaceStatus::Success)
                    return Status;
                if (Pass != 0 && Candidate.Metrics.Collapses == 0)
                    break;
                Candidate.Metrics.Collapses += Current.Metrics.Collapses;
                const bool Changed = Candidate.Metrics.Collapses != Current.Metrics.Collapses;
                Current = std::move(Candidate);
                if (!Changed)
                    break;
            }
            if (Current.Metrics.Collapses) {
                const auto Status = CheckVisualDensityOverlap(Current.Mesh, CellsPerAxis, Current.ChangedFaces,
                    Current.Metrics.OverlapPairs, Stop);
                if (Status != SurfaceStatus::Success)
                    return Status;
            }
            Metrics = Current.Metrics;
            Output = std::move(Current.Mesh);
            return SurfaceStatus::Success;
        } catch (const std::bad_alloc &) {
            return SurfaceStatus::AllocationFailure;
        }
    }

    std::expected<VisualDensityVariant, SurfaceStatus> VisualDensityVariant::Prepare(
        const std::shared_ptr<const SurfaceMesh> &Fine, std::stop_token Stop) {
        if (!Fine)
            return std::unexpected(SurfaceStatus::InvalidInput);
        SurfaceMesh Output;
        VisualDensityMetrics Metrics;
        const auto Status = BuildVisualDensity(*Fine, ChunkSide, MaximumPasses, Output, Metrics, Stop);
        if (Status != SurfaceStatus::Success)
            return std::unexpected(Status);
        if (Metrics.Collapses == 0)
            return VisualDensityVariant(Fine, {});
        try {
            return VisualDensityVariant(Fine, std::make_shared<const SurfaceMesh>(std::move(Output)));
        } catch (const std::bad_alloc &) {
            return std::unexpected(SurfaceStatus::AllocationFailure);
        }
    }

    std::shared_ptr<const SurfaceMesh> VisualDensityVariant::Select(
        const std::shared_ptr<const SurfaceMesh> &CurrentFine, bool PreferReduced) const noexcept {
        if (PreferReduced && Reduced) {
            const auto Original = Source.lock();
            if (Original && Original.get() == CurrentFine.get() && !Original.owner_before(CurrentFine) &&
                !CurrentFine.owner_before(Original))
                return Reduced;
        }
        return CurrentFine;
    }

    std::size_t VisualDensityVariant::GetOwnedBytes() const noexcept {
        return Reduced ? sizeof(SurfaceMesh) + Reduced->Vertices.capacity() * sizeof(SurfaceVertex) +
            Reduced->Indices.capacity() * sizeof(std::uint32_t) : 0;
    }
}
