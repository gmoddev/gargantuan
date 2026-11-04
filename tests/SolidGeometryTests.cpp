#include "gargantuan/geometry/SolidGeometryCompiler.hpp"
#include "../src/assets/AssetImporter.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>

using namespace gargantuan;

namespace gargantuan { std::unique_ptr<IAssetImporter> CreateSolidImporter(); }

namespace {
	int Failures = 0;
	void Check(bool Condition, const char *Message) {
		if (!Condition) { std::cerr << "FAIL: " << Message << '\n'; ++Failures; }
	}

	SolidNode Box(std::string Id, glm::vec3 Size = {2, 2, 2}) {
		SolidNode Node;
		Node.Id = std::move(Id);
		Node.Kind = SolidNodeKind::Box;
		Node.Size = Size;
		return Node;
	}

	SolidNode Move(std::string Id, std::string Input, glm::vec3 Offset) {
		SolidNode Node;
		Node.Id = std::move(Id);
		Node.Kind = SolidNodeKind::Transform;
		Node.Inputs = {std::move(Input)};
		Node.Matrix = glm::translate(glm::mat4(1), Offset);
		return Node;
	}

	SolidNode Boolean(std::string Id, SolidNodeKind Kind, std::vector<std::string> Inputs) {
		SolidNode Node;
		Node.Id = std::move(Id);
		Node.Kind = Kind;
		Node.Inputs = std::move(Inputs);
		return Node;
	}

	std::string Hash(const ImportedMesh &Mesh) {
		auto Encoded = EncodeAssetArtifact(ImportedAsset(Mesh), AssetKind::Mesh);
		return Encoded ? AssetContentId::Hash(**Encoded).ToString() : "encode-failed";
	}

