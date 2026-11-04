#include "../src/physics/Box3DMeshChunks.hpp"

#include <box3d/box3d.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

using namespace gargantuan::physics_detail;

namespace {
	int Failures = 0;
	void Check(bool Condition, const char *Message) {
		if (!Condition) {
			std::cerr << "[Geometry:MeshChunkTest] " << Message << '\n';
			++Failures;
		}
	}

	struct InputMesh {
		std::vector<b3Vec3> Vertices;
		std::vector<std::int32_t> Indices;
	};

	InputMesh Grid(int Width, int Height) {
		InputMesh Mesh;
		for (int Z = 0; Z <= Height; ++Z)
			for (int X = 0; X <= Width; ++X)
				Mesh.Vertices.push_back({float(X), 0.0f, float(Z)});
		for (int Z = 0; Z < Height; ++Z) {
			for (int X = 0; X < Width; ++X) {
				const auto A = static_cast<std::int32_t>(Z * (Width + 1) + X);
				const auto B = A + 1;
				const auto C = A + Width + 1;
				const auto D = C + 1;
				Mesh.Indices.insert(Mesh.Indices.end(), {A, C, B, B, C, D});
			}
		}
		return Mesh;
	}

	InputMesh DisconnectedTriangles(int Count) {
		InputMesh Mesh;
		for (int Index = 0; Index < Count; ++Index) {
			const auto Base = static_cast<std::int32_t>(Mesh.Vertices.size());
			const auto X = float(Index * 4);
			Mesh.Vertices.insert(Mesh.Vertices.end(), {
				{X, 0, 0}, {X + 1, 0, 0}, {X, 0, 1}});
			Mesh.Indices.insert(Mesh.Indices.end(), {Base, Base + 2, Base + 1});
		}
		return Mesh;
	}

	bool SameVertex(b3Vec3 A, b3Vec3 B) {
		return A.x == B.x && A.y == B.y && A.z == B.z;
	}

	bool SameTriangle(const b3MeshData &A, int AIndex, const b3MeshData &B, int BIndex) {
		const auto &ATriangle = b3GetMeshTriangles(&A)[AIndex];
		const auto &BTriangle = b3GetMeshTriangles(&B)[BIndex];
		const auto *AVertices = b3GetMeshVertices(&A);
		const auto *BVertices = b3GetMeshVertices(&B);
		return SameVertex(AVertices[ATriangle.index1], BVertices[BTriangle.index1]) &&
			SameVertex(AVertices[ATriangle.index2], BVertices[BTriangle.index2]) &&
			SameVertex(AVertices[ATriangle.index3], BVertices[BTriangle.index3]);
	}

	void CheckFlagsAndCoverage(InputMesh &Input, const Box3DMeshChunkSet &Set) {
		b3MeshDef Definition{};
		Definition.vertices = Input.Vertices.data();
		Definition.indices = Input.Indices.data();
		Definition.vertexCount = static_cast<int>(Input.Vertices.size());
		Definition.triangleCount = static_cast<int>(Input.Indices.size() / 3);
		Definition.identifyEdges = true;
		OwnedBox3DMesh Global(b3CreateMesh(&Definition, nullptr, 0));
		Check(Global != nullptr, "reference global mesh builds");
		if (!Global) return;
		std::vector<bool> Seen(static_cast<std::size_t>(Global->triangleCount), false);
		int Checked = 0;
		int NonzeroFlags = 0;
		for (const auto &Chunk : Set.Chunks) {
			Check(Chunk && Chunk->triangleCount > 0 &&
				Chunk->triangleCount <= static_cast<int>(MaximumBox3DMeshChunkTriangles),
				"native chunk triangle count stays in range");
			if (!Chunk) continue;
			Check(Chunk->materialCount == 1 && Chunk->hash != 0,
				"native chunk restores one material and a nonzero content hash");
			const auto StoredHash = Chunk->hash;
			Chunk->hash = 0;
			auto RecomputedHash = b3Hash(B3_HASH_INIT,
				reinterpret_cast<const std::uint8_t *>(Chunk.get()), Chunk->byteCount);
			if (RecomputedHash == 0) RecomputedHash = 1;
			Chunk->hash = StoredHash;
			Check(StoredHash == RecomputedHash, "mutated chunk content hash matches Box3D's hash rule");
			for (int Output = 0; Output < Chunk->triangleCount; ++Output) {
				int Match = -1;
				for (int GlobalIndex = 0; GlobalIndex < Global->triangleCount; ++GlobalIndex) {
					if (SameTriangle(*Global, GlobalIndex, *Chunk, Output)) {
						Match = GlobalIndex;
						break;
					}
				}
				Check(Match >= 0, "each chunk triangle matches a global triangle");
				if (Match < 0) continue;
				Check(!Seen[static_cast<std::size_t>(Match)], "no global triangle is duplicated");
				Seen[static_cast<std::size_t>(Match)] = true;
				Check(b3GetMeshFlags(Chunk.get())[Output] == b3GetMeshFlags(Global.get())[Match],
					"global edge flags survive chunk remapping and triangle reordering");
				Check(b3GetMeshMaterialIndices(Chunk.get())[Output] == 0,
					"ordinal material labels are removed after remapping");
				NonzeroFlags += b3GetMeshFlags(Global.get())[Match] != 0;
				++Checked;
			}
		}
		Check(Checked == Global->triangleCount, "all global triangles appear in chunks");
		if (Global->triangleCount > 2)
			Check(NonzeroFlags > 0, "grid fixture exercises nonzero adjacency flags");
	}
}

