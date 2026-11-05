// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include <box3d/box3d.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gargantuan::physics_detail {
	inline constexpr std::size_t MaximumBox3DMeshChunkTriangles = 255;

	struct Box3DMeshDeleter {
		void operator()(b3MeshData *Mesh) const noexcept {
			if (Mesh) b3DestroyMesh(Mesh);
		}
	};

	using OwnedBox3DMesh = std::unique_ptr<b3MeshData, Box3DMeshDeleter>;

	struct Box3DMeshChunkSet {
		std::vector<OwnedBox3DMesh> Chunks;
		std::size_t NativeBytes = 0;
		// Counts live b3MeshData blobs only. Builder scratch and C++ arrays are excluded.
		std::size_t PeakStagingNativeBytes = 0;
	};

	namespace Box3DMeshChunkDetail {
		[[nodiscard]] inline std::size_t Align8(std::size_t Value) {
			return (Value + 7) & ~std::size_t{7};
		}

		// Upper bound on the final b3MeshData blob, not on Box3D builder scratch.
		[[nodiscard]] inline std::size_t MeshBytesUpperBound(std::size_t Vertices, std::size_t Triangles) {
			return Align8(sizeof(b3MeshData)) +
				Align8((2 * Triangles - 1) * sizeof(b3MeshNode)) +
				Align8(Vertices * sizeof(b3Vec3)) +
				Align8(Triangles * sizeof(b3MeshTriangle)) +
				2 * Align8(Triangles * sizeof(std::uint8_t));
		}

		[[nodiscard]] inline bool ValidArray(const b3MeshData &Mesh, int Offset, std::size_t Count) {
			return Offset >= static_cast<int>(sizeof(b3MeshData)) && Mesh.byteCount > 0 &&
				static_cast<std::size_t>(Offset) <= static_cast<std::size_t>(Mesh.byteCount) &&
				Count <= static_cast<std::size_t>(Mesh.byteCount) - static_cast<std::size_t>(Offset);
		}
	}

	// The input arrays must remain alive for this call. Admission is conservative:
	// the full globally edge-identified mesh and all completed chunks fit in
	// AvailableNativeBytes at once. The caller owns all other world accounting.
	[[nodiscard]] inline std::expected<Box3DMeshChunkSet, std::string> BuildBox3DMeshChunks(
		std::span<b3Vec3> Vertices, std::span<std::int32_t> Indices,
		std::size_t AvailableNativeBytes
	) {
		try {
			if (Vertices.size() < 3 || Vertices.size() > 262144 || Indices.empty() ||
				Indices.size() > 1048575 || Indices.size() % 3 != 0)
				return std::unexpected("Mesh chunk input exceeds geometry limits");
			const auto TriangleCount = Indices.size() / 3;
			for (const auto &Vertex : Vertices)
				if (!std::isfinite(Vertex.x) || !std::isfinite(Vertex.y) || !std::isfinite(Vertex.z))
					return std::unexpected("Mesh chunk input has non-finite vertices");
			for (std::size_t Triangle = 0; Triangle < TriangleCount; ++Triangle) {
				const auto A = Indices[3 * Triangle], B = Indices[3 * Triangle + 1],
					C = Indices[3 * Triangle + 2];
				if (A < 0 || B < 0 || C < 0 || static_cast<std::size_t>(A) >= Vertices.size() ||
					static_cast<std::size_t>(B) >= Vertices.size() ||
					static_cast<std::size_t>(C) >= Vertices.size() || A == B || A == C || B == C)
					return std::unexpected("Mesh chunk input has invalid triangle indices");
				const auto EdgeA = b3Sub(Vertices[B], Vertices[A]);
				const auto EdgeB = b3Sub(Vertices[C], Vertices[A]);
				const auto Cross = b3Cross(EdgeA, EdgeB);
				const auto AreaSquared = b3Dot(Cross, Cross);
				if (!std::isfinite(AreaSquared) || AreaSquared < 1.0e-10f)
					return std::unexpected("Mesh chunk input contains degenerate triangles");
			}
			const auto GlobalUpperBound = Box3DMeshChunkDetail::MeshBytesUpperBound(
				Vertices.size(), TriangleCount);
			if (GlobalUpperBound > AvailableNativeBytes)
				return std::unexpected("Mesh chunk staging exceeds native-byte budget");
			b3MeshDef GlobalDefinition{};
			GlobalDefinition.vertices = Vertices.data();
			GlobalDefinition.indices = Indices.data();
			GlobalDefinition.vertexCount = static_cast<int>(Vertices.size());
			GlobalDefinition.triangleCount = static_cast<int>(TriangleCount);
			GlobalDefinition.identifyEdges = true;
			OwnedBox3DMesh Global(b3CreateMesh(&GlobalDefinition, nullptr, 0));
			if (!Global || Global->byteCount <= 0 ||
				Global->triangleCount != static_cast<int>(TriangleCount) ||
				static_cast<std::size_t>(Global->byteCount) > GlobalUpperBound)
				return std::unexpected("Box3D rejected or changed the global mesh geometry");
			const auto GlobalBytes = static_cast<std::size_t>(Global->byteCount);
			Box3DMeshChunkSet Result;
			Result.PeakStagingNativeBytes = GlobalBytes;
			if (TriangleCount <= MaximumBox3DMeshChunkTriangles) {
				Result.NativeBytes = GlobalBytes;
				Result.Chunks.push_back(std::move(Global));
				return std::move(Result);
			}
			if (!Box3DMeshChunkDetail::ValidArray(*Global, Global->flagsOffset, TriangleCount) ||
				!Box3DMeshChunkDetail::ValidArray(*Global, Global->triangleOffset,
					TriangleCount * sizeof(b3MeshTriangle)) ||
				!Box3DMeshChunkDetail::ValidArray(*Global, Global->vertexOffset,
					static_cast<std::size_t>(Global->vertexCount) * sizeof(b3Vec3)))
				return std::unexpected("Box3D global mesh arrays are malformed");
			const auto *GlobalVertices = b3GetMeshVertices(Global.get());
			const auto *GlobalTriangles = b3GetMeshTriangles(Global.get());
			const auto *GlobalFlags = b3GetMeshFlags(Global.get());
			const auto ChunkCount = (TriangleCount + MaximumBox3DMeshChunkTriangles - 1) /
				MaximumBox3DMeshChunkTriangles;
			Result.Chunks.reserve(ChunkCount);
			std::size_t StagingBytes = GlobalBytes;
			for (std::size_t Base = 0; Base < TriangleCount; Base += MaximumBox3DMeshChunkTriangles) {
				const auto Count = std::min(MaximumBox3DMeshChunkTriangles, TriangleCount - Base);
				std::vector<b3Vec3> LocalVertices;
				std::vector<std::int32_t> LocalIndices;
				std::vector<std::uint8_t> Ordinals;
				std::unordered_map<std::int32_t, std::int32_t> LocalByGlobal;
				LocalVertices.reserve(3 * Count);
				LocalIndices.reserve(3 * Count);
				Ordinals.reserve(Count);
				LocalByGlobal.reserve(3 * Count);
				for (std::size_t Offset = 0; Offset < Count; ++Offset) {
					const auto &Triangle = GlobalTriangles[Base + Offset];
					for (const auto GlobalIndex : {Triangle.index1, Triangle.index2, Triangle.index3}) {
						if (GlobalIndex < 0 || GlobalIndex >= Global->vertexCount)
							return std::unexpected("Box3D global triangle index is invalid");
						const auto [Found, Inserted] = LocalByGlobal.try_emplace(GlobalIndex,
							static_cast<std::int32_t>(LocalVertices.size()));
						if (Inserted) LocalVertices.push_back(GlobalVertices[GlobalIndex]);
						LocalIndices.push_back(Found->second);
					}
					Ordinals.push_back(static_cast<std::uint8_t>(Offset));
				}
				const auto ChunkUpperBound = Box3DMeshChunkDetail::MeshBytesUpperBound(
					LocalVertices.size(), Count);
				if (ChunkUpperBound > AvailableNativeBytes - StagingBytes)
					return std::unexpected("Mesh chunk staging exceeds native-byte budget");
				b3MeshDef ChunkDefinition{};
				ChunkDefinition.vertices = LocalVertices.data();
				ChunkDefinition.indices = LocalIndices.data();
				ChunkDefinition.materialIndices = Ordinals.data();
				ChunkDefinition.vertexCount = static_cast<int>(LocalVertices.size());
				ChunkDefinition.triangleCount = static_cast<int>(Count);
				ChunkDefinition.identifyEdges = false;
				OwnedBox3DMesh Chunk(b3CreateMesh(&ChunkDefinition, nullptr, 0));
				if (!Chunk || Chunk->byteCount <= 0 || Chunk->triangleCount != static_cast<int>(Count) ||
					static_cast<std::size_t>(Chunk->byteCount) > ChunkUpperBound ||
					!Box3DMeshChunkDetail::ValidArray(*Chunk, Chunk->flagsOffset, Count) ||
					!Box3DMeshChunkDetail::ValidArray(*Chunk, Chunk->materialOffset, Count))
					return std::unexpected("Box3D rejected or changed a mesh chunk");
				auto *Flags = reinterpret_cast<std::uint8_t *>(Chunk.get()) + Chunk->flagsOffset;
				auto *Materials = reinterpret_cast<std::uint8_t *>(Chunk.get()) + Chunk->materialOffset;
				std::array<bool, MaximumBox3DMeshChunkTriangles> Seen{};
				for (std::size_t Output = 0; Output < Count; ++Output) {
					const auto Ordinal = Materials[Output];
					if (Ordinal >= Count || Seen[Ordinal])
						return std::unexpected("Box3D chunk triangle reorder is invalid");
					Seen[Ordinal] = true;
					Flags[Output] = GlobalFlags[Base + Ordinal];
					Materials[Output] = 0;
				}
				Chunk->materialCount = 1;
				Chunk->hash = 0;
				Chunk->hash = b3Hash(B3_HASH_INIT,
					reinterpret_cast<const std::uint8_t *>(Chunk.get()), Chunk->byteCount);
				if (Chunk->hash == 0) Chunk->hash = 1;
				const auto ChunkBytes = static_cast<std::size_t>(Chunk->byteCount);
				StagingBytes += ChunkBytes;
				Result.PeakStagingNativeBytes = std::max(Result.PeakStagingNativeBytes, StagingBytes);
				Result.NativeBytes += ChunkBytes;
				Result.Chunks.push_back(std::move(Chunk));
			}
			return std::move(Result);
		} catch (const std::bad_alloc &) {
			return std::unexpected("GeometryMemory");
		}
	}
}
