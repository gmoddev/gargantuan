#include "gargantuan/geometry/SolidGeometryCompiler.hpp"
#include "../src/assets/AssetImporter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

using namespace gargantuan;

namespace {
	using Clock = std::chrono::steady_clock;

	struct MemorySample {
		std::uint64_t CurrentBytes = 0;
		std::uint64_t PeakBytes = 0;
	};

	MemorySample ReadMemory() {
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS_EX Counters{};
		Counters.cb = sizeof(Counters);
		if (!GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Counters), sizeof(Counters))) return {};
		return {static_cast<std::uint64_t>(Counters.PrivateUsage),
			static_cast<std::uint64_t>(Counters.PeakPagefileUsage)};
#else
		MemorySample Result;
		std::ifstream Status("/proc/self/status");
		std::string Line;
		while (std::getline(Status, Line)) {
			std::istringstream Parsed(Line);
			std::string Name;
			std::uint64_t Kilobytes = 0;
			std::string Unit;
			if (!(Parsed >> Name >> Kilobytes >> Unit)) continue;
			if (Name == "VmRSS:") Result.CurrentBytes = Kilobytes * 1024;
			if (Name == "VmHWM:") Result.PeakBytes = Kilobytes * 1024;
		}
		if (Result.PeakBytes == 0) {
			rusage Usage{};
			if (getrusage(RUSAGE_SELF, &Usage) == 0)
				Result.PeakBytes = static_cast<std::uint64_t>(Usage.ru_maxrss) * 1024;
		}
		return Result;
