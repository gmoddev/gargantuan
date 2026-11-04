#include "gargantuan/physics/PhysicsBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using Json = nlohmann::json;

	constexpr int QuerySamples = 101;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	[[nodiscard]] double Microseconds(Clock::time_point Started) {
		return std::chrono::duration<double, std::micro>(Clock::now() - Started).count();
	}

	[[nodiscard]] Json Percentiles(std::vector<double> Samples) {
		if (Samples.empty()) return nullptr;
		std::ranges::sort(Samples);
		auto At = [&](double Fraction) {
			const auto Rank = static_cast<std::size_t>(std::ceil(Fraction * Samples.size()));
			return Samples[std::min(Samples.size() - 1, std::max<std::size_t>(1, Rank) - 1)];
		};
		return {{"samples", Samples.size()}, {"p50Us", At(0.50)}, {"p95Us", At(0.95)}, {"p99Us", At(0.99)}};
	}

	[[nodiscard]] std::optional<std::uint64_t> ProcessRssBytes() {
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX Counters{};
		Counters.cb = sizeof(Counters);
		if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Counters),
				sizeof(Counters))) return std::nullopt;
		return static_cast<std::uint64_t>(Counters.WorkingSetSize);
#elif defined(__linux__)
		std::ifstream Input("/proc/self/statm");
		std::uint64_t VirtualPages = 0;
		std::uint64_t ResidentPages = 0;
		if (!(Input >> VirtualPages >> ResidentPages)) return std::nullopt;
		const auto PageBytes = sysconf(_SC_PAGESIZE);
		return PageBytes > 0 ? std::optional<std::uint64_t>(ResidentPages * static_cast<std::uint64_t>(PageBytes))
			: std::nullopt;
#else
		return std::nullopt;
