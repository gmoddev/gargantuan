// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace gargantuan::terrain::test {
    // Test-only triangle separating axes. This deliberately excludes pairs
    // sharing a vertex, including adjacent triangles with legitimate contacts.
    using Index = std::uint32_t;
    using Triangle = std::array<Index, 3>;
    using Vec3 = std::array<double, 3>;

    inline void AppendOverlapWitness(SurfaceMesh &Mesh) {
        // The rolling fixture's contraction reaches this detached triangle.
        const auto First = static_cast<Index>(Mesh.Vertices.size());
        for (const SurfacePoint Point : {SurfacePoint{6.33383608f, 8.96637821f, 1.97645724f},
                 {6.33283043f, 8.97220898f, 1.97745383f}, {6.33333349f, 8.96979904f, 1.97399843f}})
            Mesh.Vertices.push_back({.Position = Point});
        Mesh.Indices.insert(Mesh.Indices.end(), {First, First + 1, First + 2});
    }

    inline Vec3 Sub3(Vec3 A, Vec3 B) {
        return {A[0] - B[0], A[1] - B[1], A[2] - B[2]};
    }

    inline Vec3 Cross3(Vec3 A, Vec3 B) {
        return {A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0]};
    }

    inline double Dot(Vec3 A, Vec3 B) {
        return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
    }

    struct AuditFace {
        Triangle Ids;
        std::array<Vec3, 3> Points;
        std::array<Vec3, 3> Edges;
        Vec3 Normal, Min, Max;
    };

    inline AuditFace ReadAuditFace(const SurfaceMesh &Mesh, std::size_t At) {
        AuditFace F{};
        for (std::size_t I = 0; I < 3; ++I) {
            F.Ids[I] = Mesh.Indices[At + I];
            const auto P = Mesh.Vertices[F.Ids[I]].Position;
            F.Points[I] = {P.X, P.Y, P.Z};
        }
        for (std::size_t I = 0; I < 3; ++I) {
            F.Edges[I] = Sub3(F.Points[(I + 1) % 3], F.Points[I]);
            F.Min[I] = std::min({F.Points[0][I], F.Points[1][I], F.Points[2][I]});
            F.Max[I] = std::max({F.Points[0][I], F.Points[1][I], F.Points[2][I]});
        }
        F.Normal = Cross3(F.Edges[0], F.Edges[1]);
        return F;
    }

    inline bool Overlaps(const AuditFace &A, const AuditFace &B, double Tolerance) {
        const auto Separates = [&](Vec3 Axis) {
            const auto Scale = std::sqrt(Dot(Axis, Axis));
            if (Scale == 0.0) return false;
            const auto Project = [&](const auto &Points) {
                const std::array P{Dot(Axis, Sub3(Points[0], A.Points[0])),
                    Dot(Axis, Sub3(Points[1], A.Points[0])), Dot(Axis, Sub3(Points[2], A.Points[0]))};
                const auto MinMax = std::ranges::minmax(P);
                return std::array{MinMax.min, MinMax.max};
            };
            const auto X = Project(A.Points);
            const auto Y = Project(B.Points);
            return X[1] + Tolerance * Scale < Y[0] || Y[1] + Tolerance * Scale < X[0];
        };
        if (Separates(A.Normal) || Separates(B.Normal)) return false;
        for (const auto &AE : A.Edges) {
            if (Separates(Cross3(A.Normal, AE))) return false;
            for (const auto &BE : B.Edges)
                if (Separates(Cross3(AE, BE))) return false;
        }
        for (const auto &BE : B.Edges)
            if (Separates(Cross3(B.Normal, BE))) return false;
        return true;
    }

    inline std::size_t AuditNonincidentOverlap(const SurfaceMesh &Mesh, double Tolerance) {
        std::vector<AuditFace> Faces;
        Faces.reserve(Mesh.Indices.size() / 3);
        for (std::size_t I = 0; I < Mesh.Indices.size(); I += 3)
            Faces.push_back(ReadAuditFace(Mesh, I));
        std::ranges::sort(Faces, [](const auto &A, const auto &B) { return A.Min[0] < B.Min[0]; });
        std::size_t Count = 0;
        for (std::size_t I = 0; I < Faces.size(); ++I) {
            const auto &A = Faces[I];
            for (std::size_t J = I + 1; J < Faces.size() && Faces[J].Min[0] <= A.Max[0] + Tolerance; ++J) {
                const auto &B = Faces[J];
                if (A.Max[1] + Tolerance < B.Min[1] || B.Max[1] + Tolerance < A.Min[1] ||
                    A.Max[2] + Tolerance < B.Min[2] || B.Max[2] + Tolerance < A.Min[2] ||
                    std::ranges::any_of(A.Ids, [&](auto V) { return std::ranges::find(B.Ids, V) != B.Ids.end(); }))
                    continue;
                Count += Overlaps(A, B, Tolerance);
            }
        }
        return Count;
    }
}
