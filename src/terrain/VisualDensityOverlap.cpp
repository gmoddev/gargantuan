// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/VisualDensityOverlap.hpp"

#include "gargantuan/Profiler.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace gargantuan::terrain {
    namespace {
        using Point = std::array<double, 3>;
        using Cells3 = std::array<unsigned, 3>;
        using FaceIndex = std::uint32_t;
        constexpr double DistanceTolerance = 1.0e-8;
        constexpr double AngularTolerance = 1.0e-7;
        constexpr double PlaneToleranceSquared = 1.0e-12;
        constexpr unsigned BinWidth = 2;
        constexpr std::size_t MaximumBinMemberships = 512 * 1024;
        constexpr std::size_t MaximumPairVisits = 1'000'000;

        Point Sub(Point A, Point B) noexcept {
            return {A[0] - B[0], A[1] - B[1], A[2] - B[2]};
        }

        Point Scale(Point A, double Factor) noexcept {
            return {A[0] * Factor, A[1] * Factor, A[2] * Factor};
        }

        double Dot(Point A, Point B) noexcept {
            return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
        }

        Point Cross(Point A, Point B) noexcept {
            return {A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0]};
        }

        Point Unit(Point Direction) noexcept {
            const auto Length = std::sqrt(Dot(Direction, Direction));
            return Length == 0 ? Point{} : Scale(Direction, 1.0 / Length);
        }

        struct Face {
            std::array<FaceIndex, 3> Vertices;
            Point Low, High;
            Point Normal;
            Cells3 First, Last;
        };

        // Bin offsets are built in counting order and used in identical order
        // for queries. Inclusive expanded bounds include bin-plane contacts.
        template <typename Visit>
        void ForBins(const Face &Value, unsigned Side, Visit Function) {
            for (auto Z = Value.First[2]; Z <= Value.Last[2]; ++Z)
                for (auto Y = Value.First[1]; Y <= Value.Last[1]; ++Y)
                    for (auto X = Value.First[0]; X <= Value.Last[0]; ++X)
                        if (!Function((Z * Side + Y) * Side + X)) return;
        }

        std::array<Point, 3> GetPoints(const Face &Value, std::span<const Point> Points) noexcept {
            return {Points[Value.Vertices[0]], Points[Value.Vertices[1]], Points[Value.Vertices[2]]};
        }

        Point InnerDirection(const Face &Value, std::span<const Point> Points, FaceIndex A, FaceIndex B) noexcept {
            for (const auto V : Value.Vertices)
                if (V != A && V != B) return Sub(Points[V], Points[A]);
            return {};
        }

        bool SameEdgeIsSafe(const Face &A, const Face &B, std::span<const Point> Points,
            FaceIndex First, FaceIndex Second, Point Line) noexcept {
            if (Dot(Line, Line) > PlaneToleranceSquared)
                return true; // The two face planes intersect in their common edge.

            const auto Edge = Unit(Sub(Points[Second], Points[First]));
            const auto Left = Unit(Cross(Edge, InnerDirection(A, Points, First, Second)));
            const auto Right = Unit(Cross(Edge, InnerDirection(B, Points, First, Second)));
            return Dot(Left, Right) < -0.99; // Opposite sides of the common supporting edge.
        }

        std::array<Point, 2> GetRays(const Face &Value, std::span<const Point> Points, FaceIndex Shared) noexcept {
            std::size_t I = 0;
            while (Value.Vertices[I] != Shared) ++I;
            return {Unit(Sub(Points[Value.Vertices[(I + 1) % 3]], Points[Shared])),
                Unit(Sub(Points[Value.Vertices[(I + 2) % 3]], Points[Shared]))};
        }

        bool InWedge(const std::array<Point, 2> &Rays, Point Normal, Point Direction) noexcept {
            return Dot(Cross(Rays[0], Direction), Normal) >= -AngularTolerance &&
                Dot(Cross(Direction, Rays[1]), Normal) >= -AngularTolerance;
        }

        bool SeparatesWedges(const std::array<Point, 2> &A, Point Normal, const std::array<Point, 2> &B) noexcept {
            for (unsigned I = 0; I < 2; ++I) {
                auto Axis = Unit(Cross(Normal, A[I]));
                const auto Own = Dot(Axis, A[1 - I]);
                if (std::abs(Own) <= AngularTolerance) continue;
                if (Own < 0) Axis = Scale(Axis, -1);
                if (Dot(Axis, B[0]) < -AngularTolerance && Dot(Axis, B[1]) < -AngularTolerance)
                    return true;
            }
            return false;
        }

        bool SameVertexIsSafe(const Face &A, const Face &B, std::span<const Point> Points,
            FaceIndex Shared, Point Line) noexcept {
            const auto AEdges = GetRays(A, Points, Shared);
            const auto BEdges = GetRays(B, Points, Shared);
            if (Dot(Line, Line) <= PlaneToleranceSquared) {
                // A separating plane through the shared vertex can prove
                // that even nearly coplanar wedges only meet at the vertex.
                for (const auto U : AEdges)
                    for (const auto V : BEdges) {
                        const auto Axis = Sub(U, V);
                        if (Dot(Axis, AEdges[0]) > AngularTolerance && Dot(Axis, AEdges[1]) > AngularTolerance &&
                            Dot(Axis, BEdges[0]) < -AngularTolerance && Dot(Axis, BEdges[1]) < -AngularTolerance)
                            return true;
                    }
                return SeparatesWedges(AEdges, A.Normal, BEdges) || SeparatesWedges(BEdges, B.Normal, AEdges);
            }
            const auto Forward = Unit(Line);
            const auto Reverse = Scale(Forward, -1);
            // For distinct planes, an intersection outside the common
            // vertex must follow one of the two plane-intersection rays.
            return !(InWedge(AEdges, A.Normal, Forward) && InWedge(BEdges, B.Normal, Forward)) &&
                !(InWedge(AEdges, A.Normal, Reverse) && InWedge(BEdges, B.Normal, Reverse));
        }

        bool DisjointFaces(const Face &A, const Face &B, std::span<const Point> Points, Point PlaneLine) noexcept {
            auto Left = GetPoints(A, Points);
            auto Right = GetPoints(B, Points);
            const auto Origin = Left[0];
            for (auto &P : Left) P = Sub(P, Origin);
            for (auto &P : Right) P = Sub(P, Origin);
            const auto Separates = [&Left, &Right](Point Axis) {
                const auto LengthSquared = Dot(Axis, Axis);
                if (LengthSquared == 0) return false;
                const auto Project = [Axis](const auto &P) {
                    const auto Q0 = Dot(Axis, P[0]), Q1 = Dot(Axis, P[1]), Q2 = Dot(Axis, P[2]);
                    return std::array{std::min({Q0, Q1, Q2}), std::max({Q0, Q1, Q2})};
                };
                const auto X = Project(Left);
                const auto Y = Project(Right);
                const auto Gap = std::max(X[0] - Y[1], Y[0] - X[1]);
                return Gap > 0 && Gap * Gap > DistanceTolerance * DistanceTolerance * LengthSquared;
            };
            if (Separates(A.Normal) || Separates(B.Normal)) return true;
            const auto Edges = [](const auto &P) {
                return std::array{Sub(P[1], P[0]), Sub(P[2], P[1]), Sub(P[0], P[2])};
            };
            const auto AEdges = Edges(Left);
            const auto BEdges = Edges(Right);
            if (Dot(PlaneLine, PlaneLine) <= PlaneToleranceSquared) {
                for (const auto E : AEdges) if (Separates(Cross(A.Normal, E))) return true;
                for (const auto E : BEdges) if (Separates(Cross(B.Normal, E))) return true;
            }
            for (const auto L : AEdges)
                for (const auto R : BEdges)
                    if (Separates(Cross(L, R))) return true;
            return false;
        }

        bool HasOnlyExpectedContact(const Face &A, const Face &B, std::span<const Point> Points) noexcept {
            if (Dot(A.Normal, A.Normal) == 0 || Dot(B.Normal, B.Normal) == 0)
                return false;
            std::array<FaceIndex, 3> Shared{};
            unsigned Count = 0;
            for (const auto V : A.Vertices)
                if (std::ranges::find(B.Vertices, V) != B.Vertices.end())
                    Shared[Count++] = V;
            const auto Line = Cross(A.Normal, B.Normal);
            switch (Count) {
                case 0: return DisjointFaces(A, B, Points, Line);
                case 1: return SameVertexIsSafe(A, B, Points, Shared[0], Line);
                case 2: return SameEdgeIsSafe(A, B, Points, Shared[0], Shared[1], Line);
                default: return false;
            }
        }

        bool BoundsOverlap(const Face &A, const Face &B) noexcept {
            for (unsigned Axis = 0; Axis < 3; ++Axis)
                if (A.High[Axis] + DistanceTolerance < B.Low[Axis] || B.High[Axis] + DistanceTolerance < A.Low[Axis])
                    return false;
            return true;
        }
    }

    SurfaceStatus CheckVisualDensityOverlap(const SurfaceMesh &Mesh, unsigned Cells,
        std::span<const std::uint8_t> Changed, std::size_t &TestedPairs, std::stop_token Stop) {
        G_PROFILE("Terrain::VisualDensityOverlap");
        if (Stop.stop_requested()) return SurfaceStatus::Canceled;
        if (Cells == 0 || Cells > ChunkSide || Mesh.Indices.size() % 3 != 0 ||
            Changed.size() != Mesh.Indices.size() / 3 ||
            (Mesh.Spacing != 0.5 && Mesh.Spacing != 1.0 && Mesh.Spacing != 2.0))
            return SurfaceStatus::InvalidInput;
        if (Mesh.Indices.size() > MaximumVisualDensityIndices || Mesh.Vertices.size() > MaximumVisualDensityVertices)
            return SurfaceStatus::WorkLimit;
        const auto BinSide = (Cells + BinWidth - 1) / BinWidth;
        const auto BinCount = BinSide * BinSide * BinSide;
        std::vector<Point> Points;
        Points.reserve(Mesh.Vertices.size());
        for (const auto &V : Mesh.Vertices) {
            const auto P = Scale({V.Position.X, V.Position.Y, V.Position.Z}, 1.0 / Mesh.Spacing);
            if (!std::ranges::all_of(P, [Cells](double Axis) { return Axis >= 0 && Axis <= Cells; }))
                return SurfaceStatus::InvalidInput;
            Points.push_back(P);
        }

        std::vector<Face> Faces;
        Faces.reserve(Changed.size());
        std::vector<std::uint32_t> Offsets(BinCount + 1);
        std::size_t Memberships = 0;
        const auto Bin = [BinSide](double P) {
            return std::min(static_cast<unsigned>(std::max(P, 0.0) / BinWidth), BinSide - 1);
        };
        for (std::size_t I = 0; I < Mesh.Indices.size(); I += 3) {
            if ((I & 511u) == 0 && Stop.stop_requested()) return SurfaceStatus::Canceled;
            Face F{.Vertices = {Mesh.Indices[I], Mesh.Indices[I + 1], Mesh.Indices[I + 2]}};
            if (std::ranges::any_of(F.Vertices, [&](auto V) { return V >= Points.size(); }))
                return SurfaceStatus::InvalidInput;
            const auto P = GetPoints(F, Points);
            const auto U = Sub(P[1], P[0]);
            const auto V = Sub(P[2], P[0]);
            const auto N = Cross(U, V);
            if (Dot(N, N) > PlaneToleranceSquared * Dot(U, U) * Dot(V, V))
                F.Normal = Unit(N);
            if (Changed[I / 3] && Dot(F.Normal, F.Normal) == 0) return SurfaceStatus::Degenerate;
            for (unsigned Axis = 0; Axis < 3; ++Axis) {
                F.Low[Axis] = std::min({P[0][Axis], P[1][Axis], P[2][Axis]});
                F.High[Axis] = std::max({P[0][Axis], P[1][Axis], P[2][Axis]});
                F.First[Axis] = Bin(F.Low[Axis] - DistanceTolerance);
                F.Last[Axis] = Bin(F.High[Axis] + DistanceTolerance);
            }
            const auto Count = (F.Last[0] - F.First[0] + 1) * (F.Last[1] - F.First[1] + 1) *
                (F.Last[2] - F.First[2] + 1);
            if (Count > MaximumBinMemberships - Memberships) return SurfaceStatus::WorkLimit;
            Memberships += Count;
            ForBins(F, BinSide, [&](unsigned J) { ++Offsets[J + 1]; return true; });
            Faces.push_back(F);
        }
        std::partial_sum(Offsets.begin(), Offsets.end(), Offsets.begin());
        auto Cursors = Offsets;
        std::vector<FaceIndex> Rows(Memberships);
        for (FaceIndex I = 0; I < Faces.size(); ++I)
            ForBins(Faces[I], BinSide, [&](unsigned J) { Rows[Cursors[J]++] = I; return true; });

        std::vector<FaceIndex> Visited(Faces.size());
        std::size_t Visits = 0;
        std::size_t Pairs = 0;
        for (FaceIndex I = 0; I < Faces.size(); ++I) {
            if (!Changed[I]) continue;
            auto Status = SurfaceStatus::Success;
            ForBins(Faces[I], BinSide, [&](unsigned BinIndex) {
                for (auto At = Offsets[BinIndex]; At < Offsets[BinIndex + 1]; ++At) {
                    if (++Visits > MaximumPairVisits) Status = SurfaceStatus::WorkLimit;
                    if ((Visits & 511u) == 0 && Stop.stop_requested()) Status = SurfaceStatus::Canceled;
                    if (Status != SurfaceStatus::Success) return false;
                    const auto Other = Rows[At];
                    if (Other == I || (Other < I && Changed[Other]) || Visited[Other] == I + 1)
                        continue;
                    Visited[Other] = I + 1;
                    if (!BoundsOverlap(Faces[I], Faces[Other])) continue;
                    ++Pairs;
                    if (!HasOnlyExpectedContact(Faces[I], Faces[Other], Points)) {
                        Status = SurfaceStatus::Degenerate;
                        return false;
                    }
                }
                return true;
            });
            if (Status != SurfaceStatus::Success) return Status;
        }
        if (Stop.stop_requested()) return SurfaceStatus::Canceled;
        TestedPairs = Pairs;
        return SurfaceStatus::Success;
    }
}