#endif
	}

	[[nodiscard]] std::shared_ptr<const PhysicsTriangleMesh> MakeMesh(int Cells, std::uint64_t Revision) {
		auto Positions = std::make_shared<std::vector<glm::vec3>>();
		auto Indices = std::make_shared<std::vector<std::uint32_t>>();
		Positions->reserve(static_cast<std::size_t>(Cells + 1) * (Cells + 1));
		Indices->reserve(static_cast<std::size_t>(Cells) * Cells * 6);
		for (int Z = 0; Z <= Cells; ++Z)
			for (int X = 0; X <= Cells; ++X)
				Positions->push_back({2.0f * X / Cells - 1.0f, 0.0f, 2.0f * Z / Cells - 1.0f});
		for (int Z = 0; Z < Cells; ++Z) {
			for (int X = 0; X < Cells; ++X) {
				const auto A = static_cast<std::uint32_t>(Z * (Cells + 1) + X);
				const auto B = A + static_cast<std::uint32_t>(Cells + 1);
				Indices->insert(Indices->end(), {A, B, B + 1, A, B + 1, A + 1});
			}
		}
		return std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = "asset://mesh-collision-benchmark", .ContentRevision = Revision,
			.Positions = Positions, .Indices = Indices,
		});
	}

	[[nodiscard]] PhysicsBodyDesc MakeDescription(
		bool MeshCase, std::size_t Index, const std::shared_ptr<const PhysicsTriangleMesh> &Mesh
	) {
		PhysicsBodyDesc Result;
		Result.Anchored = true;
		Result.Transform = CFrame(static_cast<float>(Index % 40) * 4.0f,
			0.0f, static_cast<float>(Index / 40) * 4.0f);
		const glm::vec3 Scale{1.0f + static_cast<float>(Index % 3) * 0.2f, 1.0f,
			1.0f + static_cast<float>(Index % 5) * 0.1f};
		Result.Shape = MeshCase ? PhysicsShapeDesc{PhysicsShapeKind::StaticTriangleMesh, Scale, Mesh}
			: PhysicsShapeDesc{PhysicsShapeKind::Box, {2.0f * Scale.x, 0.2f, 2.0f * Scale.z}};
		return Result;
	}

	[[nodiscard]] Json EncodeResidency(const PhysicsCollisionResidency &Value) {
		return {{"activeResources", Value.ActiveResources}, {"activeConsumers", Value.ActiveConsumers},
			{"canonicalPositionIndexBytes", Value.CanonicalBytes}, {"nativeMeshBytes", Value.NativeMeshBytes},
			{"nativeShapeBytesKnown", Value.NativeShapeBytesKnown}, {"failedAdmissions", Value.FailedAdmissions}};
	}

	[[nodiscard]] Json RunCase(bool MeshCase, std::size_t Count, int Cells, int RevisionRounds) {
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, -10.0f, 0.0f}});
		Require(World.IsValid(), "physics backend unavailable");
		const auto RssBefore = ProcessRssBytes();
		auto Started = Clock::now();
		auto Source = MakeMesh(Cells, 1);
		const auto NeutralConstructionUs = Microseconds(Started);
		std::vector<PhysicsBodyId> Bodies;
		Bodies.reserve(Count);
		std::vector<double> AdditionalCreateSamples;
		AdditionalCreateSamples.reserve(Count > 0 ? Count - 1 : 0);
		Started = Clock::now();
		const auto First = World.CreateBody(MakeDescription(MeshCase, 0, Source));
		const auto FirstCreateUs = Microseconds(Started);
		Require(First.IsValid(), "first static collider creation failed");
		Bodies.push_back(First);
		for (std::size_t Index = 1; Index < Count; ++Index) {
			Started = Clock::now();
			const auto Body = World.CreateBody(MakeDescription(MeshCase, Index, Source));
			AdditionalCreateSamples.push_back(Microseconds(Started));
			Require(Body.IsValid(), "shared static collider creation failed");
			Bodies.push_back(Body);
		}
		const auto RssAfterCreate = ProcessRssBytes();
		const auto ResidencyCreated = World.GetCollisionResidency();
		if (MeshCase)
			Require(ResidencyCreated.ActiveResources == 1 && ResidencyCreated.ActiveConsumers == Count,
				"native mesh sharing count is wrong");

		const PhysicsRaycastRequest Ray{.Origin = {0.0f, 5.0f, 0.0f}, .Direction = {0.0f, -10.0f, 0.0f}};
		for (int Warm = 0; Warm < 8; ++Warm) Require(World.Raycast(Ray).HasHit(), "raycast warmup missed");
		std::vector<double> RaySamples;
		std::vector<double> MoverSamples;
		std::vector<double> StaticStepSamples;
		for (int Sample = 0; Sample < QuerySamples; ++Sample) {
			Started = Clock::now();
			const auto Hit = World.Raycast(Ray);
			RaySamples.push_back(Microseconds(Started));
			Require(Hit.Succeeded() && Hit.HasHit(), "static collider raycast missed");
			Started = Clock::now();
			const auto Mover = World.MoveKinematicCapsule({
				.Position = {0.0f, 2.0f, 0.0f}, .Radius = 0.5f, .Height = 2.0f,
				.Translation = {0.0f, -2.0f, 0.0f}, .Velocity = {0.0f, -2.0f, 0.0f},
			});
			MoverSamples.push_back(Microseconds(Started));
			Require(Mover.Succeeded() && Mover.Collided, "kinematic mover missed static collider");
			Started = Clock::now();
			(void)World.Step({});
			StaticStepSamples.push_back(Microseconds(Started));
		}

		PhysicsBodyDesc Dynamic;
		Dynamic.Shape = {.Kind = PhysicsShapeKind::Ball, .Size = {1.0f, 1.0f, 1.0f}};
		Dynamic.Transform = CFrame(0.0f, 2.0f, 0.0f);
		const auto Ball = World.CreateBody(Dynamic);
		Require(Ball.IsValid(), "dynamic contact fixture failed");
		std::vector<double> ContactStepSamples;
		std::size_t ContactEvents = 0;
		for (int Sample = 0; Sample < QuerySamples; ++Sample) {
			Started = Clock::now();
			const auto Step = World.Step({});
			ContactStepSamples.push_back(Microseconds(Started));
			ContactEvents += Step.Contacts.size();
		}
		Require(World.DestroyBody(Ball).Succeeded(), "dynamic contact fixture retirement failed");
		Require(ContactEvents > 0, "dynamic body produced no contact events against static collider");

		Json Revisions = Json::array();
		for (int Round = 0; Round < RevisionRounds; ++Round) {
			Source = MakeMesh(Cells, static_cast<std::uint64_t>(Round + 2));
			auto RevisedDescription = [&](std::size_t Index) {
				auto Value = MakeDescription(MeshCase, Index, Source);
				if (!MeshCase) Value.Shape.Size.y += 0.01f * static_cast<float>(Round + 1);
				return Value;
			};
			Started = Clock::now();
			Require(World.UpdateBody(Bodies.front(), RevisedDescription(0)).Succeeded(),
				"first revision replacement failed");
			const auto FirstReplaceUs = Microseconds(Started);
			const auto During = World.GetCollisionResidency();
			std::vector<double> Remaining;
			for (std::size_t Index = 1; Index < Bodies.size(); ++Index) {
				Started = Clock::now();
				Require(World.UpdateBody(Bodies[Index], RevisedDescription(Index)).Succeeded(),
					"revision replacement failed");
				Remaining.push_back(Microseconds(Started));
			}
			const auto After = World.GetCollisionResidency();
			Require(std::ranges::all_of(Bodies, [&](PhysicsBodyId Body) { return World.IsBodyValid(Body); }),
				"revision replacement changed neutral identity");
			if (MeshCase)
				Require(After.ActiveResources == 1 && After.ActiveConsumers == Count &&
					During.ActiveResources == (Count == 1 ? 1 : 2), "revision mesh ownership count is wrong");
			Revisions.push_back({{"revision", Round + 2}, {"firstReplacementUs", FirstReplaceUs},
				{"remainingReplacementUs", Percentiles(std::move(Remaining))},
				{"duringFirstReplacement", EncodeResidency(During)}, {"after", EncodeResidency(After)}});
		}

		Json Rejections = Json::object();
		if (MeshCase) {
			auto Bad = MakeDescription(true, 0, Source);
			Bad.Shape.Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
				.ResourceId = "asset://bad-benchmark", .ContentRevision = 1,
				.Positions = Source->Positions,
				.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 999999}),
			});
			Started = Clock::now();
			const auto Malformed = World.UpdateBody(First, Bad);
			Rejections["malformedUs"] = Microseconds(Started);
			Require(!Malformed.Succeeded() && World.IsBodyValid(First), "malformed mesh replaced prior shape");
			Bad = MakeDescription(true, 0, Source);
			Bad.Shape.Size = {0.01f, 1.0f, 0.01f};
			Started = Clock::now();
			const auto Scaled = World.UpdateBody(First, Bad);
			Rejections["scaledThinUs"] = Microseconds(Started);
			Require(!Scaled.Succeeded() && World.IsBodyValid(First), "collapsed scaled mesh replaced prior shape");
		} else {
			auto Bad = MakeDescription(false, 0, Source);
			Bad.Shape.Size.x = 0.0f;
			Started = Clock::now();
			const auto Malformed = World.UpdateBody(First, Bad);
			Rejections["invalidBoxUs"] = Microseconds(Started);
			Require(!Malformed.Succeeded() && World.IsBodyValid(First), "invalid box replaced prior shape");
		}
		Require(World.Raycast(Ray).HasHit(), "failed replacement changed queryable prior shape");
		const auto ResidencyBeforeRetire = World.GetCollisionResidency();
		std::vector<double> RetirementSamples;
		for (const auto Body : Bodies) {
			Started = Clock::now();
			Require(World.DestroyBody(Body).Succeeded(), "static body retirement failed");
			RetirementSamples.push_back(Microseconds(Started));
			Require(!World.IsBodyValid(Body), "retired body ID remained live");
		}
		const auto ResidencyRetired = World.GetCollisionResidency();
		Require(ResidencyRetired.ActiveResources == 0 && ResidencyRetired.ActiveConsumers == 0 &&
			ResidencyRetired.NativeMeshBytes == 0, "native mesh resource remained after final shape retirement");
		const auto Replacement = World.CreateBody(MakeDescription(MeshCase, 0, Source));
		Require(Replacement.IsValid() && Replacement != First && !World.IsBodyValid(First),
			"reused body slot did not invalidate stale identity");
		Require(World.DestroyBody(Replacement).Succeeded(), "replacement body retirement failed");
		const auto RssAfterRetire = ProcessRssBytes();

		return {{"kind", MeshCase ? "staticTriangleMesh" : "box"}, {"consumers", Count},
			{"activeStaticShapesBeforeRetire", Count}, {"activeStaticShapesAfterRetire", 0},
			{"retiredStaticShapes", Count}, {"staleBodyIdsInvalidated", Count},
			{"trianglesPerMesh", MeshCase ? 2 * Cells * Cells : 0},
			{"syntheticNeutralConstructionUs", NeutralConstructionUs}, {"firstShapeCreateUs", FirstCreateUs},
			{"additionalShapeCreateUs", Percentiles(std::move(AdditionalCreateSamples))},
			{"raycastUs", Percentiles(std::move(RaySamples))},
			{"kinematicMoverUs", Percentiles(std::move(MoverSamples))},
			{"settledStaticStepUs", Percentiles(std::move(StaticStepSamples))},
			{"dynamicContactStepUs", Percentiles(std::move(ContactStepSamples))},
			{"contactEvents", ContactEvents}, {"revisions", std::move(Revisions)},
			{"rejections", std::move(Rejections)}, {"beforeRetire", EncodeResidency(ResidencyBeforeRetire)},
			{"retirementPerShapeUs", Percentiles(std::move(RetirementSamples))},
			{"afterRetire", EncodeResidency(ResidencyRetired)},
			{"rssBytesBefore", RssBefore ? Json(*RssBefore) : Json(nullptr)},
			{"rssBytesAfterCreate", RssAfterCreate ? Json(*RssAfterCreate) : Json(nullptr)},
			{"rssBytesAfterRetire", RssAfterRetire ? Json(*RssAfterRetire) : Json(nullptr)}};
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
		const bool Stress = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--stress";
		const auto Counts = Quick ? std::vector<std::size_t>{1, 100} : std::vector<std::size_t>{1, 100, 1'000};
		const int Cells = Stress ? 32 : Quick ? 8 : 16;
		const int RevisionRounds = Quick ? 1 : 3;
		Json Results = Json::array();
		for (const auto Count : Counts) {
			Results.push_back(RunCase(false, Count, Cells, RevisionRounds));
			Results.push_back(RunCase(true, Count, Cells, RevisionRounds));
		}
		std::cout << Json{{"benchmark", "MeshCollisionNativeBoundary"}, {"quick", Quick},
			{"stress", Stress}, {"querySamples", QuerySamples},
			{"scope", "synthetic canonical triangles; native backend only; no AssetService import or DataModel"},
			{"memoryScope", "canonical position/index and Box3D mesh-data bytes exact; per-shape bytes unknown; RSS process-wide snapshots"},
			{"results", std::move(Results)}}.dump(2) << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Physics:MeshBenchmark] FAIL: " << Error.what() << '\n';
		return 1;
	}
}