#endif
	}

	SolidNode Box(std::string Id, glm::vec3 Size = {2, 2, 2}) {
		SolidNode Node;
		Node.Id = std::move(Id);
		Node.Kind = SolidNodeKind::Box;
		Node.Size = Size;
		return Node;
	}

	SolidNode Sphere(std::string Id, float Radius = 1.0f) {
		SolidNode Node;
		Node.Id = std::move(Id);
		Node.Kind = SolidNodeKind::Sphere;
		Node.Radius = Radius;
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

	SolidGraph Simple() {
		return {1, "result", {Box("a"), Move("b", "a", {0.7f, 0, 0}),
			Boolean("result", SolidNodeKind::Union, {"a", "b"})}};
	}

	SolidGraph Medium() {
		return {1, "result", {Box("a"), Sphere("sphere", 0.75f),
			Move("b", "sphere", {0.45f, 0.3f, 0}),
			Boolean("cut", SolidNodeKind::Subtraction, {"a", "b"}),
			Move("c", "a", {-0.7f, 0, 0}),
			Boolean("result", SolidNodeKind::Union, {"cut", "c"})}};
	}

	SolidGraph Complex() {
		SolidGraph Graph;
		Graph.Version = 1;
		Graph.Root = "result";
		Graph.Nodes.push_back(Box("base", {1.5f, 1.5f, 1.5f}));
		std::vector<std::string> Inputs;
		for (int Index = 0; Index < 12; ++Index) {
			const auto Id = "piece" + std::to_string(Index);
			Graph.Nodes.push_back(Move(Id, "base", {
				float(Index % 4) * 1.15f, float((Index / 4) % 3) * 1.15f, 0.0f}));
			Inputs.push_back(Id);
		}
		Graph.Nodes.push_back(Boolean("result", SolidNodeKind::Union, Inputs));
		return Graph;
	}

	SolidGraph DenseIntersection() {
		auto Graph = Complex();
		Graph.Nodes.back().Id = "union";
		Graph.Nodes.push_back(Sphere("sphere", 2.5f));
		Graph.Nodes.push_back(Boolean("result", SolidNodeKind::Intersection, {"union", "sphere"}));
		return Graph;
	}

	std::shared_ptr<const ImportedMesh> Prism(std::size_t Segments) {
		std::vector<RenderVertex> Vertices;
		std::vector<std::uint32_t> Indices;
		Vertices.reserve(Segments * 2);
		Indices.reserve((4 * Segments - 4) * 3);
		for (int Layer = 0; Layer < 2; ++Layer) {
			for (std::size_t Index = 0; Index < Segments; ++Index) {
				const double Angle = 6.28318530717958647692 * double(Index) / double(Segments);
				RenderVertex Vertex;
				Vertex.Position = {float(std::cos(Angle)), float(std::sin(Angle)),
					Layer == 0 ? -0.5f : 0.5f};
				Vertices.push_back(Vertex);
			}
		}
		for (std::size_t Index = 1; Index + 1 < Segments; ++Index) {
			Indices.insert(Indices.end(), {0, static_cast<std::uint32_t>(Index + 1),
				static_cast<std::uint32_t>(Index)});
			Indices.insert(Indices.end(), {static_cast<std::uint32_t>(Segments),
				static_cast<std::uint32_t>(Segments + Index),
				static_cast<std::uint32_t>(Segments + Index + 1)});
		}
		for (std::size_t Index = 0; Index < Segments; ++Index) {
			const auto Next = (Index + 1) % Segments;
			const auto A = static_cast<std::uint32_t>(Index);
			const auto B = static_cast<std::uint32_t>(Next);
			const auto C = static_cast<std::uint32_t>(Segments + Index);
			const auto D = static_cast<std::uint32_t>(Segments + Next);
			Indices.insert(Indices.end(), {A, B, D, A, D, C});
		}
		ImportedMesh Mesh;
		Mesh.Vertices = std::make_shared<const std::vector<RenderVertex>>(std::move(Vertices));
		Mesh.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(Indices));
		return std::make_shared<const ImportedMesh>(std::move(Mesh));
	}

	SolidGraph PrismIntersection(std::size_t Segments, float Offset) {
		SolidNode Operand;
		Operand.Id = "prism";
		Operand.Kind = SolidNodeKind::ImportedMesh;
		Operand.Mesh = Prism(Segments);
		return {1, "result", {Operand, Move("copy", "prism", {Offset, 0, 0}),
			Boolean("result", SolidNodeKind::Intersection, {"prism", "copy"})}};
	}

	std::string Hash(const SolidCompilation &Candidate) {
		const auto Encoded = EncodeAssetArtifact(ImportedAsset(Candidate.Mesh), AssetKind::Mesh);
		return Encoded ? AssetContentId::Hash(**Encoded).ToString() : "encode-failed";
	}

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	double Percentile(const std::vector<double> &Sorted, double Fraction) {
		if (Sorted.empty()) return 0;
		const auto Rank = static_cast<std::size_t>(std::ceil(Fraction * Sorted.size()));
		return Sorted[std::min(Sorted.size() - 1, std::max(std::size_t{1}, Rank) - 1)];
	}

	void Measure(const char *Name, const SolidGraph &Graph, int Iterations,
		std::size_t RequestedPairs = 0) {
		const auto Before = ReadMemory();
		std::vector<double> Times;
		std::size_t Triangles = 0;
		std::size_t InputTriangles = 0;
		std::size_t EvaluatedPairs = 0;
		std::size_t PeakIntermediateTriangles = 0;
		std::size_t Failures = 0;
		std::string Diagnostic;
		std::string FirstHash;
		bool HashStable = true;
		for (const auto &Node : Graph.Nodes)
			if (Node.Kind == SolidNodeKind::ImportedMesh && Node.Mesh && Node.Mesh->Indices)
				InputTriangles += Node.Mesh->Indices->size() / 3;
		for (int Index = 0; Index < Iterations; ++Index) {
			const auto Start = Clock::now();
			const auto Candidate = CompileSolidGeometry(Graph);
			Times.push_back(Milliseconds(Clock::now() - Start));
			if (!Candidate) { ++Failures; Diagnostic = Candidate.error().Code; continue; }
			Triangles = Candidate->Mesh.Indices->size() / 3;
			InputTriangles = Candidate->InputTriangles;
			EvaluatedPairs = Candidate->EvaluatedTrianglePairs;
			PeakIntermediateTriangles = Candidate->PeakIntermediateTriangles;
			const auto CurrentHash = Hash(*Candidate);
			if (FirstHash.empty()) FirstHash = CurrentHash;
			else HashStable &= FirstHash == CurrentHash;
		}
		std::sort(Times.begin(), Times.end());
		const auto After = ReadMemory();
		std::cout << "{\"Scenario\":\"" << Name << "\",\"Iterations\":" << Iterations
			<< ",\"Failures\":" << Failures << ",\"Diagnostic\":\"" << Diagnostic
			<< "\",\"InputTriangles\":" << InputTriangles
			<< ",\"RequestedPairCount\":";
		if (RequestedPairs) std::cout << RequestedPairs;
		else std::cout << "null";
		std::cout
			<< ",\"EvaluatedPairCount\":" << EvaluatedPairs
			<< ",\"PeakIntermediateTriangles\":" << PeakIntermediateTriangles
			<< ",\"OutputTriangles\":" << Triangles << ",\"P50Ms\":" << Percentile(Times, 0.50)
			<< ",\"P95Ms\":" << Percentile(Times, 0.95)
			<< ",\"P99Ms\":" << Percentile(Times, 0.99)
			<< ",\"StableHash\":" << (HashStable ? "true" : "false")
			<< ",\"Hash\":\"" << FirstHash << "\",\"MemoryBeforeBytes\":" << Before.CurrentBytes
			<< ",\"MemoryAfterBytes\":" << After.CurrentBytes
			<< ",\"RetainedDeltaBytes\":" << static_cast<std::int64_t>(After.CurrentBytes) -
				static_cast<std::int64_t>(Before.CurrentBytes)
			<< ",\"OsPeakBytes\":" << After.PeakBytes
			<< ",\"PeakScope\":\"process_lifetime\""
			<< ",\"MemoryMetric\":\""
#ifdef _WIN32
			<< "PrivateCommit"
#else
			<< "RSS"
#endif
			<< "\"}\n";
	}

	void MeasureIncrementalEdit(int Iterations) {
		auto Graph = Simple();
		std::vector<double> Times;
		std::string PreviousHash;
		int Changed = 0;
		for (int Index = 0; Index < Iterations; ++Index) {
			Graph.Nodes[1].Matrix[3][0] = 0.4f + 0.01f * Index;
			const auto Start = Clock::now();
			const auto Candidate = CompileSolidGeometry(Graph);
			Times.push_back(Milliseconds(Clock::now() - Start));
			if (Candidate) {
				const auto CurrentHash = Hash(*Candidate);
				if (!PreviousHash.empty() && CurrentHash != PreviousHash) ++Changed;
				PreviousHash = CurrentHash;
			}
		}
		std::sort(Times.begin(), Times.end());
		std::cout << "{\"Scenario\":\"incremental_edit\",\"Iterations\":" << Iterations
			<< ",\"ChangedHashes\":" << Changed << ",\"P50Ms\":" << Percentile(Times, 0.5)
			<< ",\"P95Ms\":" << Percentile(Times, 0.95)
			<< ",\"P99Ms\":" << Percentile(Times, 0.99) << "}\n";
	}

	void MeasureCancellation() {
		const auto Graph = PrismIntersection(120, 0.1f);
		AssetCancellationToken Token;
		SolidCompileProbe Probe;
		std::atomic_bool Done = false;
		std::string Outcome;
		std::thread Worker([&] {
			const auto Result = CompileSolidGeometry(Graph, Token,
				Clock::time_point::max(), {}, &Probe);
			Outcome = Result ? "Completed" : Result.error().Code;
			Done.store(true, std::memory_order_release);
		});
		const auto WaitStart = Clock::now();
		while (!Probe.InsideBoolean.load(std::memory_order_acquire) &&
			!Done.load(std::memory_order_acquire) && Clock::now() - WaitStart < std::chrono::seconds(5))
			std::this_thread::yield();
		const bool ActiveAtRequest = Probe.InsideBoolean.load(std::memory_order_acquire);
		const auto Requested = Clock::now();
		Token.Cancel();
		Worker.join();
		std::cout << "{\"Scenario\":\"cancellation\",\"Outcome\":\"" << Outcome
			<< "\",\"RequestedWhileBooleanActive\":" << (ActiveAtRequest ? "true" : "false")
			<< ",\"JoinLatencyMs\":" << Milliseconds(Clock::now() - Requested) << "}\n";
	}

	void MeasureStress() {
		constexpr std::size_t AcceptedTriangles = 4 * 239 - 4;
		constexpr std::size_t DeniedTriangles = 4 * 252 - 4;
		Measure("near_pair_limit_accepted", PrismIntersection(239, 0.1f), 1,
			AcceptedTriangles * AcceptedTriangles);
		Measure("over_pair_limit_denied", PrismIntersection(252, 0.1f), 1,
			DeniedTriangles * DeniedTriangles);
		Measure("coplanar_touching", {1, "result", {Box("a"),
			Move("b", "a", {2, 0, 0}),
			Boolean("result", SolidNodeKind::Union, {"a", "b"})}}, 5);
		Measure("thin_cut", {1, "result", {Box("a", {2, 0.02f, 2}),
			Move("b", "a", {0.01f, 0, 0}),
			Boolean("result", SolidNodeKind::Subtraction, {"a", "b"})}}, 5);
		std::uint32_t Seed = 0x4ac8172du;
		int Accepted = 0;
		int Rejected = 0;
		const auto Start = Clock::now();
		for (int Index = 0; Index < 128; ++Index) {
			auto Next = [&] { Seed ^= Seed << 13; Seed ^= Seed >> 17; Seed ^= Seed << 5; return Seed; };
			auto Case = Simple();
			Case.Nodes[1].Matrix[3][0] = float(Next() % 501) / 100.0f - 2.5f;
			Case.Nodes[1].Matrix[3][1] = float(Next() % 501) / 100.0f - 2.5f;
			if (Index % 9 == 0) Case.Nodes[2].Inputs[1] = "missing";
			if (Index % 13 == 0) Case.Nodes[1].Inputs = {"result"};
			if (Index % 17 == 0) Case.Nodes[0].Size.x = 0;
			if (Index % 19 == 0) Case.Nodes[0].Size.y = 1e8f;
			if (Index % 23 == 0) Case.Nodes[1].Matrix[0][0] = 0;
			if (CompileSolidGeometry(Case)) ++Accepted;
			else ++Rejected;
		}
		std::cout << "{\"Scenario\":\"seeded_128\",\"Seed\":1254627117,\"Accepted\":"
			<< Accepted << ",\"Rejected\":" << Rejected
			<< ",\"TotalMs\":" << Milliseconds(Clock::now() - Start) << "}\n";
	}
}