int main() {
	constexpr std::size_t Budget = 64 * 1024 * 1024;
	const auto InitialBox3DBytes = b3GetByteCount();
	{
		auto Input = Grid(1, 1);
		auto Result = BuildBox3DMeshChunks(Input.Vertices, Input.Indices, Budget);
		Check(Result && Result->Chunks.size() == 1 && Result->Chunks[0]->triangleCount == 2,
			"two-triangle mesh keeps the single native mesh fast path");
		if (Result) {
			Check(Result->NativeBytes == Result->PeakStagingNativeBytes,
				"single native mesh has no duplicate staging blob");
			CheckFlagsAndCoverage(Input, *Result);
		}
	}
	{
		auto Input = Grid(1, 1);
		Input.Indices[0] = static_cast<std::int32_t>(Input.Vertices.size());
		Check(!BuildBox3DMeshChunks(Input.Vertices, Input.Indices, Budget),
			"out-of-range triangle index is denied before Box3D ingestion");
		Input.Indices[0] = 0;
		Input.Vertices[0].x = std::numeric_limits<float>::infinity();
		Check(!BuildBox3DMeshChunks(Input.Vertices, Input.Indices, Budget),
			"non-finite vertex is denied before Box3D ingestion");
		Input.Vertices[0].x = 0.0f;
		Input.Vertices[0] = Input.Vertices[1];
		Check(!BuildBox3DMeshChunks(Input.Vertices, Input.Indices, Budget),
			"triangle loss from collapsed positions cannot publish a partial mesh");
	}
	{
		auto Input = DisconnectedTriangles(257);
		auto Result = BuildBox3DMeshChunks(Input.Vertices, Input.Indices, Budget);
		Check(Result && Result->Chunks.size() == 2 && Result->Chunks[0]->triangleCount == 255 &&
			Result->Chunks[1]->triangleCount == 2,
			"257 triangles cross the contact query capacity in exactly two chunks");
	}
	{
		auto Input = Grid(13, 10); // 260 connected coplanar triangles; split edges cross a chunk boundary.
		auto Result = BuildBox3DMeshChunks(Input.Vertices, Input.Indices, Budget);
		Check(Result && Result->Chunks.size() == 2, "connected grid splits into bounded chunks");
		if (Result) {
			Check(Result->NativeBytes > 0 && Result->PeakStagingNativeBytes > Result->NativeBytes,
				"staging reports the global mesh plus completed chunks");
			CheckFlagsAndCoverage(Input, *Result);
			const auto BeforeDenial = b3GetByteCount();
			const auto Denied = BuildBox3DMeshChunks(Input.Vertices, Input.Indices,
				Result->PeakStagingNativeBytes - 1);
			Check(!Denied, "one byte below measured peak staging budget is denied");
			Check(b3GetByteCount() == BeforeDenial,
				"budget denial releases temporary Box3D mesh data");
		}
	}
	Check(b3GetByteCount() == InitialBox3DBytes,
		"all successful and denied chunk sets release Box3D mesh data");
	if (Failures) return 1;
	std::cout << "[Geometry:MeshChunkTest] all checks passed\n";
	return 0;
}
