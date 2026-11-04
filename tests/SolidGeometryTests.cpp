#include "gargantuan/geometry/SolidGeometryCompiler.hpp"
#include "../src/assets/AssetImporter.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

using namespace gargantuan;

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

	if (std::getenv("GARGANTUAN_SOLID_BENCH")) {
		const auto Start = std::chrono::steady_clock::now();
		for (int Iteration = 0; Iteration < 16; ++Iteration)
			Check(CompileSolidGeometry(Graph).has_value(), "benchmark graph compiles");
		const auto Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - Start).count();
		std::cout << "[Geometry:Benchmark] 16 box differences: " << Milliseconds << " ms\n";
	}
	if (Failures) return 1;
	std::cout << "All Solid Geometry tests passed\n";
	return 0;
}