	std::shared_ptr<const ImportedMesh> CubeOperand() {
		const std::vector<glm::vec3> Positions = {
			{-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
			{-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1},
		};
		const std::vector<std::uint32_t> Indices = {
			0,2,1, 0,3,2, 4,5,6, 4,6,7,
			0,4,7, 0,7,3, 1,2,6, 1,6,5,
			0,1,5, 0,5,4, 3,7,6, 3,6,2,
		};
		std::vector<RenderVertex> Vertices;
		for (const auto Position : Positions) {
			RenderVertex Vertex;
			Vertex.Position = Position;
			Vertices.push_back(Vertex);
		}
		ImportedMesh Mesh;
		Mesh.Vertices = std::make_shared<const std::vector<RenderVertex>>(std::move(Vertices));
		Mesh.Indices = std::make_shared<const std::vector<std::uint32_t>>(Indices);
		return std::make_shared<const ImportedMesh>(std::move(Mesh));
	}
}

int main() {
	SolidGraph Graph{1, "result", {
		Box("box"), Move("moved", "box", {1, 0, 0}),
		Boolean("result", SolidNodeKind::Union, {"box", "moved"}),
	}};
	auto Union = CompileSolidGeometry(Graph);
	Check(Union && Union->Mesh.Vertices && Union->Mesh.Indices && Union->Collision.Positions &&
		Union->Mesh.Indices->size() == Union->Collision.Indices->size(),
		"overlapping box union produces both mesh and collision candidates");
	if (Union) {
		Check(Union->Mesh.Bounds.Minimum.x <= -1 && Union->Mesh.Bounds.Maximum.x >= 2,
			"union bounds contain both operands");
		const auto FirstHash = Hash(Union->Mesh);
		auto Again = CompileSolidGeometry(Graph);
		Check(Again && FirstHash != "encode-failed" && Hash(Again->Mesh) == FirstHash,
			"repeated same-profile compilation produces identical canonical mesh bytes");
	}
	SolidGraph OneBox{1, "box", {Box("box")}};
	auto Oriented = CompileSolidGeometry(OneBox);
	if (Oriented) {
		bool FaceUvOkay = true;
		for (std::size_t Index = 0; Index < Oriented->Mesh.Vertices->size(); Index += 3) {
			const auto &A = (*Oriented->Mesh.Vertices)[Index];
			const auto &B = (*Oriented->Mesh.Vertices)[Index + 1];
			const auto &C = (*Oriented->Mesh.Vertices)[Index + 2];
			const auto Du = B.TextureCoordinate - A.TextureCoordinate;
			const auto Dv = C.TextureCoordinate - A.TextureCoordinate;
			const auto UvArea = Du.x * Dv.y - Du.y * Dv.x;
			FaceUvOkay &= std::abs(UvArea) > 1.0e-5f &&
				std::abs(glm::dot(A.Normal, glm::vec3(A.Tangent))) < 1.0e-5f &&
				std::abs(A.Tangent.w) == 1.0f;
		}
		Check(FaceUvOkay, "all six box face orientations have nondegenerate UVs and aligned tangents");
		SolidNode RoundTripOperand;
		RoundTripOperand.Id = "baked";
		RoundTripOperand.Kind = SolidNodeKind::ImportedMesh;
		RoundTripOperand.Mesh = std::make_shared<const ImportedMesh>(Oriented->Mesh);
		SolidGraph RoundTrip{1, "baked", {RoundTripOperand}};
		const auto First = CompileSolidGeometry(RoundTrip);
		const auto Second = CompileSolidGeometry(RoundTrip);
		Check(First && Second && Hash(First->Mesh) == Hash(Second->Mesh),
			"compiler-produced flat mesh round-trips as a closed imported operand with repeat hash");
		RoundTrip.Root = "combined";
		RoundTrip.Nodes.push_back(Box("other"));
		RoundTrip.Nodes.push_back(Move("offset", "other", {0.5f, 0, 0}));
		RoundTrip.Nodes.push_back(Boolean("combined", SolidNodeKind::Union, {"baked", "offset"}));
		Check(CompileSolidGeometry(RoundTrip).has_value(),
			"compiler-produced flat mesh participates in a later Boolean");
	} else Check(false, "single box compiles for face orientation test");
	Graph.Nodes.back().Kind = SolidNodeKind::Intersection;
	auto Intersection = CompileSolidGeometry(Graph);
	Check(Intersection && Intersection->Mesh.Bounds.Minimum.x >= -0.001f &&
		Intersection->Mesh.Bounds.Maximum.x <= 1.001f,
		"overlapping box intersection keeps only common volume");
	Graph.Nodes.back().Kind = SolidNodeKind::Subtraction;
	auto Difference = CompileSolidGeometry(Graph);
	Check(Difference && Difference->Mesh.Bounds.Minimum.x <= -1 &&
		Difference->Mesh.Bounds.Maximum.x <= 0.001f,
		"ordered subtraction cuts the first operand by the second");
	Graph.Nodes.back().Inputs = {"moved", "box"};
	auto ReverseDifference = CompileSolidGeometry(Graph);
	Check(ReverseDifference && Difference && Hash(ReverseDifference->Mesh) != Hash(Difference->Mesh),
		"reversing subtraction operands changes the output");

	SolidNode Imported;
	Imported.Id = "imported";
	Imported.Kind = SolidNodeKind::ImportedMesh;
	Imported.Mesh = CubeOperand();
	SolidGraph ImportedGraph{1, "imported", {Imported}};
	Check(CompileSolidGeometry(ImportedGraph).has_value(), "closed imported mesh compiles");
	ImportedGraph.Nodes[0].Mesh = std::make_shared<ImportedMesh>(*Imported.Mesh);
	auto Open = *Imported.Mesh;
	Open.Indices = std::make_shared<const std::vector<std::uint32_t>>(
		std::vector<std::uint32_t>(Imported.Mesh->Indices->begin(), Imported.Mesh->Indices->end() - 3));
	ImportedGraph.Nodes[0].Mesh = std::make_shared<const ImportedMesh>(std::move(Open));
	auto NonManifold = CompileSolidGeometry(ImportedGraph);
	Check(!NonManifold && NonManifold.error().Code == "NonManifold", "open imported mesh is rejected");

	SolidGraph Nested{1, "outer", {
		Box("base"), Move("offset", "base", {0.25f, 0.5f, 0}),
		Boolean("inner", SolidNodeKind::Subtraction, {"base", "offset"}),
		Boolean("outer", SolidNodeKind::Union, {"inner", "offset"}),
	}};
	Check(CompileSolidGeometry(Nested).has_value(), "nested Boolean graph compiles");
	Nested.Nodes.push_back(Boolean("bad", SolidNodeKind::Union, {"bad", "base"}));
	Check(!CompileSolidGeometry(Nested), "unreachable cycle is rejected during whole-graph validation");
	Nested.Nodes.pop_back();
	Nested.Nodes.push_back(Boolean("bad", SolidNodeKind::Union, {"missing", "base"}));
	Check(!CompileSolidGeometry(Nested), "unreachable missing reference is rejected");
	Nested.Nodes.pop_back();
	Nested.Nodes.push_back(Box("base"));
	Check(!CompileSolidGeometry(Nested), "duplicate stable node ID is rejected");
	Nested.Nodes.pop_back();
	Nested.Nodes.push_back(Box("invalid", {-1, 1, 1}));
	Check(!CompileSolidGeometry(Nested), "unreachable invalid primitive is rejected");
	Nested.Nodes.pop_back();
	Nested.Nodes.push_back(Move("invalid", "base", {0, 0, 0}));
	Nested.Nodes.back().Matrix[0][0] = 0;
	Check(!CompileSolidGeometry(Nested), "unreachable singular transform is rejected");
	Nested.Nodes.pop_back();
	SolidNode InvalidImported = Imported;
	InvalidImported.Id = "invalid";
	auto BadMesh = *Imported.Mesh;
	auto BadIndices = *Imported.Mesh->Indices;
	BadIndices[0] = 999;
	BadMesh.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(BadIndices));
	InvalidImported.Mesh = std::make_shared<const ImportedMesh>(std::move(BadMesh));
	Nested.Nodes.push_back(std::move(InvalidImported));
	Check(!CompileSolidGeometry(Nested), "unreachable imported mesh with invalid index is rejected");
	Nested.Nodes.pop_back();
	SolidGraph SharedDepth{1, "long", {Box("leaf"), Move("short", "leaf", {0, 0, 0})}};
	for (int Index = 0; Index < 8; ++Index)
		SharedDepth.Nodes.push_back(Move("long" + std::to_string(Index),
			Index ? "long" + std::to_string(Index - 1) : "leaf", {0, 0, 0}));
	SharedDepth.Root = "long7";
	SolidCompileLimits DepthLimit;
	DepthLimit.MaximumDepth = 7;
	Check(!CompileSolidGeometry(SharedDepth, {}, std::chrono::steady_clock::time_point::max(), DepthLimit),
		"longest graph path is enforced even when a shared leaf was visited first by a short path");

