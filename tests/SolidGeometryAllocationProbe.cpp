// Benchmark-only C++ allocation accounting for the headless solid compiler.
// This executable is deliberately separate from SolidGeometryBenchmark: global
// new/delete interception changes timing and does not observe malloc or OS heaps.
#include "gargantuan/geometry/SolidGeometryCompiler.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

using namespace gargantuan;

namespace {
	struct alignas(std::max_align_t) AllocationHeader {
		void *Raw = nullptr;
		std::size_t RequestedBytes = 0;
		std::size_t Generation = 0;
		bool Counted = false;
	};

	static_assert(sizeof(AllocationHeader) % alignof(AllocationHeader) == 0);
	std::atomic_bool Recording{false};
	std::atomic<std::size_t> CurrentGeneration{0};
	std::atomic<std::size_t> AllocationCount{0};
	std::atomic<std::size_t> RequestedBytes{0};
	std::atomic<std::size_t> LiveBytes{0};
	std::atomic<std::size_t> PeakLiveBytes{0};

	void *Allocate(std::size_t Size, std::size_t Alignment) noexcept {
		Alignment = std::max(Alignment, alignof(AllocationHeader));
		if ((Alignment & (Alignment - 1)) != 0) return nullptr;
		const auto PayloadBytes = std::max(Size, std::size_t{1});
		if (Alignment - 1 > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
			PayloadBytes > std::numeric_limits<std::size_t>::max() -
				(sizeof(AllocationHeader) + Alignment - 1))
			return nullptr;
		const auto Overhead = sizeof(AllocationHeader) + Alignment - 1;
		void *Raw = std::malloc(PayloadBytes + Overhead);
		if (!Raw) return nullptr;
		auto *Start = static_cast<unsigned char *>(Raw) + sizeof(AllocationHeader);
		const auto Remainder = reinterpret_cast<std::uintptr_t>(Start) & (Alignment - 1);
		auto *Result = Start + ((Alignment - Remainder) & (Alignment - 1));
		auto *Header = reinterpret_cast<AllocationHeader *>(Result - sizeof(AllocationHeader));
		Header->Raw = Raw;
		Header->RequestedBytes = Size;
		Header->Counted = Recording.load(std::memory_order_relaxed);
		Header->Generation = CurrentGeneration.load(std::memory_order_relaxed);
		if (Header->Counted) {
			AllocationCount.fetch_add(1, std::memory_order_relaxed);
			RequestedBytes.fetch_add(Size, std::memory_order_relaxed);
			const auto Live = LiveBytes.fetch_add(Size, std::memory_order_relaxed) + Size;
			auto Peak = PeakLiveBytes.load(std::memory_order_relaxed);
			while (Live > Peak && !PeakLiveBytes.compare_exchange_weak(
				Peak, Live, std::memory_order_relaxed)) {}
		}
		return Result;
	}

	void Deallocate(void *Pointer) noexcept {
		if (!Pointer) return;
		auto *Header = reinterpret_cast<AllocationHeader *>(
			static_cast<unsigned char *>(Pointer) - sizeof(AllocationHeader));
		if (Header->Counted &&
			Header->Generation == CurrentGeneration.load(std::memory_order_relaxed))
			LiveBytes.fetch_sub(Header->RequestedBytes, std::memory_order_relaxed);
		std::free(Header->Raw);
	}
}