int main(int ArgumentCount, char **Arguments) {
	bool Stress = false;
	bool Full = false;
	std::string_view Selected = "all";
	for (int Index = 1; Index < ArgumentCount; ++Index) {
		const std::string_view Argument = Arguments[Index];
		if (Argument == "--stress") Stress = true;
		else if (Argument == "--full101") Full = true;
		else if (Argument == "--scenario" && Index + 1 < ArgumentCount) Selected = Arguments[++Index];
		else { std::cerr << "Usage: SolidGeometryBenchmark [--stress] [--full101] [--scenario NAME]\n"; return 2; }
	}
	const int Iterations = Full ? 101 : Stress ? 25 : 5;
	if (Selected == "all" || Selected == "simple_union") Measure("simple_union", Simple(), Iterations);
	if (Selected == "all" || Selected == "medium_nested") Measure("medium_nested", Medium(), Iterations);
	if (Selected == "all" || Selected == "complex_union") Measure("complex_union", Complex(), Iterations);
	if (Selected == "all" || Selected == "dense_intersection")
		Measure("dense_intersection", DenseIntersection(), Full ? 101 : Stress ? 5 : 1);
	if (Selected == "all" || Selected == "near_coplanar")
		Measure("near_coplanar", {1, "result", {Box("a"), Move("b", "a", {0.0001f, 0, 0}),
			Boolean("result", SolidNodeKind::Subtraction, {"a", "b"})}}, Full ? 101 : Stress ? 5 : 1);
	if (Selected == "all" || Selected == "incremental_edit") MeasureIncrementalEdit(Iterations);
	if (Selected == "all" || Selected == "cancellation") MeasureCancellation();
	if (Stress && Selected == "all") MeasureStress();
	if (Selected == "near_pair_limit_accepted") {
		constexpr std::size_t Triangles = 4 * 239 - 4;
		Measure("near_pair_limit_accepted", PrismIntersection(239, 0.1f), 1, Triangles * Triangles);
	}
	if (Selected == "over_pair_limit_denied") {
		constexpr std::size_t Triangles = 4 * 252 - 4;
		Measure("over_pair_limit_denied", PrismIntersection(252, 0.1f), 1, Triangles * Triangles);
	}
	if (Selected == "coplanar_touching")
		Measure("coplanar_touching", {1, "result", {Box("a"),
			Move("b", "a", {2, 0, 0}),
			Boolean("result", SolidNodeKind::Union, {"a", "b"})}}, Iterations);
	if (Selected == "thin_cut")
		Measure("thin_cut", {1, "result", {Box("a", {2, 0.02f, 2}),
			Move("b", "a", {0.01f, 0, 0}),
			Boolean("result", SolidNodeKind::Subtraction, {"a", "b"})}}, Iterations);
	return 0;
}