	AssetCancellationToken Cancelled;
	Cancelled.Cancel();
	Check(!CompileSolidGeometry(Graph, Cancelled) &&
		CompileSolidGeometry(Graph, Cancelled).error().Code == "Cancelled",
		"pre-cancelled compilation rejects before kernel evaluation");
	Check(!CompileSolidGeometry(Graph, {}, std::chrono::steady_clock::now()) &&
		CompileSolidGeometry(Graph, {}, std::chrono::steady_clock::now()).error().Code == "Cancelled",
		"expired deadline rejects before kernel evaluation");
	SolidCompileLimits Small;
	Small.MaximumNodes = 2;
	Check(!CompileSolidGeometry(Graph, {}, std::chrono::steady_clock::time_point::max(), Small),
		"node limit rejects graph before evaluation");
	Small = {};
	Small.MaximumIntermediateTriangles = 1;
	Check(!CompileSolidGeometry(Graph, {}, std::chrono::steady_clock::time_point::max(), Small),
		"intermediate triangle limit is enforced");
	Small = {};
	Small.MaximumBooleanSteps = 0;
	Check(!CompileSolidGeometry(Graph, {}, std::chrono::steady_clock::time_point::max(), Small),
		"Boolean step budget is enforced");

	SolidGraph Touch{1, "result", {
		Box("a"), Move("b", "a", {2, 0, 0}),
		Boolean("result", SolidNodeKind::Union, {"a", "b"}),
	}};
	Check(CompileSolidGeometry(Touch).has_value(), "face-touching union compiles");
	Touch.Nodes[1].Matrix = glm::translate(glm::mat4(1), glm::vec3(4, 0, 0));
	Check(CompileSolidGeometry(Touch).has_value(), "disconnected union compiles");
	Touch.Nodes[2].Kind = SolidNodeKind::Intersection;
	auto Empty = CompileSolidGeometry(Touch);
	Check(!Empty && Empty.error().Code == "EmptySolid", "empty intersection has explicit diagnostic");
	SolidGraph Thin{1, "thin", {Box("thin", {2, 0.1f, 2})}};
	Check(CompileSolidGeometry(Thin).has_value(), "thin but representable solid compiles");
	SolidGraph Huge{1, "huge", {Box("huge", {1e8f, 2, 2})}};
	Check(!CompileSolidGeometry(Huge), "extreme input scale is rejected");
	SolidGraph NestedOverflow{1, "empty", {
		Box("base"), Move("first", "base", {90'000, 0, 0}),
		Move("second", "first", {90'000, 0, 0}),
		Boolean("empty", SolidNodeKind::Intersection, {"base", "second"}),
	}};
	const auto Overflow = CompileSolidGeometry(NestedOverflow);
	Check(!Overflow && Overflow.error().Code == "GeometryLimit",
		"nested transforms exceeding coordinate bounds reject before an empty final intersection");
	{
		const auto Importer = CreateSolidImporter();
		AssetImportContext Context;
		Context.RequestedKind = AssetKind::Mesh;
		Context.SourceExtension = ".solid";
		Context.Deadline = std::chrono::steady_clock::time_point::max();
		const auto Parse = [&](const std::string &Source) {
			return Importer->ImportGraph(std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t *>(Source.data()), Source.size()), Context);
		};
		const auto WrongVersion = Parse(R"({"Version":4294967297,"Root":"a","Nodes":[{"Id":"a","Kind":"Box","Size":[1,1,1]}]})");
		Check(!WrongVersion && WrongVersion.error().Code == "InvalidSolidGraph",
			"oversized unsigned version cannot wrap into supported version one");
		const auto OversizedIndex = Parse(R"({"Version":1,"Root":"a","Nodes":[{"Id":"a","Kind":"ImportedMesh","Positions":[[0,0,0],[1,0,0],[0,1,0]],"Indices":[4294967296,1,2]}]})");
		Check(!OversizedIndex && OversizedIndex.error().Code == "InvalidSolidMesh",
			"oversized unsigned triangle index cannot wrap to vertex zero");
	}

	if (std::getenv("GARGANTUAN_SOLID_BENCH")) {
		const auto Start = std::chrono::steady_clock::now();
		for (int Iteration = 0; Iteration < 16; ++Iteration)
			Check(CompileSolidGeometry(Graph).has_value(), "benchmark graph compiles");
		const auto Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - Start).count();
		std::cout << "[Geometry:Benchmark] 16 box differences: " << Milliseconds << " ms\n";
	}
	if (std::getenv("GARGANTUAN_SOLID_FUZZ")) {
		std::uint32_t Seed = 0x4ac8172du;
		for (int Iteration = 0; Iteration < 128; ++Iteration) {
			auto Next = [&] { Seed ^= Seed << 13; Seed ^= Seed >> 17; Seed ^= Seed << 5; return Seed; };
			const auto Offset = glm::vec3(float(Next() % 401) / 100.0f - 2.0f,
				float(Next() % 401) / 100.0f - 2.0f,
				float(Next() % 401) / 100.0f - 2.0f);
			SolidGraph Case{1, "result", {Box("a"), Move("b", "a", Offset),
				Boolean("result", static_cast<SolidNodeKind>(4 + Next() % 3), {"a", "b"})}};
			if (Iteration % 7 == 0) Case.Nodes[1].Inputs = {"result"};
			if (Iteration % 11 == 0) Case.Nodes[0].Size.x = 0;
			const auto Compiled = CompileSolidGeometry(Case);
			Check(!Compiled || (Compiled->Mesh.Indices && Compiled->Collision.Indices &&
				Compiled->Mesh.Indices->size() == Compiled->Collision.Indices->size()),
				"seeded malformed/overlap corpus returns complete candidates or diagnostics");
		}
		std::cout << "[Geometry:Fuzz] seed 0x4ac8172d, 128 cases\n";
	}
	if (Failures) return 1;
	std::cout << "All Solid Geometry tests passed\n";
	return 0;
}