void *operator new(std::size_t Size) {
	if (void *Pointer = Allocate(Size, alignof(std::max_align_t))) return Pointer;
	throw std::bad_alloc();
}
void *operator new[](std::size_t Size) {
	if (void *Pointer = Allocate(Size, alignof(std::max_align_t))) return Pointer;
	throw std::bad_alloc();
}
void *operator new(std::size_t Size, std::align_val_t Alignment) {
	if (void *Pointer = Allocate(Size, static_cast<std::size_t>(Alignment))) return Pointer;
	throw std::bad_alloc();
}
void *operator new[](std::size_t Size, std::align_val_t Alignment) {
	if (void *Pointer = Allocate(Size, static_cast<std::size_t>(Alignment))) return Pointer;
	throw std::bad_alloc();
}
void *operator new(std::size_t Size, const std::nothrow_t &) noexcept {
	return Allocate(Size, alignof(std::max_align_t));
}
void *operator new[](std::size_t Size, const std::nothrow_t &) noexcept {
	return Allocate(Size, alignof(std::max_align_t));
}
void *operator new(std::size_t Size, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	return Allocate(Size, static_cast<std::size_t>(Alignment));
}
void *operator new[](std::size_t Size, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	return Allocate(Size, static_cast<std::size_t>(Alignment));
}
void operator delete(void *Pointer) noexcept { Deallocate(Pointer); }
void operator delete[](void *Pointer) noexcept { Deallocate(Pointer); }
void operator delete(void *Pointer, std::size_t) noexcept { Deallocate(Pointer); }
void operator delete[](void *Pointer, std::size_t) noexcept { Deallocate(Pointer); }
void operator delete(void *Pointer, std::align_val_t) noexcept { Deallocate(Pointer); }
void operator delete[](void *Pointer, std::align_val_t) noexcept { Deallocate(Pointer); }
void operator delete(void *Pointer, std::size_t, std::align_val_t) noexcept { Deallocate(Pointer); }
void operator delete[](void *Pointer, std::size_t, std::align_val_t) noexcept { Deallocate(Pointer); }
void operator delete(void *Pointer, const std::nothrow_t &) noexcept { Deallocate(Pointer); }
void operator delete[](void *Pointer, const std::nothrow_t &) noexcept { Deallocate(Pointer); }
void operator delete(void *Pointer, std::align_val_t, const std::nothrow_t &) noexcept { Deallocate(Pointer); }
void operator delete[](void *Pointer, std::align_val_t, const std::nothrow_t &) noexcept { Deallocate(Pointer); }

namespace {
	bool VerifyHooks() {
		CurrentGeneration.fetch_add(1, std::memory_order_relaxed);
		AllocationCount.store(0, std::memory_order_relaxed);
		RequestedBytes.store(0, std::memory_order_relaxed);
		LiveBytes.store(0, std::memory_order_relaxed);
		PeakLiveBytes.store(0, std::memory_order_relaxed);
		Recording.store(true, std::memory_order_release);
		void *Zero = ::operator new(0);
		void *Aligned = ::operator new(31, std::align_val_t{64});
		void *Nothrow = ::operator new[](7, std::nothrow);
		const bool Okay = Zero && Aligned && Nothrow &&
			(reinterpret_cast<std::uintptr_t>(Aligned) % 64) == 0 &&
			AllocationCount.load(std::memory_order_relaxed) == 3 &&
			RequestedBytes.load(std::memory_order_relaxed) == 38 &&
			PeakLiveBytes.load(std::memory_order_relaxed) == 38 &&
			::operator new(std::numeric_limits<std::size_t>::max(), std::nothrow) == nullptr;
		::operator delete(Zero, std::size_t{0});
		::operator delete(Aligned, std::size_t{31}, std::align_val_t{64});
		::operator delete[](Nothrow, std::nothrow);
		Recording.store(false, std::memory_order_release);
		return Okay && LiveBytes.load(std::memory_order_relaxed) == 0;
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

	SolidGraph Simple() {
		return {1, "result", {Box("a"), Move("b", "a", {0.7f, 0, 0}),
			Boolean("result", SolidNodeKind::Union, {"a", "b"})}};
	}

	SolidGraph Medium() {
		SolidNode Sphere;
		Sphere.Id = "sphere";
		Sphere.Kind = SolidNodeKind::Sphere;
		Sphere.Radius = 0.75f;
		return {1, "result", {Box("a"), Sphere, Move("b", "sphere", {0.45f, 0.3f, 0}),
			Boolean("cut", SolidNodeKind::Subtraction, {"a", "b"}),
			Move("c", "a", {-0.7f, 0, 0}),
			Boolean("result", SolidNodeKind::Union, {"cut", "c"})}};
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

	SolidGraph NearPairLimit() {
		SolidNode Operand;
		Operand.Id = "prism";
		Operand.Kind = SolidNodeKind::ImportedMesh;
		Operand.Mesh = Prism(239); // 952 triangles per operand; 906304 estimated pairs.
		return {1, "result", {Operand, Move("copy", "prism", {0.1f, 0, 0}),
			Boolean("result", SolidNodeKind::Intersection, {"prism", "copy"})}};
	}

	struct AllocationSample {
		bool Succeeded = false;
		char Diagnostic[64]{};
		std::size_t InputTriangles = 0;
		std::size_t OutputTriangles = 0;
		std::size_t EvaluatedPairs = 0;
		std::size_t Count = 0;
		std::size_t TotalRequestedBytes = 0;
		std::size_t PeakBytes = 0;
		std::size_t LiveWithCandidateBytes = 0;
		std::size_t RetainedAfterReleaseBytes = 0;
	};

	AllocationSample Measure(const SolidGraph &Graph) {
		AllocationSample Sample;
		Recording.store(false, std::memory_order_release);
		CurrentGeneration.fetch_add(1, std::memory_order_relaxed);
		AllocationCount.store(0, std::memory_order_relaxed);
		RequestedBytes.store(0, std::memory_order_relaxed);
		LiveBytes.store(0, std::memory_order_relaxed);
		PeakLiveBytes.store(0, std::memory_order_relaxed);
		Recording.store(true, std::memory_order_release);
		{
			const auto Candidate = CompileSolidGeometry(Graph);
			Sample.Succeeded = Candidate.has_value();
			if (Candidate) {
				Sample.InputTriangles = Candidate->InputTriangles;
				Sample.OutputTriangles = Candidate->Mesh.Indices->size() / 3;
				Sample.EvaluatedPairs = Candidate->EvaluatedTrianglePairs;
			} else {
				std::snprintf(Sample.Diagnostic, sizeof(Sample.Diagnostic), "%s",
					Candidate.error().Code.c_str());
			}
			Sample.LiveWithCandidateBytes = LiveBytes.load(std::memory_order_relaxed);
		}
		Recording.store(false, std::memory_order_release);
		Sample.Count = AllocationCount.load(std::memory_order_relaxed);
		Sample.TotalRequestedBytes = RequestedBytes.load(std::memory_order_relaxed);
		Sample.PeakBytes = PeakLiveBytes.load(std::memory_order_relaxed);
		Sample.RetainedAfterReleaseBytes = LiveBytes.load(std::memory_order_relaxed);
		return Sample;
	}

	void Print(const char *Name, const AllocationSample &Sample) {
		std::printf("{\"Scenario\":\"%s\",\"Succeeded\":%s,\"Diagnostic\":\"%s\","
			"\"InputTriangles\":%zu,\"OutputTriangles\":%zu,\"EvaluatedPairs\":%zu,"
			"\"CppAllocationCount\":%zu,\"CppRequestedBytes\":%zu,"
			"\"CppPeakLiveBytes\":%zu,\"CppLiveWithCandidateBytes\":%zu,"
			"\"CppRetainedAfterCandidateReleaseBytes\":%zu}\n",
			Name, Sample.Succeeded ? "true" : "false", Sample.Diagnostic,
			Sample.InputTriangles, Sample.OutputTriangles, Sample.EvaluatedPairs,
			Sample.Count, Sample.TotalRequestedBytes, Sample.PeakBytes,
			Sample.LiveWithCandidateBytes, Sample.RetainedAfterReleaseBytes);
	}
}

int main(int ArgumentCount, char **Arguments) {
	std::string_view Selected = "all";
	if (ArgumentCount == 3 && std::string_view(Arguments[1]) == "--scenario")
		Selected = Arguments[2];
	else if (ArgumentCount != 1) {
		std::fprintf(stderr, "Usage: SolidGeometryAllocationProbe [--scenario simple|medium|near_pair_limit]\n");
		return 2;
	}
	if (Selected != "all" && Selected != "simple" && Selected != "medium" &&
		Selected != "near_pair_limit") {
		std::fprintf(stderr, "Unknown scenario\n");
		return 2;
	}
	if (!VerifyHooks()) {
		std::fprintf(stderr, "[Geometry:AllocationProbe] Global allocation hook self-check failed\n");
		return 1;
	}
	// Prime one-time library setup before accounting. Scenario graphs and reporting
	// are outside the count. Each sample covers only a single compiler call.
	const auto Warmup = CompileSolidGeometry(Simple());
	if (!Warmup) return 1;
	int Failures = 0;
	if (Selected == "all" || Selected == "simple") {
		const auto Sample = Measure(Simple());
		Print("simple", Sample);
		Failures += !Sample.Succeeded;
	}
	if (Selected == "all" || Selected == "medium") {
		const auto Sample = Measure(Medium());
		Print("medium", Sample);
		Failures += !Sample.Succeeded;
	}
	if (Selected == "all" || Selected == "near_pair_limit") {
		const auto Sample = Measure(NearPairLimit());
		Print("near_pair_limit", Sample);
		Failures += !Sample.Succeeded;
	}
	return Failures ? 1 : 0;
}
