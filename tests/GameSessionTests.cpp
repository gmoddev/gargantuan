#include "../src/network/GameSessionTestAccess.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"
#include "../src/host/common/FarmPublicationEvidence.hpp"
#include "../src/host/common/FarmLifecycleEvidence.hpp"
#include "../src/host/server/PhysicalScaleQualification.hpp"
#include "PublicationLatencyFixture.hpp"
#include "JoinedCharacterFixture.hpp"
#include "FarmF1EvidenceFixture.hpp"
#include "FarmOrdinaryEvidenceFixture.hpp"
#include "FarmServiceTimingEvidenceFixture.hpp"
#include "GameplayScalingFixture.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/GameSessionProtocol.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/network/TerrainSession.hpp"
#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "../src/terrain/PhysicsStage.hpp"
#include "../src/terrain/RenderStage.hpp"
#endif
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/CharacterControlService.hpp"
#include "gargantuan/services/Players.hpp"

#include <algorithm>
#include <array>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(GARGANTUAN_GAMEPLAY_WORKLOAD_ALLOCATIONS)
namespace {
	thread_local std::uint64_t GameplayWorkloadAllocatedBytes = 0;
	thread_local std::uint64_t GameplayWorkloadAllocations = 0;
}

void *operator new(std::size_t Size) {
	if (auto *Value = std::malloc(std::max(Size, std::size_t{1}))) {
		gargantuan::runtime_detail::AddWorkCounter(GameplayWorkloadAllocatedBytes, Size);
		gargantuan::runtime_detail::AddWorkCounter(GameplayWorkloadAllocations, 1);
		return Value;
	}
	throw std::bad_alloc();
}

void operator delete(void *Value) noexcept { std::free(Value); }
void operator delete(void *Value, std::size_t) noexcept { std::free(Value); }
#endif

#if defined(GARGANTUAN_WITH_GNS)
namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t TerrainBodyCount(const WorldRoot &World) {
			const auto *Stage = World.GetTerrainPhysics();
			return Stage ? Stage->GetBodyCount() : 0;
		}
	};
}
#endif

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	std::uint64_t GameplayWorkloadAllocationBytes() noexcept {
#if defined(GARGANTUAN_GAMEPLAY_WORKLOAD_ALLOCATIONS)
		return GameplayWorkloadAllocatedBytes;
#else
		return 0;
#endif
	}
	std::uint64_t GameplayWorkloadAllocationCount() noexcept {
#if defined(GARGANTUAN_GAMEPLAY_WORKLOAD_ALLOCATIONS)
		return GameplayWorkloadAllocations;
#else
		return 0;
#endif
	}

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	void TestPhysicalScaleCounterDecoding() {
		using host::detail::DecodePhysicalScaleCounter;
		Check(DecodePhysicalScaleCounter(WireValue(32)) == 32,
			"host-authored integral scale count is accepted");
		Check(DecodePhysicalScaleCounter(WireValue(32.0)) == 32,
			"Luau-authored integral double scale count is accepted");
		for (const auto Value : {31.0, 33.0, 31.5,
			std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
			Check(!DecodePhysicalScaleCounter(WireValue(Value)).has_value() ||
				DecodePhysicalScaleCounter(WireValue(Value)) != 32,
				"incorrect or nonfinite scale count cannot satisfy the 32-peer gate");
		Check(!DecodePhysicalScaleCounter(std::nullopt).has_value() &&
			!DecodePhysicalScaleCounter(WireValue(std::string("32"))).has_value() &&
			!DecodePhysicalScaleCounter(WireValue(true)).has_value() &&
			!DecodePhysicalScaleCounter(WireValue(-1)).has_value(),
			"missing, wrong-type, and negative scale counts are rejected");

		using host::detail::HasPhysicalScaleTerminalConvergence;
		GameSessionMetrics Metrics;
		Metrics.ReadyPeers = 32;
		Metrics.StructuralFeedbackPeersObserved = 32;
		Metrics.NativeQueuedReliablePeersObserved = 32;
		Metrics.ReliableAdmission.AcceptedBytes = 774;
		Metrics.ReliableAdmission.VerifiedAttributedRetirement = 774;
		Check(HasPhysicalScaleTerminalConvergence(Metrics),
			"connected peers with exact grant retirement may conclude");
		Metrics.ReliableAdmission.VerifiedAttributedRetirement = 0;
		Metrics.ReliableAdmission.OutstandingBytes = 774;
		Check(!HasPhysicalScaleTerminalConvergence(Metrics),
			"completion observation cannot replace accepted-grant retirement");
		Metrics.ReliableAdmission.OutstandingBytes = 0;
		Metrics.ReliableAdmission.TerminalReleasedBytes = 774;
		Check(!HasPhysicalScaleTerminalConvergence(Metrics),
			"terminal release cannot masquerade as retirement");
		Metrics.ReliableAdmission.TerminalReleasedBytes = 0;
		Metrics.ReliableAdmission.VerifiedAttributedRetirement = 774;
		Metrics.ReadyPeers = 31;
		Check(!HasPhysicalScaleTerminalConvergence(Metrics),
			"a disconnected client cannot satisfy the terminal farm gate");
		Metrics.ReadyPeers = 32;
		Metrics.StructuralPendingEnters = 1;
		Check(!HasPhysicalScaleTerminalConvergence(Metrics),
			"unresolved structural pending work blocks terminal convergence");
		Metrics.StructuralPendingLeaves = 1;
		Metrics.NativeQueuedReliablePeersObserved = 31;
		Check(!HasPhysicalScaleTerminalConvergence(Metrics),
			"unobserved native queue feedback cannot masquerade as zero");
	}

	void TestRecoveryConvergenceBound() {
		using host::detail::RecoveryConvergenceBoundMicroseconds;
		std::array<std::uint64_t, 32> Work{};
		Check(RecoveryConvergenceBoundMicroseconds(Work) == 20'470'500,
			"zero retained structural work preserves the fixed service and credit/fairness envelope");
		Work[0] = 77;
		Check(RecoveryConvergenceBoundMicroseconds(Work) == 20'470'537,
			"a positive tiny complete message extends the structural deadline by exact peer service time");
		Work.fill(512 * 1024);
		Check(RecoveryConvergenceBoundMicroseconds(Work) == 20'720'500,
			"32 positive peer workloads use per-peer and pool capacity without a fixed cutoff");
		Work.fill(0);
		Work[0] = 40ULL * 1024 * 1024 * 1024;
		Check(RecoveryConvergenceBoundMicroseconds(Work) == 20'500'470'500ULL,
			"canonical hard peer envelope remains exact and finite");
		Work[0]++;
		Check(!RecoveryConvergenceBoundMicroseconds(Work),
			"a quote above the canonical peer retained-work bound is rejected");
		Check(!RecoveryConvergenceBoundMicroseconds(std::span(Work).first(31)),
			"an incomplete peer quote cannot establish a 32-peer recovery bound");
	}

	struct RetirementEvidenceCapture {
		using Record = runtime_detail::PublicationLatencyRecord;
		std::array<Record, 128> Records{};
		std::size_t Count = 0;
		bool Overflow = false;
		runtime_detail::PublicationLatencySink Sink{this, Selected, Append, Packet};
		runtime_detail::PublicationLatencySink *Previous = runtime_detail::ActivePublicationLatency;
		static bool Selected(void *, ConnectionId) noexcept { return true; }
		static void Append(void *Context, Record Value) noexcept {
			if (std::string_view(Value.Stage) != "RecipientRetired") return;
			auto &Self = *static_cast<RetirementEvidenceCapture *>(Context);
			if (Self.Count == Self.Records.size()) { Self.Overflow = true; return; }
			Self.Records[Self.Count++] = Value;
		}
		static void Packet(void *, const char *, ConnectionId, std::span<const std::byte>,
			std::uint64_t) noexcept {}
		RetirementEvidenceCapture() { runtime_detail::ActivePublicationLatency = &Sink; }
		~RetirementEvidenceCapture() { runtime_detail::ActivePublicationLatency = Previous; }
		RetirementEvidenceCapture(const RetirementEvidenceCapture &) = delete;
		RetirementEvidenceCapture &operator=(const RetirementEvidenceCapture &) = delete;
	};

	float HorizontalDistance(const glm::vec3 &Left, const glm::vec3 &Right) {
		return glm::distance(glm::vec2(Left.x, Left.z), glm::vec2(Right.x, Right.z));
	}

	void TestGroundedNetworkLocomotion() {
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Runtime(World, &Renderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		Runtime.ProcessService->Alive = true;
		Runtime.Step();
		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetParent(World);
		Character->ApplyRuntimeControllerFacts({0, -700, 0}, {0, 1, 0}, true);
		CharacterInputCommand Command;
		Command.DeltaSeconds = 1.0f / 60;
		for (int Index = 0; Index < 600; ++Index) {
			const auto Motion = Runtime.CharacterControl->EvaluateMovement(Command, *Character);
			Check(Motion.Velocity.y < 0 && Motion.Velocity.y > -4,
				"default grounded network movement cannot accumulate unencodable falling velocity");
			Character->ApplyRuntimeControllerFacts(Motion.Velocity, {0, 1, 0}, true);
		}
		Command.Flags = static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested);
		Check(Runtime.CharacterControl->EvaluateMovement(Command, *Character).Velocity.y == 50,
			"grounded velocity reset preserves default jump admission");
		Command.Flags = 0;
		Character->ApplyRuntimeControllerFacts({0, 20, 0}, {0, 1, 0}, true);
		Check(Runtime.CharacterControl->EvaluateMovement(Command, *Character).Velocity.y > 16,
			"grounded velocity reset preserves upward motion");
		Character->ApplyRuntimeControllerFacts({0, -20, 0}, {}, false);
		Check(Runtime.CharacterControl->EvaluateMovement(Command, *Character).Velocity.y < -20,
			"airborne default movement still applies gravity");
		Runtime.Destroy();
	}

	void TestPublicationLatencyBounds() {
		using Record = runtime_detail::PublicationLatencyRecord;
		std::vector<Record> DueRecords{
			{.Stage="FrameBegin", .Tick=10, .Nanoseconds=1'000'000'000},
			{.Stage="CharacterNextDue", .Connection={1,1}, .Object={9,2}, .Tick=10, .Due=22},
			{.Stage="FrameBegin", .Tick=22, .Nanoseconds=1'600'000'000},
			{.Stage="CharacterDue", .Connection={1,1}, .Object={9,2}, .Tick=22, .Due=22},
			{.Stage="StateBuilt", .Object={9,2}, .Tick=22, .Sequence=7, .Epoch=3, .Nanoseconds=1'601'000'000},
			{.Stage="CharacterProduced", .Connection={1,1}, .Object={9,2}, .Tick=22, .Sequence=7, .Due=22, .Nanoseconds=1'602'000'000},
			{.Stage="SchedulerAccepted", .Connection={1,1}, .Object={9,2}, .Tick=22, .Sequence=7, .Due=3, .Nanoseconds=1'603'000'000, .Kind=5},
			{.Stage="CharacterNextDue", .Connection={1,1}, .Object={9,2}, .Tick=22, .Due=34},
			{.Stage="ObserverState", .Connection={1,1}, .Object={9,2}, .Tick=22, .Sequence=7, .Due=3, .Nanoseconds=1'652'000'000}
		};
		auto Due = test::MeasureDueService(DueRecords, {{9,2}});
		Check(Due.Milliseconds == std::vector<double>{52} && Due.RootMilliseconds == Due.Milliseconds &&
			Due.Missing == 0 && Due.Unresolved == 0 && Due.Future == 1,
			"due service excludes twelve sparse cadence ticks and counts future work separately");
		DueRecords.back().Due = 4;
		Check(test::MeasureDueService(DueRecords).Missing == 1, "due service never joins a different control epoch");
		DueRecords.back().Due = 3; DueRecords.back().Object.Generation = 3;
		Check(test::MeasureDueService(DueRecords).Missing == 1, "due service never joins a recycled ObjectId");
		DueRecords.back().Object.Generation = 2; DueRecords.back().Connection.Generation = 2;
		Check(test::MeasureDueService(DueRecords).Missing == 1, "due service never joins a recycled recipient");
		DueRecords.resize(4);
		Check(test::MeasureDueService(DueRecords).Unresolved == 1, "scheduled due work without production is observable");
		DueRecords.push_back({.Stage="CharacterUnchanged", .Connection={1,1}, .Object={9,2}, .Tick=22, .Due=22});
		Due = test::MeasureDueService(DueRecords);
		Check(Due.Unresolved == 0 && Due.Suppressed == 1 && Due.Milliseconds.empty(),
			"legitimate unchanged suppression is not a missing delivery or latency sample");
		DueRecords.resize(3);
		DueRecords.push_back({.Stage="CharacterNextDue", .Connection={1,1}, .Object={9,2}, .Tick=22, .Due=34});
		Due = test::MeasureDueService(DueRecords);
		Check(Due.Unresolved == 0 && Due.Rescheduled == 1, "cadence refresh before discovery can supersede a forecast");
		DueRecords.insert(DueRecords.begin() + 3, {.Stage="CharacterDue", .Connection={1,1}, .Object={9,2}, .Tick=22, .Due=22});
		Due = test::MeasureDueService(DueRecords);
		Check(Due.Unresolved == 1 && Due.Rescheduled == 0, "rescheduling cannot hide confirmed due work without service");
		DueRecords.push_back({.Stage="RecipientRetired", .Connection={1,1}, .Object={9,3}});
		Check(test::MeasureDueService(DueRecords).Unresolved == 1, "stale retirement cannot erase another generation's due work");
		DueRecords.back().Object.Generation = 2;
		Due = test::MeasureDueService(DueRecords);
		Check(Due.Unresolved == 0 && Due.Retired == 1 && Due.RetiredDue == 1,
			"explicit materialization retirement is terminal accounting, not a delivered sample");
		test::ServiceBucket Bucket;
		Bucket.Add(8, 0, 64); Bucket.Add(8, .125, 64); Bucket.Add(10, .125, 64);
		Check(Bucket.Required == 18, "all-window accounting detects bursts that one-second averages hide");
		test::RecipientGapHistogram Histogram;
		Histogram.Add(1.25); Histogram.Add(20.75); Histogram.Add(5000);
		Check(Histogram.Count == 3 && Histogram.Upper(.50) == 21 && Histogram.Maximum == 5000 &&
			Histogram.Upper(1.0) == -1, "recipient histogram retains exact max and does not invent overflow percentiles");
		const auto Previous = runtime_detail::ActivePublicationLatency;
		{
			test::PublicationLatencyFixture Fixture(true, {1, 1}, {2, 1});
			Fixture.Begin("load");
			runtime_detail::RecordPublicationLatency({.Connection = {1, 2}});
			Check(Fixture.GetCount() == 0, "latency probe rejects a stale peer generation");
			for (std::size_t Index = 0; Index < 131075; ++Index)
				runtime_detail::RecordPublicationLatency({.Stage = "BoundTest", .Connection = {1, 1}});
			Check(Fixture.GetCount() == 131072 && Fixture.GetDropped() == 3, "latency diagnostic storage has a hard cap");
			Fixture.End();
		}
		Check(runtime_detail::ActivePublicationLatency == Previous && test::ActiveRecipientGaps == nullptr,
			"latency probe releases borrowed sink and histogram on scope teardown");
		{
			const std::array<ConnectionId, 2> Connections{{{2, 1}, {1, 1}}};
			test::JoinedCharacterFixture Joined(true, Connections);
			Joined.Begin("baseline");
			Joined.Mark("NotLoad", 1);
			Check(Joined.GetCount() == 0, "joined diagnostic records only its bounded load phase");
			Joined.Begin("load");
			runtime_detail::RecordPublicationLatency({.Connection = {1, 2}});
			Check(Joined.GetCount() == 1, "joined diagnostic rejects stale peer generation");
			CharacterStateFrame Frame{.ServerTick = 31, .FrameSequence = CharacterStateFrameSequence{1}, .StateCount = 1};
			Frame.States[0] = CharacterAuthoritativeState{.Character = {9, 2}, .ControlEpoch = CharacterControlEpoch{7},
				.StateSequence = RealtimeStateSequence{12}, .AuthoritativeTick = 31};
			const auto Bytes = EncodeCharacterMessage(Frame);
			Check(Bytes.has_value(), "joined trace fixture encodes a valid state");
			runtime_detail::RecordPublicationPacket("EgressSubmit", {1, 1}, *Bytes);
			const auto Records = Joined.GetRecords();
			Check(Records.size() == 3 && Records[1].Kind == 405 && Records[1].Bytes == Bytes->size() &&
				Records[2].Object == ObjectId{9, 2} && Records[2].Sequence == 12 && Records[2].Tick == 31 &&
				Records[2].Due == 7, "joined packet preserves full state identity and counts packet bytes only once");
			for (std::size_t Index = 0; Index < 1'048'574; ++Index) Joined.Mark("Bound", Index);
			Check(Joined.GetCount() == 1'048'576 && Joined.GetDropped() == 1, "joined diagnostic hard cap is observable");
			Joined.End();
		}
		Check(runtime_detail::ActivePublicationLatency == Previous, "joined diagnostic restores scoped sink");
	}

	void TestFarmPublicationEvidence() {
		const auto Root = std::filesystem::temp_directory_path() /
			("gargantuan-farm-publication-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directory(Root);
		const auto Previous = runtime_detail::ActivePublicationLatency;
		const auto ServerPath = Root / "publication-service.bin";
		{
			host::detail::FarmPublicationEvidence Evidence(true, "test-run", -1, 0, ServerPath, {}, 3);
			Evidence.MarkFrameBegin(10);
			runtime_detail::RecordPublicationLatency({.Stage = "CharacterDue", .Connection = {1, 1},
				.Object = {9, 2}, .Tick = 10, .Due = 10});
			runtime_detail::RecordPublicationLatency({.Stage = "RecipientRetired", .Connection = {4, 7},
				.Object = {9, 3}, .Tick = 11});
			Evidence.Dump();
			Check(Evidence.Valid() && Evidence.Count() == 3 && Evidence.BytesWritten() >= 240,
				"farm server trace seals bounded records after measured work");
		}
		Check(runtime_detail::ActivePublicationLatency == Previous, "farm server trace restores scoped sink");
		const auto LifetimeDirectory = Root / "lifetime";
		std::filesystem::create_directory(LifetimeDirectory);
		const auto LifetimePath = LifetimeDirectory / "publication-service.bin";
		{
			host::detail::FarmPublicationEvidence Evidence(true, "test-run", -1, 0, LifetimePath, {}, 2);
			Evidence.MarkFrameBegin(11);
			runtime_detail::RecordPublicationLatency({.Stage = "CharacterRelationshipRetired", .Connection = {4, 7},
				.Object = {9, 3}, .Tick = 11});
			Evidence.Dump();
			Check(Evidence.Valid() && Evidence.Count() == 2, "farm owner lifetime end stays in the existing fixed record budget");
		}
		{
			std::ifstream File(LifetimePath, std::ios::binary);
			const std::string Bytes(std::istreambuf_iterator<char>{File}, {});
			const auto HeaderEnd = Bytes.find('\n');
			Check(Bytes.starts_with("format=GargantuanFarmPublicationV2\t"), "new lifetime evidence explicitly identifies V2");
			if (HeaderEnd != std::string::npos && Bytes.size() == HeaderEnd + 1 + 2 * 80) {
				const auto Offset = HeaderEnd + 1 + 80;
				auto Read = [&](std::size_t Index, std::size_t Length) {
					std::uint64_t Value = 0;
					for (std::size_t Byte = 0; Byte < Length; ++Byte)
						Value |= static_cast<std::uint64_t>(static_cast<unsigned char>(Bytes[Offset + Index + Byte])) << (8 * Byte);
					return Value;
				};
				Check(Read(0, 2) == 29 && Read(2, 2) == 0 && Read(4, 4) == 4 && Read(8, 4) == 7 &&
					Read(12, 4) == 9 && Read(16, 4) == 3 && Read(20, 4) == 0 && Read(24, 8) != 0 &&
					Read(32, 8) == 11 && Read(40, 8) == 0 && Read(48, 8) == 0 && Read(56, 8) == 0 &&
					Read(64, 8) == 0 && Read(72, 8) == 0,
					"Stage29 preserves exact lifetime identity and tick without ordinary clock or packet-field remapping");
			} else Check(false, "fixed80-byte lifetime record is present without trailing or missing bytes");
		}
		const auto DirectDirectory = Root / "direct";
		std::filesystem::create_directory(DirectDirectory);
		const auto DirectPath = DirectDirectory / "publication-service.bin";
		{
			host::detail::FarmPublicationEvidence Evidence(true, "test-run", -1, 0, DirectPath, {}, 2);
			Evidence.MarkFrameBegin(12);
			runtime_detail::RecordPublicationLatency({.Stage = "CharacterDirectOffered", .Connection = {4, 7},
				.Object = {9, 3}, .Tick = 12, .Sequence = 17, .Due = 11, .Epoch = 23,
				.Operations = 1});
			Evidence.Dump();
			Check(Evidence.Valid() && Evidence.Count() == 2,
				"farm direct Character source marker seals after FrameBegin");
		}
		bool InvalidNameRejected = false;
		try {
			host::detail::FarmPublicationEvidence Evidence(true, "test-run", -1, 0,
				Root / "publication-service-direct.bin", {}, 2);
		} catch (const std::invalid_argument &) { InvalidNameRejected = true; }
		Check(InvalidNameRejected, "farm publication evidence requires its canonical file name");
		{
			std::ifstream File(DirectPath, std::ios::binary);
			const std::string Bytes(std::istreambuf_iterator<char>{File}, {});
			const auto HeaderEnd = Bytes.find('\n');
			if (HeaderEnd != std::string::npos && Bytes.size() >= HeaderEnd + 1 + 2 * 80) {
				const auto Offset = HeaderEnd + 1 + 80;
				auto Read32 = [&](std::size_t Index) {
					std::uint32_t Value = 0;
					for (std::size_t Byte = 0; Byte < 4; ++Byte)
						Value |= static_cast<std::uint32_t>(static_cast<unsigned char>(Bytes[Offset + Index + Byte])) << (8 * Byte);
					return Value;
				};
				auto Read64 = [&](std::size_t Index) {
					std::uint64_t Value = 0;
					for (std::size_t Byte = 0; Byte < 8; ++Byte)
						Value |= static_cast<std::uint64_t>(static_cast<unsigned char>(Bytes[Offset + Index + Byte])) << (8 * Byte);
					return Value;
				};
				Check(static_cast<unsigned char>(Bytes[Offset]) == 12 &&
					static_cast<unsigned char>(Bytes[Offset + 1]) == 0 &&
					static_cast<unsigned char>(Bytes[Offset + 2]) == 1 &&
					Read32(4) == 4 && Read32(8) == 7 && Read32(12) == 9 && Read32(16) == 3 &&
					Read64(32) == 12 && Read64(40) == 17 && Read64(48) == 11 && Read64(56) == 0 &&
					Read64(64) == 23 && Read64(72) == 0,
					"farm direct marker retains source, due, reliability and generation identity");
			} else Check(false, "farm direct marker is present in sealed binary trace");
		}
		{
			std::ifstream File(ServerPath, std::ios::binary);
			const std::string Bytes(std::istreambuf_iterator<char>{File}, {});
			const auto HeaderEnd = Bytes.find('\n');
			if (HeaderEnd != std::string::npos && Bytes.size() >= HeaderEnd + 1 + 3 * 80) {
				const auto Offset = HeaderEnd + 1 + 2 * 80;
				auto Read32 = [&](std::size_t Index) {
					std::uint32_t Value = 0;
					for (std::size_t Byte = 0; Byte < 4; ++Byte)
						Value |= static_cast<std::uint32_t>(static_cast<unsigned char>(Bytes[Offset + Index + Byte])) << (8 * Byte);
					return Value;
				};
				Check(static_cast<unsigned char>(Bytes[Offset]) == 11 &&
					Read32(4) == 4 && Read32(8) == 7 && Read32(12) == 9 && Read32(16) == 3 &&
					static_cast<unsigned char>(Bytes[Offset + 32]) == 11 &&
					static_cast<unsigned char>(Bytes[Offset + 40]) == 0,
					"farm retirement stage preserves full recipient/object generations and tick without state sequence");
			} else Check(false, "farm retirement record is present in the sealed binary trace");
		}
		const auto ClientPath = Root / "publication-service-0.bin";
		{
			host::detail::FarmPublicationEvidence Evidence(false, "test-run", 0, 17, ClientPath, {1, 1}, 2);
			runtime_detail::RecordPublicationLatency({.Stage = "CharacterRelationshipRetired", .Connection = {1, 1},
				.Object = {9, 2}, .Tick = 20});
			CharacterStateFrame Frame{.ServerTick = 31, .FrameSequence = CharacterStateFrameSequence{1}, .StateCount = 1};
			Frame.States[0] = CharacterAuthoritativeState{.Character = {9, 2},
				.ControlEpoch = CharacterControlEpoch{7}, .StateSequence = RealtimeStateSequence{12},
				.AuthoritativeTick = 31};
			const auto Bytes = EncodeCharacterMessage(Frame);
			Check(Bytes.has_value(), "farm client trace fixture encodes a valid state");
			if (Bytes) {
				runtime_detail::RecordPublicationPacket("ClientNativeReceive", {1, 2}, *Bytes);
				runtime_detail::RecordPublicationPacket("ClientNativeReceive", {1, 1}, *Bytes);
				runtime_detail::RecordPublicationPacket("ClientHandled", {1, 1}, *Bytes);
			}
			Evidence.Dump();
			Check(Evidence.Valid() && Evidence.Count() == 2,
				"farm client trace retains only its actual full-generation recipient");
		}
		Check(runtime_detail::ActivePublicationLatency == Previous, "farm client trace restores scoped sink");
		const auto OverflowPath = Root / "publication-service-1.bin";
		{
			host::detail::FarmPublicationEvidence Evidence(false, "test-run", 1, 18, OverflowPath, {1, 1}, 1);
			std::array<std::byte, 4> Invalid{std::byte{0x47}, std::byte{0x43}, std::byte{0x48}, std::byte{0x52}};
			runtime_detail::RecordPublicationPacket("ClientHandled", {1, 1}, Invalid);
			CharacterStateFrame Frame{.ServerTick = 31, .FrameSequence = CharacterStateFrameSequence{1}, .StateCount = 1};
			Frame.States[0] = CharacterAuthoritativeState{.Character = {9, 2},
				.ControlEpoch = CharacterControlEpoch{7}, .StateSequence = RealtimeStateSequence{12},
				.AuthoritativeTick = 31};
			if (const auto Bytes = EncodeCharacterMessage(Frame)) {
				runtime_detail::RecordPublicationPacket("ClientNativeReceive", {1, 1}, *Bytes);
				runtime_detail::RecordPublicationPacket("ClientHandled", {1, 1}, *Bytes);
			}
			Evidence.Dump();
			Check(!Evidence.Valid() && Evidence.Overflowed() && Evidence.Failures() == 1,
				"farm trace overflow and malformed GCHR invalidate the evidence");
		}
		Check(runtime_detail::ActivePublicationLatency == Previous, "failed farm trace restores scoped sink");
		const auto RpcDirectory = Root / "rpc";
		std::filesystem::create_directory(RpcDirectory);
		const auto RpcClientPath = RpcDirectory / "publication-service-0.bin";
		{
			host::detail::FarmPublicationEvidence Evidence(false, "test-run", 0, 17,
				RpcClientPath, {1, 1}, 3);
			runtime_detail::RecordPublicationLatency({.Stage = "RpcRequestStarted",
				.Connection = {1, 1}, .Object = {18, 2}, .Sequence = 7});
			runtime_detail::RecordPublicationLatency({.Stage = "RpcResponseReceived",
				.Connection = {1, 1}, .Object = {18, 2}, .Sequence = 7, .Bytes = 42});
			RemoteMessage Request{.Kind = RemoteMessageKind::Request, .Remote = {18, 2},
				.Request = RemoteRequestId{7}, .Deadline = std::chrono::milliseconds(1'000),
				.Arguments = {WireValue(std::string("recovery"))}};
			const auto Encoded = EncodeRemoteMessage(Request);
			Check(Encoded.has_value(), "farm RPC diagnostic fixture encodes a real request");
			if (Encoded) runtime_detail::RecordPublicationPacket("Handoff", {1, 1}, *Encoded);
			Evidence.Dump();
			Check(Evidence.Valid() && Evidence.Count() == 3,
				"farm RPC diagnostic retains only bounded request metadata");
		}
		{
			std::ifstream File(RpcClientPath, std::ios::binary);
			const std::string Bytes(std::istreambuf_iterator<char>{File}, {});
			const auto HeaderEnd = Bytes.find('\n');
			if (HeaderEnd != std::string::npos && Bytes.size() >= HeaderEnd + 1 + 3 * 80) {
				const auto Offset = HeaderEnd + 1 + 2 * 80;
				Check(static_cast<unsigned char>(Bytes[Offset]) == 16 &&
					static_cast<unsigned char>(Bytes[Offset + 12]) == 18 &&
					static_cast<unsigned char>(Bytes[Offset + 16]) == 2 &&
					static_cast<unsigned char>(Bytes[Offset + 40]) == 7,
					"farm RPC GNS-accept marker retains remote and request identity");
				Check(static_cast<unsigned char>(Bytes[HeaderEnd + 1 + 80]) == 22 &&
					static_cast<unsigned char>(Bytes[HeaderEnd + 1 + 80 + 20]) == 42,
					"farm RPC receive marker retains exact received bytes");
			} else Check(false, "farm RPC GNS-accept marker is present in sealed binary trace");
		}
		RemoteMessage NativeRequest{.Kind = RemoteMessageKind::Request, .Remote = {18, 2},
			.Request = RemoteRequestId{7}, .Deadline = std::chrono::milliseconds(1'000),
			.Arguments = {WireValue(std::string("recovery"))}};
		const auto NativeEncoded = EncodeRemoteMessage(NativeRequest);
		Check(NativeEncoded.has_value(), "native receive diagnostic fixture encodes a real RPC");
		if (NativeEncoded) {
			// Old aggregate initializers have no callback and must remain no-op.
			runtime_detail::PublicationLatencySink LegacySink{nullptr,
				+[](void *, ConnectionId) noexcept { return true; }};
			runtime_detail::ActivePublicationLatency = &LegacySink;
			Check(!runtime_detail::PublicationRpcNativeReceiveSelected({1, 1}, *NativeEncoded),
				"legacy diagnostic sink leaves optional native receive disabled");
			runtime_detail::RecordPublicationRpcNativeReceive({1, 1}, *NativeEncoded, 100, 101, 0, 0, 1);
			runtime_detail::ActivePublicationLatency = Previous;
			for (int Case = 0; Case < 14; ++Case) {
				const auto Directory = Root / ("native-" + std::to_string(Case));
				std::filesystem::create_directory(Directory);
				const auto Output = Directory / "publication-service-0.bin";
				auto Payload = *NativeEncoded;
				std::int64_t Age = 500'000, Number = 23;
				std::size_t Depth = 4096;
				ConnectionId Connection{1, 1};
				if (Case == 1) Payload[4] = std::byte{1}; // Protocol version.
				if (Case == 2) Payload[7] = std::byte{1}; // Reserved.
				if (Case == 3) for (std::size_t I = 12; I < 16; ++I) Payload[I] = std::byte{0};
				if (Case == 4) for (std::size_t I = 24; I < 32; ++I) Payload[I] = std::byte{0};
				if (Case == 5) Age = -1;
				if (Case == 6) Depth = host::detail::FarmPublicationEvidence::MaximumNativeReceiveEventDepth + 1;
				if (Case == 7) Number = 0;
				if (Case == 8) Connection.Generation = 2; // Not this client's lifetime.
				if (Case == 9) Payload[6] = std::byte{0}; // Event: not an RPC.
				if (Case == 10) Payload.resize(31); // Fixed-header selection requires 32 bytes.
				if (Case == 12) { Age = 0; Depth = 0; }
				{
					host::detail::FarmPublicationEvidence Evidence(false, "test-run", 0, 17, Output, {1, 1}, 1);
					const auto At = runtime_detail::PublicationLatencyNow();
					const auto After = Case == 13 ? At - 1 : At + 100;
					runtime_detail::RecordPublicationRpcNativeReceive(Connection, Payload,
						At, After, Age, Depth, Number);
					if (Case == 11) runtime_detail::RecordPublicationRpcNativeReceive(Connection, Payload,
						At + 100, After + 100, Age, Depth, Number);
					Evidence.Dump();
					if (Case == 0 || Case == 12)
						Check(Evidence.Valid() && Evidence.Count() == 1, "native dequeue retains exact RPC metadata including zero age/depth");
					else if (Case >= 8 && Case <= 10)
						Check(Evidence.Valid() && Evidence.Count() == 0, "native dequeue ignores other generations, Events and short headers");
					else if (Case == 11)
						Check(!Evidence.Valid() && Evidence.Overflowed() && Evidence.Count() == 1,
							"native dequeue overflow invalidates evidence without growing storage");
					else Check(!Evidence.Valid() && Evidence.Failures() == 1 && Evidence.Count() == 0,
						"invalid native dequeue protocol, identity, signed age, depth or message number invalidates evidence");
				}
				if (Case == 0) {
					std::ifstream File(Output, std::ios::binary);
					const std::string Bytes(std::istreambuf_iterator<char>{File}, {});
					const auto Offset = Bytes.find('\n') + 1;
					auto Read = [&](std::size_t Field, std::size_t Width) {
						std::uint64_t Value = 0;
						for (std::size_t I = 0; I < Width; ++I)
							Value |= static_cast<std::uint64_t>(static_cast<unsigned char>(Bytes[Offset + Field + I])) << (8 * I);
						return Value;
					};
					Check(Offset && Bytes.size() >= Offset + 80 && Read(0, 2) == 28 &&
						Read(4, 4) == 1 && Read(8, 4) == 1 && Read(12, 4) == 18 && Read(16, 4) == 2 &&
						Read(32, 8) == 500'000 && Read(40, 8) == 7 && Read(48, 8) == 4096 &&
						Read(56, 8) == 3 && Read(64, 8) == 23 && Read(72, 8) - Read(24, 8) == 100,
						"native dequeue binary stage preserves ready age, pre-enqueue depth, clock bracket and both identities");
				}
				std::filesystem::remove(Output);
				std::filesystem::remove(Directory);
			}
		}
		Check(runtime_detail::ActivePublicationLatency == Previous, "native receive fixture restores scoped callback");
		std::filesystem::remove(ServerPath);
		std::filesystem::remove(DirectPath);
		std::filesystem::remove(ClientPath);
		std::filesystem::remove(OverflowPath);
		std::filesystem::remove(RpcClientPath);
		std::filesystem::remove(RpcDirectory);
		std::filesystem::remove(DirectDirectory);
		std::filesystem::remove(LifetimePath);
		std::filesystem::remove(LifetimeDirectory);
		std::filesystem::remove(Root);
	}

	class HandoffTransport final : public IGameTransport {
	  public:
		explicit HandoffTransport(std::shared_ptr<IGameTransport> Value) : Inner(std::move(Value)) {}
		TransportOperationResult Start(const TransportStartConfiguration &Value) override { return Inner->Start(Value); }
		TransportOperationResult Stop(DisconnectInfo Value) override { return Inner->Stop(std::move(Value)); }
		TransportOperationResult Disconnect(ConnectionId Id, DisconnectInfo Value) override {
			return Inner->Disconnect(Id, std::move(Value));
		}
		TransportOperationResult Send(const NetworkMessageIntent &Message) override {
			const auto Terrain = DecodeTerrainFragment(Message.Payload());
			if ((HoldTerrain && Terrain) || (HoldTerrainReady && IsTerrainWindowReady(Message.Payload()))) {
				if (HoldTerrainReady && IsTerrainWindowReady(Message.Payload())) ++TerrainReadyDeferrals;
				++TerrainDeferrals;
				return {.Status = TransportOperationStatus::WouldBlock};
			}
			auto Result = Inner->Send(Message);
			if (Result.Succeeded() && TerrainObservedPeer && Message.Destination() != TerrainObservedPeer)
				if (const auto Offer = DecodeTerrainWindowOffer(Message.Payload())) TerrainObservedOffer = *Offer;
			if (Result.Succeeded() && Terrain) {
				TerrainBytes += Terrain->Bytes.size();
				if (TerrainObservedPeer == Message.Destination()) ObservedTerrainBytes += Terrain->Bytes.size();
				MaximumTerrainSlice = std::max(MaximumTerrainSlice, Terrain->Bytes.size());
			}
			if (Result.Succeeded() && Started != 0) {
				Bytes += Message.Payload().size();
				++Messages;
				if (const auto *Sample = runtime_detail::ActiveWorkSample) {
					const bool Character = Message.Traffic() == TrafficClass::RealtimeState;
					const bool Remote = Message.Traffic() == TrafficClass::ReliableApplication;
					const auto Phase = Character ? runtime_detail::WorkPhase::CharacterPublication : runtime_detail::WorkPhase::RemotePump;
					if ((Character || Remote) && (*Sample)[static_cast<std::size_t>(Phase)].Calls != 0) {
						const auto Index = static_cast<std::size_t>(Character ? runtime_detail::WorkProducer::Gchr
							: runtime_detail::WorkProducer::RemoteEventReliable);
						++Fresh[Index];
					}
				}
			}
			return Result;
		}
		std::size_t PollEvents(std::span<TransportEvent> Events) override { return Inner->PollEvents(Events); }
		std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Id) const override { return Inner->GetAvailableDatagramBytes(Id); }
		std::optional<NetworkStatistics> GetStatistics(ConnectionId Id) const override { return Inner->GetStatistics(Id); }
		void Begin() { Fresh.fill(0); Bytes = Messages = 0; Started = runtime_detail::WorkTimestamp(); }
		std::shared_ptr<IGameTransport> Inner;
		std::array<std::uint64_t, runtime_detail::WorkProducerNames.size()> Fresh{};
		std::uint64_t Started = 0, Bytes = 0, Messages = 0;
		std::size_t TerrainDeferrals = 0, TerrainBytes = 0, TerrainReadyDeferrals = 0, MaximumTerrainSlice = 0;
		std::optional<ConnectionId> TerrainObservedPeer;
		std::size_t ObservedTerrainBytes = 0;
		std::optional<TerrainWindowOffer> TerrainObservedOffer;
		bool HoldTerrain = false, HoldTerrainReady = false;
	};

	GameSessionConfiguration Configuration(GameSessionRole Role, std::string Endpoint = "game-session") {
		return {
			.Role = Role,
			.Endpoint = {std::move(Endpoint), 27020},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 120,
			.ClientNonce = Role == GameSessionRole::Client ? 0x12345678abcdefull : 0,
			.AllowInsecureDevelopmentNetwork = true,
		};
	}

	class ManualGameAdmissionProvider final : public IGameAdmissionProvider {
	  public:
		bool Begin(GameAdmissionRequest Request, GameAdmissionCompletion Completion) override {
			if (!StartAllowed) return false;
			const bool Repeated = RejectRepeatedProofs && !SeenProofs.insert(Request.Proof).second;
			Requests.push_back(Request);
			if (Repeated) {
				Completion(std::nullopt);
				return true;
			}
			Completions.push_back(std::move(Completion));
			return true;
		}
		void Release(std::uint64_t RequestId) noexcept override {
			if (ReleaseCount < Released.size()) Released[ReleaseCount++] = RequestId;
		}
		void Complete(std::size_t Index, std::optional<PlayerIdentity> Identity) {
			if (Index < Completions.size()) Completions[Index](std::move(Identity));
		}
		bool StartAllowed = true;
		bool RejectRepeatedProofs = false;
		std::set<std::array<std::byte, GameAdmissionProofBytes>> SeenProofs;
		std::vector<GameAdmissionRequest> Requests;
		std::vector<GameAdmissionCompletion> Completions;
		std::array<std::uint64_t, 16> Released{};
		std::size_t ReleaseCount = 0;
	};

	class UntrustedServerTransport final : public IGameTransport {
	  public:
		explicit UntrustedServerTransport(std::shared_ptr<IGameTransport> Value) : Inner(std::move(Value)) {}
		TransportOperationResult Start(const TransportStartConfiguration &Value) override { return Inner->Start(Value); }
		TransportOperationResult Stop(DisconnectInfo Value) override { return Inner->Stop(std::move(Value)); }
		TransportOperationResult Disconnect(ConnectionId Id, DisconnectInfo Value) override {
			return Inner->Disconnect(Id, std::move(Value));
		}
		TransportOperationResult Send(const NetworkMessageIntent &Value) override {
			++SentMessages;
			return Inner->Send(Value);
		}
		std::size_t PollEvents(std::span<TransportEvent> Output) override { return Inner->PollEvents(Output); }
		std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Id) const override {
			return Inner->GetAvailableDatagramBytes(Id);
		}
		std::optional<NetworkStatistics> GetStatistics(ConnectionId Id) const override {
			return Inner->GetStatistics(Id);
		}
		std::size_t SentMessages = 0;
	  private:
		std::shared_ptr<IGameTransport> Inner;
	};

	std::array<std::byte, GameAdmissionProofBytes> TestAdmissionProof() {
		std::array<std::byte, GameAdmissionProofBytes> Proof{};
		for (std::size_t Index = 0; Index < Proof.size(); ++Index)
			Proof[Index] = static_cast<std::byte>(Index + 1);
		return Proof;
	}

	GameSessionConfiguration AuthenticatedConfiguration(GameSessionRole Role,
		const std::shared_ptr<ManualGameAdmissionProvider> &Provider, std::uint64_t Timeout = 120) {
		auto Result = Configuration(Role, "127.0.0.1");
		Result.AllowInsecureDevelopmentNetwork = false;
		Result.AdmissionMode = GameSessionAdmissionMode::Authenticated;
		Result.HandshakeTimeoutTicks = Timeout;
		if (Role == GameSessionRole::Server) Result.AdmissionProvider = Provider;
		else Result.AdmissionProof = TestAdmissionProof();
		return Result;
	}

	void Advance(const std::shared_ptr<SimulatedNetwork> &Network, GameSession &Server,
		GameSession &Client, std::uint64_t Tick, runtime_detail::WorkSample *ServerWork = nullptr);

	struct AdmissionFixture {
		std::shared_ptr<SimulatedNetwork> Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		std::shared_ptr<DataModel> ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer{Vector2(64, 64)};
		Engine ServerRuntime{ServerWorld, &ServerRenderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}};
		std::shared_ptr<ManualGameAdmissionProvider> Provider;
		GameSession Server;
		GameSession Client;
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		std::uint64_t Tick = 1;

		explicit AdmissionFixture(std::uint64_t Timeout = 120,
			std::shared_ptr<ManualGameAdmissionProvider> SharedProvider = {})
			: Provider(SharedProvider ? std::move(SharedProvider) : std::make_shared<ManualGameAdmissionProvider>()),
			  Server(Network->CreateTransport(), AuthenticatedConfiguration(GameSessionRole::Server, Provider, Timeout),
				&ServerRuntime),
			  Client(Network->CreateTransport(), AuthenticatedConfiguration(GameSessionRole::Client, Provider, Timeout)) {
			ServerRuntime.ProcessService->Alive = true;
			Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "authenticated fixture sessions start");
		}
		~AdmissionFixture() {
			Client.Stop();
			Server.Stop();
			if (ClientRuntime) ClientRuntime->Destroy();
			ServerRuntime.Destroy();
		}
		void AdvanceTick(bool AttachClient = false) {
			Advance(Network, Server, Client, Tick++);
			if (AttachClient && !ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
				ClientRuntime = std::make_unique<Engine>(Client.GetClientDataModel(), ClientRenderer.get(), nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "authenticated client attaches after trusted bootstrap");
			}
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
		}
		void AwaitRequest() {
			for (int Index = 0; Index < 20 && Provider->Requests.empty(); ++Index) AdvanceTick();
			Check(Provider->Requests.size() == 1, "v2 hello reaches exactly one trusted provider request");
			if (Provider->Requests.empty()) throw std::runtime_error("authenticated fixture received no provider request");
		}
	};

	void Advance(
		const std::shared_ptr<SimulatedNetwork> &Network, GameSession &Server, GameSession &Client, std::uint64_t Tick,
		runtime_detail::WorkSample *ServerWork
	) {
		(void)Network->Advance(2ms);
		Network->Pump();
		(void)Server.Poll();
		(void)Client.Poll();
		{
			runtime_detail::WorkCapture Capture(ServerWork);
			Server.Step(Tick);
		}
		Client.Step(Tick);
		(void)Network->Advance(20ms);
	}

	void TestProtocolBounds() {
		const auto Limits = GameSessionConfiguration::DefaultLimits();
		GameSessionMessage Message = GameSessionClientHello{77, Limits};
		auto Encoded = EncodeGameSessionMessage(Message);
		Check(
			Encoded.has_value() && Encoded->size() <= MaximumGameSessionFrameBytes,
			"GSES client hello has a bounded representation"
		);
		Check(Encoded && DecodeGameSessionMessage(*Encoded).has_value(), "GSES client hello round-trips");
		if (!Encoded) return;
		Check((*Encoded)[4] == std::byte{GameSessionProtocolVersion}, "ordinary GSES retains version 2");
		for (const bool Managed : {false, true}) {
			GameSessionClientHello Spatial{.Nonce=77, .AdvertisedLimits=Limits,
				.RuntimeDistributionIdentity=Managed ? std::string(64, 'a') : std::string{}, .SpatialContent=true};
			auto Hello = EncodeGameSessionMessage(Spatial);
			Check(Hello && (*Hello)[4] == (Managed ? std::byte{18} : std::byte{17}) && DecodeGameSessionMessage(*Hello),
				"spatial GSES negotiates the controller checkpoint and rectangular field profile");
			if (!Hello) continue;
			for (const auto Version : {13, 15}) {
				(*Hello)[4] = static_cast<std::byte>(Version + (Managed ? 1 : 0));
				Check(!DecodeGameSessionMessage(*Hello), "older spatial peers require a matching profile");
			}
		}
		for (std::size_t Size = 0; Size < Encoded->size(); ++Size)
			Check(
				!DecodeGameSessionMessage(std::span(*Encoded).first(Size)),
				"every truncated GSES client hello is rejected"
			);
		auto Trailing = *Encoded;
		Trailing.push_back(std::byte{0});
		Check(!DecodeGameSessionMessage(Trailing), "GSES rejects trailing bytes");
		auto WrongVersion = *Encoded;
		WrongVersion[4] = std::byte{0xff};
		Check(!DecodeGameSessionMessage(WrongVersion), "GSES rejects an unsupported protocol version");
		auto LegacyHello = *Encoded;
		LegacyHello[4] = std::byte{1};
		const auto Legacy = DecodeGameSessionMessage(LegacyHello);
		Check(!Legacy && Legacy.error().Code == SerializationErrorCode::UnsupportedVersion,
			"GSES rejects a pre-structured client during handshake");
		auto WrongMagic = *Encoded;
		WrongMagic[0] = std::byte{0};
		Check(!DecodeGameSessionMessage(WrongMagic), "GSES rejects an invalid protocol magic");
		auto UnknownKind = *Encoded;
		UnknownKind[6] = std::byte{0xff};
		Check(!DecodeGameSessionMessage(UnknownKind), "GSES rejects an unknown message kind");
		auto ReservedHeader = *Encoded;
		ReservedHeader[7] = std::byte{1};
		Check(!DecodeGameSessionMessage(ReservedHeader), "GSES rejects non-zero reserved header data");
		auto Oversized = *Encoded;
		Oversized.resize(MaximumGameSessionFrameBytes + 1);
		Check(!DecodeGameSessionMessage(Oversized), "GSES rejects an oversized bootstrap frame before decoding");
		auto Accepted = EncodeGameSessionMessage(
			GameSessionServerAccepted{
				.Nonce = 77,
				.SessionEpoch = 1,
				.Replication = ReplicationEpoch(1),
				.Player = {1, 1},
				.PlayerId = 1,
				.Identity = SessionIdentityKind::DevelopmentLocal,
				.NegotiatedLimits = Limits,
			}
		);
		Check(Accepted.has_value(), "bounded GSES acceptance encodes");
		if (Accepted) {
			(*Accepted)[44] = std::byte{0xff};
			Check(!DecodeGameSessionMessage(*Accepted), "GSES rejects an unknown session identity kind");
		}
		const std::string Pair(64, 'a');
		auto Managed = EncodeGameSessionMessage(GameSessionClientHello{77, Limits, {}, Pair});
		Check(Managed && (*Managed)[4] == std::byte{GameSessionDistributionProtocolVersion},
			"Managed GSES retains version 3 for the pair identity");
		if (Managed) {
			const auto Decoded = DecodeGameSessionMessage(*Managed);
			Check(Decoded && std::get<GameSessionClientHello>(*Decoded).RuntimeDistributionIdentity == Pair,
				"Managed GSES preserves the exact pair identity");
			auto Truncated = *Managed;
			Truncated.pop_back();
			Check(!DecodeGameSessionMessage(Truncated), "Managed GSES rejects an incomplete pair identity");
			(*Managed)[4] = std::byte{GameSessionProtocolVersion};
			Check(!DecodeGameSessionMessage(*Managed), "Legacy GSES cannot consume a managed pair identity");
		}
		Check(!EncodeGameSessionMessage(GameSessionClientHello{77, Limits, {}, std::string(64, 'z')}),
			"GSES rejects a malformed native pair configuration");

		AssetContentId Binding;
		Binding.Bytes.front() = 1;
		const auto Terrain = EncodeGameSessionMessage(GameSessionClientHello{77, Limits, Binding});
		const auto TerrainReady = EncodeGameSessionMessage(GameSessionClientReady{1, ReplicationEpoch(1), {1, 1}, Binding});
		Check(Terrain && (*Terrain)[4] == std::byte{GameSessionTerrainProtocolVersion},
			"Terrain selects its own GSES version");
		if (Terrain) {
			const auto Decoded = DecodeGameSessionMessage(*Terrain);
			Check(Decoded && std::get<GameSessionClientHello>(*Decoded).TerrainBinding == Binding,
				"Terrain GSES preserves the exact terrain binding");
			auto MissingBinding = *Terrain;
			MissingBinding[MissingBinding.size() - Binding.Bytes.size()] = std::byte{0};
			Check(!DecodeGameSessionMessage(MissingBinding), "Terrain GSES rejects an empty binding");
			auto WrongTerrainVersion = *Terrain;
			WrongTerrainVersion[4] = std::byte{GameSessionDistributionProtocolVersion};
			Check(!DecodeGameSessionMessage(WrongTerrainVersion), "Managed GSES cannot consume a terrain binding");
		}
		Check(TerrainReady && (*TerrainReady)[4] == std::byte{GameSessionTerrainProtocolVersion},
			"Terrain readiness uses the same terrain-specific protocol");
		if (TerrainReady) {
			const auto Decoded = DecodeGameSessionMessage(*TerrainReady);
			Check(Decoded && std::get<GameSessionClientReady>(*Decoded).TerrainBinding == Binding,
				"Terrain readiness carries the same exact binding");
		}
		Check(!EncodeGameSessionMessage(GameSessionClientHello{77, Limits, Binding, Pair}),
			"GSES rejects mixing terrain and managed package admission");
		auto AuthenticatedHello = EncodeGameSessionMessage(GameSessionAuthenticatedClientHello{
			77, Limits, TestAdmissionProof()});
		Check(AuthenticatedHello && AuthenticatedHello->size() <= MaximumGameSessionFrameBytes,
			"Hosted proof has a bounded representation");
		if (AuthenticatedHello) {
			auto Decoded = DecodeGameSessionMessage(*AuthenticatedHello);
			const auto *Hello = Decoded ? std::get_if<GameSessionAuthenticatedClientHello>(&*Decoded) : nullptr;
			Check(Hello && Hello->Proof == TestAdmissionProof(), "Hosted proof round-trips without reinterpretation");
			for (std::size_t Size = 0; Size < AuthenticatedHello->size(); ++Size)
				Check(!DecodeGameSessionMessage(std::span(*AuthenticatedHello).first(Size)),
					"every truncated hosted proof is rejected");
			auto WrongVersionHello = *AuthenticatedHello;
			WrongVersionHello[4] = std::byte{1};
			Check(!DecodeGameSessionMessage(WrongVersionHello), "Hosted proof cannot be interpreted as v1 hello");
		}
		Check(!EncodeGameSessionMessage(GameSessionServerAccepted{
			.Nonce = 77, .SessionEpoch = 1, .Replication = ReplicationEpoch(1), .Player = {1, 1},
			.PlayerId = 1, .Identity = SessionIdentityKind::DevelopmentLocal, .NegotiatedLimits = Limits,
			.Version = AuthenticatedGameSessionProtocolVersion}),
			"v2 acceptance cannot claim DevelopmentLocal identity");
		Check(EncodeGameSessionMessage(GameSessionServerAccepted{
			.Nonce = 77, .SessionEpoch = 1, .Replication = ReplicationEpoch(1), .Player = {1, 1},
			.PlayerId = 1, .Identity = SessionIdentityKind::Authenticated, .NegotiatedLimits = Limits,
			.Version = AuthenticatedGameSessionProtocolVersion}).has_value(),
			"v2 acceptance requires authenticated identity kind");
	}

#if defined(GARGANTUAN_WITH_GNS)
	constexpr unsigned DefaultTerrainChurnCycles = 24;
	constexpr unsigned MaximumTerrainChurnCycles = 120;

	struct TerrainChurnResources {
		std::size_t Entries, Pages, FieldBytes, Meshes, RenderBytes, Bodies;
		bool operator==(const TerrainChurnResources &) const = default;
	};

	TerrainChurnResources ReadTerrainChurnResources(const Engine &Runtime) {
		const auto Stats = Runtime.Workspace->GetTerrain()->GetField().GetStats();
		const auto Render = Runtime.WorldRoot->GetTerrainRenderState();
		return {Stats.Entries, Stats.MaterializedPages, Stats.OwnedBytes, Render ? Render->Chunks.size() : 0,
			Render ? Render->Bytes : 0, WorldRootTestAccess::TerrainBodyCount(*Runtime.WorldRoot)};
	}

	WorldRaycastResult CastTerrainChurnRay(Engine &Runtime, float X) {
		RaycastParams Params;
		Params.IncludeTerrain = true;
		Params.FilterType = Enums::RaycastFilterType::Include;
		Params.FilterDescendantsInstances.Values.emplace_back(Runtime.Workspace->GetTerrain());
		return Runtime.Workspace->ResolveRaycast({X, 12, 12}, {0, -12, 0}, Params);
	}

	void RunTerrainChurn(Engine &Authority, Engine &Near, Engine &Distant, Character &NearActor,
		Character &DistantActor, const GameSession &Server, const GameSession &Client,
		HandoffTransport &Transport, ConnectionId DistantPeer, unsigned Cycles, const std::function<void()> &Exchange) {
		const auto LocalActor = [](Engine &Runtime) {
			const auto Owner = Runtime.Players->GetLocalPlayer().value_or(nullptr);
			return Owner ? Owner->GetCharacter().value_or(nullptr) : nullptr;
		};
		const auto NearReplica = LocalActor(Near);
		const auto DistantReplica = LocalActor(Distant);
		if (!NearReplica || !DistantReplica) {
			Check(false, "terrain churn needs both native replica actors");
			return;
		}
		// Keep both probes over authored support across ordinary server steps.
		NearActor.SetPosition({76.f, 8.8f, 12.f});
		NearReplica->SetPosition({76.f, 8.8f, 12.f});
		DistantActor.SetPosition({140.f, 9.f, 12.f});
		DistantReplica->SetPosition({140.f, 9.f, 12.f});

		const auto BothWindowsReleased = [&] {
			return CastTerrainChurnRay(Near, 108.f).Succeeded() &&
				NearActor.AdmitMotion(*Authority.WorldRoot, {}).Succeeded() &&
				NearReplica->AdmitMotion(*Near.WorldRoot, {}).Succeeded() &&
				DistantReplica->AdmitMotion(*Distant.WorldRoot, {}).Succeeded();
		};
		for (int Step = 0; Step < 150 && !BothWindowsReleased(); ++Step) {
			Authority.Step();
			Exchange();
		}
		if (!BothWindowsReleased()) {
			Check(false, "terrain churn begins only after both existing windows are acknowledged");
			return;
		}

		const auto NearBaseline = ReadTerrainChurnResources(Near);
		const auto DistantBaseline = ReadTerrainChurnResources(Distant);
		auto NearPeak = NearBaseline;
		const auto BeforeBytes = Transport.TerrainBytes;
		Transport.TerrainObservedPeer = DistantPeer;
		Transport.ObservedTerrainBytes = 0;
		const auto Terrain = Authority.Workspace->GetTerrain();
		const auto ReplicaTerrain = Near.Workspace->GetTerrain();
		const auto InitialRevision = Terrain->GetField().GetRevision();
		const auto Started = std::chrono::steady_clock::now();
		constexpr std::size_t MaximumBytesPerEdit = 64 * 1024;
		const auto MaximumTotalBytes = std::size_t{Cycles} * (2 * 1024 * 1024) / DefaultTerrainChurnCycles;
		std::size_t MaximumEditBytes = 0, Edits = 0, DistantMoves = 0, StencilSamples = 0;
		float PreviousHeight = 0.f;
		for (unsigned Index = 0; Index < Cycles * 3; ++Index) {
			const auto Phase = Index % 3;
			const auto Material = Phase == 2 ? 0u : 1000u + (Index / 3) * 2 + Phase;
			const std::string Argument = ", " + std::to_string(Material) + ")";
			const auto Command = Phase == 0
				? "Terrain:FillBall(Vector3.new(58.4, 4.4, 12.4), 2.15" + Argument
				: Phase == 1 ? "Terrain:PaintBall(Vector3.new(58.4, 4.4, 12.4), 2.8" + Argument
				: "Terrain:RemoveBall(Vector3.new(58.4, 4.4, 12.4), 3.8)";
			const auto Revision = Terrain->GetField().GetRevision();
			const auto Sent = Transport.TerrainBytes;
			auto ScriptValue = std::make_shared<Script>();
			ScriptValue->SetRunContext(Enums::RunContext::Server);
			ScriptValue->SetSource("local Terrain = game:GetService(\"Workspace\").Terrain\n" + Command);
			ScriptValue->SetParent(Authority.DataModel);
			for (int Step = 0; Step < 180 && Terrain->GetField().GetRevision() == Revision; ++Step) {
				Authority.Step();
				if (Terrain->GetField().GetRevision() == Revision) Exchange();
			}
			ScriptValue->Destroy();

			const CharacterMotionRequest Motion{.Translation = {Index % 2 ? -0.25f : 0.25f, 0.f, 0.f}};
			const auto Move = [&](Character &Actor, Engine &Runtime) {
				const auto Start = Actor.GetPosition();
				return Actor.AdmitMotion(*Runtime.WorldRoot, Motion).Succeeded() && Actor.GetPosition().x != Start.x;
			};
			const bool AuthorityMoved = Move(DistantActor, Authority);
			const bool ReplicaMoved = Move(*DistantReplica, Distant);
			const bool BothMoved = AuthorityMoved && ReplicaMoved;
			DistantMoves += BothMoved;
			const auto Center = Terrain->GetField().Read({58, 4, 12});
			const auto SourceRay = CastTerrainChurnRay(Authority, 58.f);
			const bool SourcePublished = Terrain->GetField().GetRevision() == Revision + 1 &&
				Center && Center->Material == Material && (Center->Density < 0) == (Material != 0) &&
				SourceRay.Succeeded() && SourceRay.HasHit() == (Material != 0);
			const bool NearFenced = !NearActor.AdmitMotion(*Authority.WorldRoot, Motion).Succeeded();
			bool NearReleased = false, ReplicaReleased = false;
			if (SourcePublished) {
				for (int Step = 0; Step < 250 && (!NearReleased || !ReplicaReleased); ++Step) {
					Exchange();
					if (ReplicaTerrain->GetField().Read({58, 4, 12}) != Center) continue;
					if (!NearReleased) NearReleased = NearActor.AdmitMotion(*Authority.WorldRoot, Motion).Succeeded();
					if (!ReplicaReleased) ReplicaReleased = NearReplica->AdmitMotion(*Near.WorldRoot, Motion).Succeeded();
				}
			}
			bool ExactSamples = true;
			for (std::int64_t Z = 11; Z <= 13; ++Z)
				for (std::int64_t Y = 3; Y <= 5; ++Y)
					for (std::int64_t X = 57; X <= 59; ++X) {
						const auto A = Terrain->GetField().Read({X, Y, Z});
						const auto B = ReplicaTerrain->GetField().Read({X, Y, Z});
						const bool Match = A.has_value() && A == B;
						ExactSamples &= Match;
						StencilSamples += Match;
					}
			const auto NearRay = CastTerrainChurnRay(Near, 58.f);
			const auto DistantRay = CastTerrainChurnRay(Distant, 140.f);
			const bool ExactRays = NearRay.Succeeded() && NearRay.HasHit() == SourceRay.HasHit() &&
				(!SourceRay.HasHit() || (NearRay.GetHitInstance() == ReplicaTerrain &&
					glm::length(NearRay.Position - SourceRay.Position) < 1e-4f &&
					glm::length(NearRay.Normal - SourceRay.Normal) < 1e-4f)) &&
				(Phase != 1 || std::abs(PreviousHeight - SourceRay.Position.y) < 1e-4f) &&
				DistantRay.Succeeded() && DistantRay.GetHitInstance() == Distant.Workspace->GetTerrain();
			PreviousHeight = SourceRay.Position.y;
			const auto Current = ReadTerrainChurnResources(Near);
			NearPeak = {std::max(NearPeak.Entries, Current.Entries), std::max(NearPeak.Pages, Current.Pages),
				std::max(NearPeak.FieldBytes, Current.FieldBytes), std::max(NearPeak.Meshes, Current.Meshes),
				std::max(NearPeak.RenderBytes, Current.RenderBytes), std::max(NearPeak.Bodies, Current.Bodies)};
			MaximumEditBytes = std::max(MaximumEditBytes, Transport.TerrainBytes - Sent);
			const bool Bounded = Current.Entries <= TerrainWindowChunkCount && Current.Pages <= NearBaseline.Pages + 2 &&
				Current.Meshes <= NearBaseline.Meshes + 4 && Current.Bodies <= NearBaseline.Bodies + 4 &&
				Current.FieldBytes <= NearBaseline.FieldBytes + 1024 * 1024 &&
				Current.RenderBytes <= NearBaseline.RenderBytes + 1024 * 1024 &&
				ReadTerrainChurnResources(Distant) == DistantBaseline && Transport.ObservedTerrainBytes == 0 &&
				MaximumEditBytes <= MaximumBytesPerEdit && Transport.TerrainBytes - BeforeBytes <= MaximumTotalBytes &&
				Server.GetMetrics().ReadyPeers == 2;
			if (!(SourcePublished && NearFenced && NearReleased && ReplicaReleased && BothMoved &&
				ExactSamples && ExactRays && Bounded)) {
				const auto ReplicaCenter = ReplicaTerrain->GetField().Read({58, 4, 12});
				std::cerr << "[Terrain:Network] churn failed operation=" << Index + 1 << " phase=" << Phase
					<< " source=" << SourcePublished << " old_fence=" << NearFenced << " release=" << NearReleased
					<< " replica_release=" << ReplicaReleased << " distant_moves=" << BothMoved
					<< " samples=" << ExactSamples << " rays=" << ExactRays << " bounds=" << Bounded
					<< " material=" << Material << " replica_material=" << (ReplicaCenter ? ReplicaCenter->Material : 0)
					<< " near_x=" << NearActor.GetPosition().x << " near_y=" << NearActor.GetPosition().y
					<< " sent_this_edit=" << Transport.TerrainBytes - Sent << " client=" << Client.GetFailure() << '\n';
				if (const auto &Offer = Transport.TerrainObservedOffer)
					std::cerr << "[Terrain:Network] churn last offer revision=" << Offer->Identity.FieldRevision
						<< " center=" << Offer->Center.X << ',' << Offer->Center.Y << ',' << Offer->Center.Z
						<< " manifest_bytes=" << Offer->ManifestByteCount << " leaf_bytes=" << Offer->LeafByteCount << '\n';
				break;
			}
			++Edits;
		}
		Transport.TerrainObservedPeer.reset();
		const auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Started);
		std::cout << "[Terrain:Network] churn cycles=" << Cycles << " operations=" << Edits
			<< " first_revision=" << InitialRevision + 1
			<< " last_revision=" << Terrain->GetField().GetRevision() << " elapsed_ms=" << Elapsed.count()
			<< " exact_samples=" << StencilSamples << " distant_moves_per_peer=" << DistantMoves
			<< " fragment_payload_bytes=" << Transport.TerrainBytes - BeforeBytes << " payload_limit_bytes=" << MaximumTotalBytes
			<< " max_edit_fragment_bytes=" << MaximumEditBytes << " distant_fragment_bytes=" << Transport.ObservedTerrainBytes
			<< " peak_near_entries=" << NearPeak.Entries << " peak_near_pages=" << NearPeak.Pages
			<< " peak_near_field_bytes=" << NearPeak.FieldBytes << " peak_near_meshes=" << NearPeak.Meshes
			<< " peak_near_render_bytes=" << NearPeak.RenderBytes << " peak_near_bodies=" << NearPeak.Bodies
			<< " distant_entries=" << DistantBaseline.Entries << " distant_pages=" << DistantBaseline.Pages
			<< " distant_meshes=" << DistantBaseline.Meshes << " distant_bodies=" << DistantBaseline.Bodies << '\n';
		Check(Edits == Cycles * 3 && Terrain->GetField().GetRevision() == InitialRevision + Edits,
			"all opt-in terrain churn operations committed, rolled, and retained bounded regional resources");
	}

	void TestAuthenticatedAdmission() {
		const auto Provider = std::make_shared<ManualGameAdmissionProvider>();
		Provider->RejectRepeatedProofs = true;
		auto NonLoopback = AuthenticatedConfiguration(GameSessionRole::Server, Provider);
		NonLoopback.Endpoint = {"10.0.0.2", 27020};
		Check(!NonLoopback.IsValid(), "a ticket provider alone cannot authorize an unauthenticated remote transport");
		NonLoopback.RequireServerIdentity = true;
		Check(NonLoopback.IsValid(), "remote authenticated server requires native listener identity");
		NonLoopback.RequireServerIdentity = false;
		NonLoopback.AllowInsecureDevelopmentNetwork = true;
		Check(!NonLoopback.IsValid(), "authenticated hosting cannot use the development escape hatch");
		NonLoopback.AdmissionProvider.reset();
		Check(!NonLoopback.IsValid(), "authenticated server cannot start without its provider");
		auto RemoteClient = AuthenticatedConfiguration(GameSessionRole::Client, Provider);
		RemoteClient.Endpoint = {"10.0.0.2", 27020};
		Check(!RemoteClient.IsValid(), "a valid ticket does not establish remote server identity");
		TransportServerPublicKey ServerKey{};
		ServerKey[0] = std::byte{1};
		RemoteClient.ExpectedServerPublicKey = ServerKey;
		Check(RemoteClient.IsValid(), "remote authenticated client requires an exact nonzero server key");
		RemoteClient.ExpectedServerPublicKey = TransportServerPublicKey{};
		Check(!RemoteClient.IsValid(), "zero server key cannot authorize remote authenticated admission");
		{
			auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
			auto ServerWorld = std::make_shared<DataModel>();
			HeadlessRenderer ServerRenderer{Vector2(64, 64)};
			Engine ServerRuntime{ServerWorld, &ServerRenderer, nullptr,
				EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}};
			GameSession Server(Network->CreateTransport(),
				AuthenticatedConfiguration(GameSessionRole::Server, Provider), &ServerRuntime);
			auto ClientTransport = std::make_shared<UntrustedServerTransport>(Network->CreateTransport());
			auto ClientConfiguration = AuthenticatedConfiguration(GameSessionRole::Client, Provider);
			ClientConfiguration.ExpectedServerPublicKey = ServerKey;
			GameSession Client(ClientTransport, ClientConfiguration);
			ServerRuntime.ProcessService->Alive = true;
			Check(Server.Start().Succeeded() && Client.Start().Succeeded(),
				"server-key negative fixture starts both transports");
			for (int Index = 0; Index < 12; ++Index) {
				(void)Server.Poll();
				(void)Client.Poll();
			}
			Check(ClientTransport->SentMessages == 0 && Provider->Requests.empty(),
				"missing authenticated transport key sends no v2 proof or admission request");
			(void)Client.Stop();
			(void)Server.Stop();
			ServerRuntime.Destroy();
		}
		auto MissingProof = AuthenticatedConfiguration(GameSessionRole::Client, Provider);
		MissingProof.AdmissionProof.reset();
		Check(!MissingProof.IsValid(), "authenticated client cannot silently send a v1 hello");
		auto Development = Configuration(GameSessionRole::Server, "127.0.0.1");
		Development.AdmissionProvider = Provider;
		Check(!Development.IsValid(), "DevelopmentLocal cannot inherit a trusted provider accidentally");

		{
			AdmissionFixture Fixture(120, Provider);
			Fixture.AwaitRequest();
			Check(Fixture.Provider->Requests[0].Proof == TestAdmissionProof() &&
				Fixture.Provider->Requests[0].Connection.IsValid() &&
				Fixture.Provider->Requests[0].ClientNonce != 0,
				"provider receives a bounded proof and generation-safe connection context");
			Check(Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				Fixture.Server.GetPeerIdentities().empty(),
				"authenticated hello creates no Player before provider completion");
			Fixture.Provider->Complete(0, PlayerIdentity{"test-provider", "account-1"});
			for (int Index = 0; Index < 150 && Fixture.Server.GetMetrics().ReadyPeers == 0; ++Index)
				Fixture.AdvanceTick(true);
			Check(Fixture.Server.GetMetrics().ReadyPeers == 1 && Fixture.Client.GetStatus() == GameSessionStatus::Ready,
				"approved v2 peer reaches the existing ready and replication path");
			const auto Peers = Fixture.Server.GetPeerIdentities();
			const auto PlayerValue = Peers.empty() ? nullptr : Fixture.Server.GetAcceptedPlayer(Peers.front().Connection);
			Check(PlayerValue && PlayerValue->GetAuthenticationIdentity() &&
				*PlayerValue->GetAuthenticationIdentity() == PlayerIdentity{"test-provider", "account-1"},
				"Player is created with the provider-qualified identity");
			const auto RequestId = Provider->Requests[0].RequestId;
			Fixture.Client.Stop();
			for (int Index = 0; Index < 10 && !Fixture.ServerRuntime.Players->GetPlayers().empty(); ++Index) {
				(void)Fixture.Network->Advance(2ms);
				Fixture.Network->Pump();
				(void)Fixture.Server.Poll();
			}
			Check(Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				std::ranges::count(Provider->Released, RequestId) == 1,
				"accepted disconnect releases the provider request exactly once");
		}
		{
			AdmissionFixture Replay(120, Provider);
			for (int Index = 0; Index < 20 && Provider->Requests.size() < 2; ++Index) Replay.AdvanceTick();
			Check(Provider->Requests.size() == 2 && Provider->Requests[1].Proof == Provider->Requests[0].Proof &&
				Provider->Requests[1].RequestId != Provider->Requests[0].RequestId &&
				Replay.ServerRuntime.Players->GetPlayers().empty() &&
				Replay.Server.GetMetrics().RejectedHandshakes > 0,
				"one-use provider replay denial never creates a DevelopmentLocal Player");
		}
		{
			AdmissionFixture Fixture;
			Fixture.AwaitRequest();
			Fixture.Provider->Complete(0, std::nullopt);
			Fixture.AdvanceTick();
			Check(Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				Fixture.Server.GetMetrics().RejectedHandshakes > 0,
				"provider denial cannot fall back to DevelopmentLocal Player creation");
		}
		{
			AdmissionFixture Fixture;
			Fixture.Provider->StartAllowed = false;
			for (int Index = 0; Index < 20 && Fixture.Server.GetMetrics().RejectedHandshakes == 0; ++Index)
				Fixture.AdvanceTick();
			Check(Fixture.Server.GetMetrics().RejectedHandshakes > 0 &&
				Fixture.ServerRuntime.Players->GetPlayers().empty(),
				"unavailable provider fails closed before Player creation");
		}
		{
			AdmissionFixture Fixture;
			Fixture.AwaitRequest();
			Fixture.Provider->Complete(0, PlayerIdentity{"invalid provider", "account"});
			Fixture.AdvanceTick();
			Check(Fixture.Server.GetMetrics().RejectedHandshakes > 0 &&
				Fixture.ServerRuntime.Players->GetPlayers().empty(),
				"invalid provider identity is rejected before Player creation");
		}
		{
			AdmissionFixture Fixture;
			Fixture.AwaitRequest();
			const auto RequestId = Fixture.Provider->Requests[0].RequestId;
			const auto OriginalConnection = Fixture.Provider->Requests[0].Connection;
			Fixture.Client.Stop();
			Fixture.Network->Pump();
			(void)Fixture.Server.Poll();
			GameSession Replacement(Fixture.Network->CreateTransport(),
				AuthenticatedConfiguration(GameSessionRole::Client, Fixture.Provider));
			Check(Replacement.Start().Succeeded(), "replacement authenticated client starts");
			for (int Index = 0; Index < 20 && Fixture.Provider->Requests.size() < 2; ++Index) {
				(void)Fixture.Network->Advance(2ms);
				Fixture.Network->Pump();
				(void)Replacement.Poll();
				(void)Fixture.Server.Poll();
				Fixture.Server.Step(Fixture.Tick);
				Replacement.Step(Fixture.Tick++);
				Fixture.ServerRuntime.Step();
			}
			Check(Fixture.Provider->Requests.size() == 2 &&
				Fixture.Provider->Requests[1].Connection != OriginalConnection,
				"replacement peer has a distinct generation-safe connection identity");
			Fixture.Provider->Complete(0, PlayerIdentity{"test-provider", "late-disconnect"});
			Fixture.Server.Step(Fixture.Tick++);
			Check(Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				std::ranges::count(Fixture.Provider->Released, RequestId) == 1,
				"disconnect cancels verification and ignores stale approval after replacement");
			Replacement.Stop();
		}
		{
			AdmissionFixture Fixture;
			Fixture.AwaitRequest();
			const auto RequestId = Fixture.Provider->Requests[0].RequestId;
			Fixture.Server.Stop();
			Fixture.Provider->Complete(0, PlayerIdentity{"test-provider", "late-stop"});
			Fixture.Server.Step(Fixture.Tick++);
			Check(Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				std::ranges::count(Fixture.Provider->Released, RequestId) == 1,
				"Stop cancels verification and cannot resurrect a Player");
		}
		{
			AdmissionFixture Fixture;
			Fixture.AwaitRequest();
			const auto RequestId = Fixture.Provider->Requests[0].RequestId;
			Fixture.Provider->Complete(0, PlayerIdentity{"test-provider", "accepted-stop"});
			Fixture.Server.Step(Fixture.Tick++);
			Check(Fixture.ServerRuntime.Players->GetPlayers().size() == 1,
				"accepted-stop fixture creates its Player only after approval");
			Fixture.Server.Stop();
			Fixture.Server.Stop();
			Check(Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				std::ranges::count(Fixture.Provider->Released, RequestId) == 1,
				"Stop releases an accepted provider request exactly once");
		}
		{
			AdmissionFixture Fixture(3);
			Fixture.AwaitRequest();
			const auto RequestId = Fixture.Provider->Requests[0].RequestId;
			for (int Index = 0; Index < 4; ++Index) Fixture.AdvanceTick();
			Fixture.Provider->Complete(0, PlayerIdentity{"test-provider", "late-timeout"});
			Fixture.Server.Step(Fixture.Tick++);
			Check(Fixture.Server.GetMetrics().HandshakeTimeouts == 1 &&
				Fixture.ServerRuntime.Players->GetPlayers().empty() &&
				std::ranges::count(Fixture.Provider->Released, RequestId) == 1,
				"verification timeout cancels and rejects late approval before Player creation");
		}
	}

	void TestNetworkTerrainBootstrap(bool NarrowQueue = false, unsigned ChurnCycles = 0) {
		auto Field = std::make_unique<terrain::Field>();
		for (const auto X : {12.0, 76.0, 140.0}) {
			const terrain::BoxBrush Brush{{X, 4.63, 12}, {8.7, 2.0, 8.7}, terrain::BrushMode::Add, 5};
			const auto Keys = terrain::GetRegionChunks(terrain::GetBrushBounds(Field->GetGrid(), Brush).value()).value();
			auto Edit = terrain::Field::Prepare(Field->Capture(Keys).value(), Brush).value();
			Check(Field->Commit(std::move(Edit)).has_value(), "build the canonical server field");
		}
		const std::array PressureKeys{terrain::ChunkKey{-1, 0, -1}};
		std::vector<terrain::SampleWrite> PressureSamples;
		for (int Z = -32; Z < 0; ++Z)
			for (int Y = 0; Y < 32; ++Y)
				for (int X = -32; X < 0; ++X) {
					const auto Density = static_cast<std::int16_t>((X * X + 17 * Y + 311 * Z * Z) % 28'000 + 1);
					PressureSamples.push_back({{X, Y, Z}, {Density, 0}});
				}
		Check(Field->Commit(terrain::Field::Prepare(Field->Capture(PressureKeys).value(), PressureSamples).value()).has_value(),
			"the pressure fixture includes a large exact non-solid leaf");
		const auto Project = ProjectId::New();
		const auto Capture = terrain::CaptureTerrain(*Field, Project, terrain::GetTerrainDatasetId(Project)).value();
		const auto Source = MakeTerrainSessionData(Project, 1, Capture.Artifacts, true);
		const auto Receiver = MakeTerrainSessionData(Project, 1, std::span(Capture.Artifacts).last(1), false);
		const auto SpawnWindow = SelectTerrainSessionWindow(*Source, {});
		auto SpawnPins = SelectTerrainSessionWindow(*Receiver, {});
		Check(SpawnWindow.Size > MaximumTerrainFragmentBytes && Receiver->Source.empty() &&
			SpawnPins.ManifestDigest == SpawnWindow.ManifestDigest &&
			SpawnWindow.Artifacts.size() < Source->Directory.Chunks.size(),
			"matching manifest-only clients derive exactly the bounded spawn neighborhood");
		auto EmptyWindow = SelectTerrainSessionWindow(*Receiver, {-40, 0, 0});
		Check(EmptyWindow.Size == 0 && LoadTerrainSessionField(EmptyWindow, {})->GetStats().MaterializedPages == 0,
			"an empty neighborhood still admits a verified field");
		std::vector<std::uint8_t> Corrupt;
		while (Corrupt.size() < SpawnWindow.Size) {
			const auto Block = GetTerrainSessionFragment(SpawnWindow, Corrupt.size());
			if (Block.empty()) throw std::runtime_error("Terrain window unexpectedly lost an immutable leaf");
			const auto *Bytes = reinterpret_cast<const std::uint8_t *>(Block.data());
			Corrupt.insert(Corrupt.end(), Bytes, Bytes + Block.size());
		}
		Corrupt.front() ^= 1;
		bool Denied = false;
		try { (void)LoadTerrainSessionField(SpawnPins, Corrupt); }
		catch (const std::exception &) { Denied = true; }
		Check(Denied, "same-length corrupt terrain fails artifact admission");
		const auto FirstSlice = GetTerrainSessionFragment(SpawnWindow, 0);
		auto First = EncodeTerrainFragment({7, 1, 0, FirstSlice});
		Check(DecodeTerrainFragment(First).has_value(), "the explicit fragment codec accepts canonical bytes");
		First.push_back(std::byte{0});
		Check(!DecodeTerrainFragment(First), "fragment codec denies trailing bytes");
		const TerrainWindowIdentity Identity{7, 1, 3, SpawnWindow.ManifestDigest};
		const auto Offer = DecodeTerrainWindowOffer(EncodeTerrainWindowOffer({Identity, {-2, 0, 1}}));
		Check(Offer && Offer->Center == terrain::ChunkKey{-2, 0, 1} &&
			EncodeTerrainWindowOffer({Identity, {40'000, 0, 1}}).empty() &&
			DecodeTerrainWindowReady(EncodeTerrainWindowReady(Identity)) == Identity,
			"terrain offers preserve signed coordinates and reject centers without a complete halo");

		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		Space->RestoreTerrainField(std::move(Field));
		HeadlessRenderer ServerRenderer(Vector2(64, 64)), ClientRenderer(Vector2(64, 64));
		Engine Runtime(World, &ServerRenderer, nullptr,
			EngineProviderConfiguration{.Mode = RuntimeMode::NetworkServer, .AdmitNetworkTerrain = true});
		Runtime.ProcessService->Alive = true;
		const TransportEndpoint Endpoint{"127.0.0.1", 36719};
		auto Limits = GameSessionConfiguration::DefaultLimits();
		Limits.MaximumReliableMessageBytes = NarrowQueue ? TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes : 32 * 1024;
		Limits.MaximumQueuedReliableBytes = NarrowQueue ? Limits.MaximumReliableMessageBytes : 64 * 1024;
		Check(SpawnWindow.Size > Limits.MaximumQueuedReliableBytes,
			"withholding a complete window would exhaust the reliable queue without admission control");
		auto ServerTransport = std::make_shared<HandoffTransport>(std::make_shared<GameNetworkingSocketsTransport>());
		GameSession Server(ServerTransport,
			{.Role = GameSessionRole::Server, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap = Source}, &Runtime);
		Check(Server.Start().Succeeded(), "matching terrain server listens through GNS");
		std::uint64_t Tick = 0;
		const auto Exchange = [&](GameSession &Client, GameSession *Other = nullptr) {
			(void)Server.Poll();
			(void)Client.Poll();
			if (Other) (void)Other->Poll();
			Server.Step(++Tick);
			Client.Step(Tick);
			if (Other) Other->Step(Tick);
			std::this_thread::sleep_for(3ms);
		};
		{
			GameSession Wrong(std::make_shared<GameNetworkingSocketsTransport>(),
				{.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits});
			Check(Wrong.Start().Succeeded(), "terrain mismatch probe connects through GNS");
			for (int Step = 0; Step < 150 && Wrong.GetStatus() != GameSessionStatus::Failed; ++Step) Exchange(Wrong);
			Check(Wrong.GetStatus() == GameSessionStatus::Failed && !Wrong.GetClientDataModel() &&
				Server.GetMetrics().PlayersCreated == 0, "absent terrain binding fails before Player creation");
			Wrong.Stop();
		}
		ServerTransport->HoldTerrain = true;
		auto ClientTransport = std::make_shared<HandoffTransport>(std::make_shared<GameNetworkingSocketsTransport>());
		GameSession Client(ClientTransport,
			{.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap = Receiver});
		Check(Client.Start().Succeeded(), "terrain client connects through GNS");
		for (int Step = 0; Step < 150 && (!Client.GetMetrics().AcceptedPeers || !ServerTransport->TerrainDeferrals); ++Step)
			Exchange(Client);
		Check(Client.GetMetrics().AcceptedPeers == 1 && ServerTransport->TerrainDeferrals > 0 &&
			!Client.GetClientDataModel() && Server.GetMetrics().ReadyPeers == 0,
			"the structurally accepted client has no gameplay world while terrain delivery is stalled");
		for (int Step = 0; Step < 64; ++Step) Exchange(Client);
		Check(Client.GetStatus() != GameSessionStatus::Failed && Server.GetPeerIdentities().size() == 1 &&
			ServerTransport->TerrainBytes == 0, "sustained transport backpressure retains one blocked fragment");
		ServerTransport->HoldTerrain = false;
		for (int Step = 0; Step < 300 && !Client.GetClientDataModel() &&
			Client.GetStatus() != GameSessionStatus::Failed; ++Step) Exchange(Client);
		const auto Replica = Client.GetClientDataModel();
		Check(Replica != nullptr, "the client verifies the real streamed terrain closure");
		if (!Replica) {
			std::cerr << "[Terrain:Network] Client bootstrap failed: " << Client.GetFailure() << '\n';
			return;
		}
		const auto ReplicaSpace = std::dynamic_pointer_cast<Workspace>(Replica->GetService("Workspace"));
		Check(ReplicaSpace && !ReplicaSpace->IsTerrainCollisionReady() &&
			ReplicaSpace->GetTerrain()->GetField().Read({12, 4, 12})->Material == 5 &&
			ReplicaSpace->GetTerrain()->GetField().Read({140, 4, 12}) == terrain::AirSample &&
			ServerTransport->TerrainBytes == SpawnWindow.Size,
			"the client receives exact local data before readiness and no distant leaf bytes");
		Check(ServerTransport->MaximumTerrainSlice == (NarrowQueue
			? Limits.MaximumQueuedReliableBytes / 2 - ReliableServiceEnvelopeBytes - TerrainFragmentHeaderBytes
			: MaximumTerrainFragmentBytes), "terrain fragment size observes the negotiated reliable queue and native envelope");
		for (int Step = 0; Step < 12; ++Step) Exchange(Client);
		Check(Server.GetMetrics().ReadyPeers == 0, "received field alone does not admit gameplay");

		Engine ClientRuntime(Replica, &ClientRenderer, nullptr,
			EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitNetworkTerrain = true});
		ClientRuntime.ProcessService->Alive = true;
		RaycastParams Query;
		Query.IncludeTerrain = true;
		const auto ClientHit = ClientRuntime.Workspace->ResolveRaycast({12, 18, 12}, {0, -18, 0}, Query);
		const auto ServerHit = Runtime.Workspace->ResolveRaycast({12, 18, 12}, {0, -18, 0}, Query);
		Check(ClientHit.GetHitInstance() == ReplicaSpace->GetTerrain() &&
			ServerHit.GetHitInstance() == Space->GetTerrain(),
			"both native providers return the terrain hit before client ready is queued");
		const auto Missing = ClientRuntime.Workspace->ResolveRaycast({140, 18, 12}, {0, -18, 0}, Query);
		const auto KnownAir = ClientRuntime.Workspace->ResolveRaycast({24, 18, 12}, {0, -18, 0}, Query);
		Check(!Missing.Succeeded() && KnownAir.Succeeded() && !KnownAir.HasHit() &&
			Runtime.Workspace->ResolveRaycast({140, 18, 12}, {0, -18, 0}, Query).GetHitInstance() == Space->GetTerrain(),
			"covered air is distinct from unavailable client terrain");
		ClientTransport->HoldTerrainReady = true;
		Check(Client.AttachClientRuntime(ClientRuntime), "client movement manager attaches after provider admission");
		const auto LocalPlayer = ClientRuntime.Players->GetLocalPlayer().value_or(nullptr);
		const auto LocalCharacter = LocalPlayer ? LocalPlayer->GetCharacter().value_or(nullptr) : nullptr;
		const auto TryLocalMove = [&] {
			return LocalCharacter && LocalCharacter->AdmitMotion(*ClientRuntime.WorldRoot,
				{.Translation = {0.25f, 0.0f, 0.0f}}).Succeeded();
		};
		for (int Step = 0; Step < 25; ++Step) Exchange(Client);
		Check(ClientTransport->TerrainReadyDeferrals > 0 && LocalCharacter && !TryLocalMove() &&
			Server.GetMetrics().ReadyPeers == 0, "installed collision alone cannot release local player movement");
		auto SpawnEdit = std::make_shared<Script>();
		SpawnEdit->SetRunContext(Enums::RunContext::Server);
		SpawnEdit->SetSource(
			"game.Workspace.Terrain:FillBall(Vector3.new(-18.4, 4.4, 12.4), 2.1, 31)");
		const auto SpawnRevision = Space->GetTerrain()->GetField().GetRevision();
		SpawnEdit->SetParent(Runtime.DataModel);
		for (int Step = 0; Step < 180 && Space->GetTerrain()->GetField().GetRevision() == SpawnRevision; ++Step) {
			Runtime.Step();
			std::this_thread::sleep_for(3ms);
		}
		SpawnEdit->Destroy();
		Check(Space->GetTerrain()->GetField().Read({-18, 4, 12})->Material == 31 &&
			ReplicaSpace->GetTerrain()->GetField().Read({-18, 4, 12})->Material == 0,
			"an authoritative edit can supersede the already installed spawn window");
		ClientTransport->HoldTerrainReady = false;
		ClientTransport->TerrainReadyDeferrals = 0;
		ServerTransport->HoldTerrainReady = true;
		for (int Step = 0; Step < 120 && Server.GetMetrics().ReadyPeers == 0; ++Step) Exchange(Client);
		Check(Server.GetMetrics().ReadyPeers == 1 && !TryLocalMove(),
			"server readiness cannot release client movement until the server confirmation arrives");
		for (int Step = 0; Step < 250 && !ServerTransport->TerrainReadyDeferrals; ++Step) Exchange(Client);
		Check(ServerTransport->TerrainReadyDeferrals > 0 &&
			ReplicaSpace->GetTerrain()->GetField().Read({-18, 4, 12})->Material == 31 && !TryLocalMove(),
			"the server replaces superseded spawn collision before sending the first movement confirmation");
		ServerTransport->HoldTerrainReady = false;
		bool ClientMotionReleased = false;
		for (int Step = 0; Step < 120 && !ClientMotionReleased; ++Step) {
			Exchange(Client);
			ClientMotionReleased = TryLocalMove();
		}
		Check(ClientMotionReleased, "the bilateral collision acknowledgment releases the client's movement");
		const auto Peers = Server.GetPeerIdentities();
		if (Peers.size() == 1) {
			const auto Owner = Server.GetAcceptedPlayer(Peers.front().Connection);
			auto Character = Owner ? Owner->GetCharacter().value_or(nullptr) : nullptr;
			Check(Character != nullptr, "the collision-ready peer owns a Character");
			if (Character) {
				Character->SetPosition({12.0f, 8.8f, 12.0f});
				for (int Step = 0; Step < 35; ++Step) {
					Runtime.Step();
					ClientRuntime.Step();
					Exchange(Client);
				}
				const auto Before = Character->GetPosition();
				(void)ClientRuntime.ProcessEvent(KeyEvent{
					.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W,
					.State = ButtonState::Pressed});
				for (int Step = 0; Step < 38; ++Step) {
					Runtime.Step();
					ClientRuntime.Step();
					Exchange(Client);
				}
				const auto After = Character->GetPosition();
				Check(glm::length(glm::vec2(After.x - Before.x, After.z - Before.z)) > 0.15f && After.y > 6.6f,
					"native input moves the authoritative Character over streamed terrain after readiness");
				(void)ClientRuntime.ProcessEvent(KeyEvent{
					.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W,
					.State = ButtonState::Released});
				Character->SetPosition({56.0f, 8.8f, 12.0f});
				if (LocalCharacter) LocalCharacter->SetPosition({56.0f, 8.8f, 12.0f});
				const CharacterMotionRequest Cross{.Translation = {6.0f, 0.0f, 0.0f}};
				const auto TryLocalCross = [&] {
					return LocalCharacter && LocalCharacter->AdmitMotion(*ClientRuntime.WorldRoot, Cross).Succeeded();
				};
				const auto Rejected = Character->AdmitMotion(*Runtime.WorldRoot, Cross);
				Check(!Rejected.Succeeded() && Character->GetPosition().x == 56.0f,
					"authoritative motion is fenced before entering terrain without verified client collision");
				std::cout << "[Terrain:Network] transferred=" << SpawnWindow.Size << " ground_y=" << After.y << '\n';

				ServerTransport->HoldTerrain = true;
				ClientTransport->HoldTerrainReady = true;
				const auto Deferred = ServerTransport->TerrainDeferrals;
				const auto BeforeWindowBytes = ServerTransport->TerrainBytes;
				for (int Step = 0; Step < 150 && ServerTransport->TerrainDeferrals == Deferred; ++Step) Exchange(Client);
				Check(ServerTransport->TerrainDeferrals > Deferred && !Character->AdmitMotion(*Runtime.WorldRoot, Cross).Succeeded(),
					"crossing the focus boundary schedules a second region and waits for collision");
				ServerTransport->HoldTerrain = false;
				for (int Step = 0; Step < 250 && !ClientTransport->TerrainReadyDeferrals; ++Step) Exchange(Client);
				auto NextWindow = SelectTerrainSessionWindow(*Source, {1, 0, 0});
				const auto FullWindowBytes = NextWindow.Size;
				ReuseTerrainSessionLeaves(NextWindow, SpawnWindow);
				Check(ClientTransport->TerrainReadyDeferrals > 0 &&
					ClientRuntime.Workspace->ResolveRaycast({76, 18, 12}, {0, -18, 0}, Query).GetHitInstance() == ReplicaSpace->GetTerrain() &&
					NextWindow.Size < FullWindowBytes && ServerTransport->TerrainBytes - BeforeWindowBytes == NextWindow.Size &&
					!Character->AdmitMotion(*Runtime.WorldRoot, Cross).Succeeded() && !TryLocalCross(),
					"incremental leaves cut over native collision while both movement paths retain the intersection fence");
				const auto EarlierReleaseDeferrals = ServerTransport->TerrainReadyDeferrals;
				ServerTransport->HoldTerrainReady = true;
				ClientTransport->HoldTerrainReady = false;
				for (int Step = 0; Step < 150 && ServerTransport->TerrainReadyDeferrals == EarlierReleaseDeferrals; ++Step)
					Exchange(Client);
				Check(ServerTransport->TerrainReadyDeferrals > EarlierReleaseDeferrals && !TryLocalCross(),
					"a rolling-window receipt cannot expand client motion until its exact confirmation arrives");
				ServerTransport->HoldTerrainReady = false;
				bool Crossed = false;
				bool LocalCrossed = false;
				for (int Step = 0; Step < 150 && (!Crossed || !LocalCrossed); ++Step) {
					Exchange(Client);
					if (!Crossed) Crossed = Character->AdmitMotion(*Runtime.WorldRoot, Cross).Succeeded();
					if (!LocalCrossed) LocalCrossed = TryLocalCross();
				}
				Check(Crossed && Character->GetPosition().x > 60.0f && LocalCrossed,
					"newly acknowledged coverage admits subsequent motion on both peers");

				GameSession Late(std::make_shared<GameNetworkingSocketsTransport>(),
					{.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap = Receiver});
				ServerTransport->HoldTerrain = true;
				Check(Late.Start().Succeeded(), "the second matching client connects concurrently");
				const auto BeforeLateDeferral = ServerTransport->TerrainDeferrals;
				for (int Step = 0; Step < 150 && ServerTransport->TerrainDeferrals == BeforeLateDeferral; ++Step)
					Exchange(Client, &Late);
				const auto ConcurrentPeers = Server.GetPeerIdentities();
				auto LatePeer = std::ranges::find_if(ConcurrentPeers,
					[&](const auto &Identity) { return Identity.Connection != Peers.front().Connection; });
				auto LateOwner = LatePeer == ConcurrentPeers.end() ? nullptr : Server.GetAcceptedPlayer(LatePeer->Connection);
				auto LateCharacter = LateOwner ? LateOwner->GetCharacter().value_or(nullptr) : nullptr;
				Check(LateCharacter && ServerTransport->TerrainDeferrals > BeforeLateDeferral && !Late.GetClientDataModel(),
					"a second client remains gated on its older held spawn offer");
				if (LateCharacter) LateCharacter->SetPosition({140.0f, 9.0f, 12.0f});

				auto EditScript = std::make_shared<Script>();
				EditScript->SetRunContext(Enums::RunContext::Server);
				EditScript->SetSource(R"(
local Terrain = game:GetService("Workspace").Terrain
Terrain:FillBlock(Vector3.new(58.4, 4.4, 12.4), Vector3.new(5.5, 3.5, 5.5), 17)
)");
				const auto BeforeEdit = Space->GetTerrain()->GetField().GetRevision();
				EditScript->SetParent(Runtime.DataModel);
				for (int Step = 0; Step < 180 && Space->GetTerrain()->GetField().GetRevision() == BeforeEdit; ++Step) {
					Runtime.Step();
					std::this_thread::sleep_for(3ms);
				}
				Character->SetPosition({52.0f, 8.8f, 12.0f});
				Check(Space->GetTerrain()->GetField().GetRevision() > BeforeEdit &&
					Runtime.Workspace->ResolveRaycast({58, 18, 12}, {0, -18, 0}, Query).GetHitInstance() == Space->GetTerrain() &&
					!Character->AdmitMotion(*Runtime.WorldRoot, Cross).Succeeded(),
					"committed server edit changes native collision and immediately holds older peer motion");
				ServerTransport->HoldTerrain = false;
				for (int Step = 0; Step < 300 && ReplicaSpace->GetTerrain()->GetField().Read({58, 4, 12})->Material != 17; ++Step)
					Exchange(Client, &Late);
				const auto ReceivedEdit = ReplicaSpace->GetTerrain()->GetField().Read({58, 4, 12});
				const auto EditHit = ClientRuntime.Workspace->ResolveRaycast({58, 12, 12}, {0, -12, 0}, Query);
				const bool ReplicatedEdit = ReceivedEdit && ReceivedEdit->Material == 17 &&
					EditHit.GetHitInstance() == ReplicaSpace->GetTerrain();
				if (!ReplicatedEdit)
					std::cerr << "[Terrain:Network] edit material=" << (ReceivedEdit ? ReceivedEdit->Material : 0)
						<< " query=" << EditHit.Message << " client=" << Client.GetFailure()
						<< " sent=" << ServerTransport->TerrainBytes << '\n';
				Check(ReplicatedEdit,
					"live terrain revision installs exact material and replacement native collision");
				EditScript->Destroy();

				for (int Step = 0; Step < 200 && !Late.GetClientDataModel() && Late.GetStatus() != GameSessionStatus::Failed; ++Step)
					Exchange(Client, &Late);
				const auto LateWorld = Late.GetClientDataModel();
				Check(LateWorld && LateCharacter &&
					!LateCharacter->AdmitMotion(*Runtime.WorldRoot, {.Translation = {1.0f, 0.0f, 0.0f}}).Succeeded(),
					"late join survives the overlapping edit and teleport while the moved owner remains fenced");
				if (LateWorld) {
					HeadlessRenderer LateRenderer(Vector2(64, 64));
					Engine LateRuntime(LateWorld, &LateRenderer, nullptr,
						EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitNetworkTerrain = true});
					LateRuntime.ProcessService->Alive = true;
					Check(Late.AttachClientRuntime(LateRuntime), "late client acknowledges locally installed collision");
					bool LateMoved = false;
					for (int Step = 0; Step < 300 && !LateMoved; ++Step) {
						Exchange(Client, &Late);
						const auto Hit = LateRuntime.Workspace->ResolveRaycast({140, 18, 12}, {0, -18, 0}, Query);
						LateMoved = Hit.HasHit() && LateCharacter && LateCharacter->AdmitMotion(
							*Runtime.WorldRoot, {.Translation = {1.0f, 0.0f, 0.0f}}).Succeeded();
					}
					Check(LateMoved && !LateRuntime.Workspace->ResolveRaycast({12, 12, 12}, {0, -12, 0}, Query).Succeeded() &&
						LateRuntime.Workspace->GetTerrain()->GetField().GetStats().MaterializedPages <
							Space->GetTerrain()->GetField().GetStats().MaterializedPages,
						"late client activates distant collision and retires its original window");
					if (LateMoved && ReplicatedEdit) {
						const auto ApplyServerEdit = [&](const char *Operation) {
							auto ScriptValue = std::make_shared<Script>();
							ScriptValue->SetRunContext(Enums::RunContext::Server);
							ScriptValue->SetSource(std::string("local Terrain = game:GetService(\"Workspace\").Terrain\n") + Operation);
							const auto OldRevision = Space->GetTerrain()->GetField().GetRevision();
							ScriptValue->SetParent(Runtime.DataModel);
							for (int Step = 0; Step < 180 && Space->GetTerrain()->GetField().GetRevision() == OldRevision; ++Step) {
								Runtime.Step();
								std::this_thread::sleep_for(3ms);
							}
							ScriptValue->Destroy();
							Character->SetPosition({52.0f, 8.8f, 12.0f});
							LateCharacter->SetPosition({140.0f, 9.0f, 12.0f});
							Check(Space->GetTerrain()->GetField().GetRevision() > OldRevision &&
								LateCharacter->AdmitMotion(*Runtime.WorldRoot, {.Translation = {1.0f, 0.0f, 0.0f}}).Succeeded(),
								"an edit keeps authoritative motion available in a disjoint acknowledged region");
						};
						ApplyServerEdit("Terrain:PaintBall(Vector3.new(58.4, 4.4, 12.4), 3.0, 29)");
						for (int Step = 0; Step < 250 && ReplicaSpace->GetTerrain()->GetField().Read({58, 4, 12})->Material != 29; ++Step)
							Exchange(Client, &Late);
						Check(ReplicaSpace->GetTerrain()->GetField().Read({58, 4, 12})->Material == 29 &&
							ClientRuntime.Workspace->ResolveRaycast({58, 12, 12}, {0, -12, 0}, Query).HasHit(),
							"authorized paint replicates the exact material and retains the surface");
						ApplyServerEdit("Terrain:RemoveBall(Vector3.new(58.4, 4.4, 12.4), 3.8)");
						for (int Step = 0; Step < 250 && ReplicaSpace->GetTerrain()->GetField().Read({58, 4, 12})->Material != 0; ++Step)
							Exchange(Client, &Late);
						const auto Opening = ClientRuntime.Workspace->ResolveRaycast({58, 12, 12}, {0, -12, 0}, Query);
						Check(Opening.Succeeded() && !Opening.HasHit() &&
							ReplicaSpace->GetTerrain()->GetField().Read({58, 4, 12})->Material == 0,
							"authorized excavation removes the covered collision");
						if (ChurnCycles && LatePeer != ConcurrentPeers.end() && LateCharacter)
							RunTerrainChurn(Runtime, ClientRuntime, LateRuntime, *Character, *LateCharacter, Server, Client,
								*ServerTransport, LatePeer->Connection, ChurnCycles, [&] { Exchange(Client, &Late); });
					}
					Late.Stop();
					LateRuntime.Destroy();
				} else Late.Stop();
			}
		}
		Client.Stop();
		ClientRuntime.Destroy();
		GameSession Reconnected(std::make_shared<GameNetworkingSocketsTransport>(),
			{.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap = Receiver});
		Check(Reconnected.Start().Succeeded(), "terrain client reconnects through GNS");
		for (int Step = 0; Step < 300 && !Reconnected.GetClientDataModel() &&
			Reconnected.GetStatus() != GameSessionStatus::Failed; ++Step) Exchange(Reconnected);
		const auto NewPeers = Server.GetPeerIdentities();
		Check(Reconnected.GetClientDataModel() && NewPeers.size() == 1 && Peers.size() == 1 &&
			NewPeers.front().SessionEpoch != Peers.front().SessionEpoch && !NewPeers.front().Ready,
			"reconnect repeats verified delivery under a fresh epoch and fresh collision gate");
		if (const auto NewWorld = Reconnected.GetClientDataModel(); NewWorld && NewPeers.size() == 1) {
			HeadlessRenderer ReconnectRenderer(Vector2(64, 64));
			Engine NewRuntime(NewWorld, &ReconnectRenderer, nullptr,
				EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient, .AdmitNetworkTerrain = true});
			NewRuntime.ProcessService->Alive = true;
			Check(Reconnected.AttachClientRuntime(NewRuntime), "reconnected runtime attaches native collision");
			const auto Ready = [&] {
				return std::ranges::any_of(Server.GetPeerIdentities(), [&](const auto &Peer) {
					return Peer.Connection == NewPeers.front().Connection && Peer.Ready;
				});
			};
			const auto ReconnectedPlayer = NewRuntime.Players->GetLocalPlayer().value_or(nullptr);
			const auto ReconnectedCharacter = ReconnectedPlayer ? ReconnectedPlayer->GetCharacter().value_or(nullptr) : nullptr;
			bool Released = false;
			for (int Step = 0; Step < 150 && !Released; ++Step) {
				Exchange(Reconnected);
				Released = Ready() && ReconnectedCharacter && ReconnectedCharacter->AdmitMotion(*NewRuntime.WorldRoot,
					{.Translation = {0.25f, 0.0f, 0.0f}}).Succeeded();
			}
			const auto Owner = Server.GetAcceptedPlayer(NewPeers.front().Connection);
			const auto Controller = Owner ? Owner->GetCharacter().value_or(nullptr) : nullptr;
			Check(Released && Controller, "fresh session owns a distinct controller and receives its movement confirmation");
			if (Controller) {
				Controller->SetPosition({12.0f, 8.8f, 12.0f});
				Check(Controller->AdmitMotion(*Runtime.WorldRoot, {.Translation = {1.0f, 0.0f, 0.0f}}).Succeeded(),
					"the reconnected player resumes motion after the new collision acknowledgment");
				const auto ReleaseDeferrals = ServerTransport->TerrainReadyDeferrals;
				Controller->SetPosition({46.0f, 8.8f, 12.0f});
				ServerTransport->HoldTerrainReady = true;
				for (int Step = 0; Step < 200 && ServerTransport->TerrainReadyDeferrals == ReleaseDeferrals; ++Step)
					Exchange(Reconnected);
				Check(ServerTransport->TerrainReadyDeferrals > ReleaseDeferrals &&
					Reconnected.GetStatus() != GameSessionStatus::Failed, "the transport holds a later server confirmation");
				Reconnected.Step(Tick + DefaultGameSessionHandshakeTimeoutTicks);
				Check(Reconnected.GetStatus() == GameSessionStatus::Failed &&
					Reconnected.GetFailure().find("Terrain motion release timed out") != std::string::npos &&
					!NewRuntime.Workspace->ResolveRaycast({12, 18, 12}, {0, -18, 0}, Query).Succeeded(),
					"a missing server confirmation has a bounded client deadline and removes coverage");
				ServerTransport->HoldTerrainReady = false;
			}
			Reconnected.Stop();
			NewRuntime.Destroy();
		} else Reconnected.Stop();
		Server.Stop();
		Runtime.Destroy();
	}

	void TestNetworkTerrainActorEnvelope() {
		constexpr terrain::GridSpec Grid{-1};
		const TerrainSessionWindow Spawn{.Grid = Grid, .Center = {0, 0, 0}};
		const TerrainSessionWindow West{.Grid = Grid, .Center = {-1, 0, 0}};
		constexpr terrain::WorldPoint Radius{9, 9, 9};
		Check(ContainsTerrainMotion(Spawn, {0.5, 10, 8}, Radius) &&
			ContainsTerrainMotion(Spawn, {-5, 10, 8}, Radius) &&
			ContainsTerrainMotion(West, {-5, 10, 8}, Radius) &&
			!ContainsTerrainMotion(Spawn, {-5.5, 10, 8}, Radius) &&
			!ContainsTerrainMotion(West, {5, 10, 8}, Radius) &&
			!ContainsTerrainMotion(Spawn, {8, 8, 8}, {22, 22, 22}),
			"half-spacing motion reserves the outer four samples and rejects an unrepresentable actor");

		auto Field = std::make_unique<terrain::Field>(Grid);
		for (const double X : {-24.0, -8.0, 8.0, 24.0}) {
			const terrain::BoxBrush Ground{{X, 2.25, 8.0}, {8.2, 1.15, 3.55}, terrain::BrushMode::Add, 21};
			const auto Keys = terrain::GetRegionChunks(terrain::GetBrushBounds(Grid, Ground).value()).value();
			auto Edit = terrain::Field::Prepare(Field->Capture(Keys).value(), Ground);
			if (!Edit) throw std::runtime_error("[Terrain:Network] Half-spacing ground preparation failed");
			Check(Field->Commit(std::move(Edit.value())).has_value(), "half-spacing ground is authored");
		}
		const auto Project = ProjectId::New();
		const auto Capture = terrain::CaptureTerrain(*Field, Project, terrain::GetTerrainDatasetId(Project)).value();
		const auto Source = MakeTerrainSessionData(Project, 1, Capture.Artifacts, true);
		const auto Receiver = MakeTerrainSessionData(Project, 1, std::span(Capture.Artifacts).last(1), false);
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		Space->RestoreTerrainField(std::move(Field));
		HeadlessRenderer ServerRenderer(Vector2(64, 64)), ClientRenderer(Vector2(64, 64));
		Engine Runtime(World, &ServerRenderer, nullptr, EngineProviderConfiguration{
			.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer, .AdmitNetworkTerrain = true});
		Runtime.ProcessService->Alive = true;
		Runtime.Players->SetCharacterAutoLoads(false);
		auto Actor = std::make_shared<Part>();
		Actor->SetName("TerrainEnvelopeActor");
		Actor->SetAnchored(true);
		Actor->SetSize({12, 12, 6});
		Actor->SetPosition({0.5f, 10, 8});
		Actor->SetParent(Space);
		const auto Seating = Runtime.Players->PlayerAdded->Connect([Actor](std::shared_ptr<Player> Owner) {
			Owner->SetPossession(Actor);
		});
		const auto Install = [](Engine &Target, Enums::RunContext Context, const char *SourceText) {
			auto Value = std::make_shared<Script>();
			Value->SetRunContext(Context);
			Value->SetSource(SourceText);
			Value->SetParent(Target.DataModel);
			return Value;
		};
		const auto ServerScript = Install(Runtime, Enums::RunContext::Server, R"(
local Players = game:GetService("Players")
local Control = game:GetService("CharacterControlService")
game:GetService("RunService").PreSimulation:Connect(function()
	for _, Player in Players:GetPlayers() do
		local Actor = Player.PossessedEntity
		local Input = Control:GetPossessionInput(Player)
		if Actor and Input and Input.Fresh then
			Control:MovePossessedPart(Player, CFrame.new(Actor.Position + Vector3.new(Input.Movement.X * 0.5, 0, 0)))
		end
	end
end)
)");
		const TransportEndpoint Endpoint{"127.0.0.1", 36720};
		const auto Limits = GameSessionConfiguration::DefaultLimits();
		auto ServerTransport = std::make_shared<HandoffTransport>(std::make_shared<GameNetworkingSocketsTransport>());
		auto ClientTransport = std::make_shared<HandoffTransport>(std::make_shared<GameNetworkingSocketsTransport>());
		std::unique_ptr<Engine> ClientRuntime;
		GameSession Server(ServerTransport,
			{.Role = GameSessionRole::Server, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap = Source}, &Runtime);
		GameSession Client(ClientTransport,
			{.Role = GameSessionRole::Client, .Endpoint = Endpoint, .Limits = Limits, .TerrainBootstrap = Receiver});
		if (!Server.Start().Succeeded() || !Client.Start().Succeeded()) {
			Check(false, "half-spacing terrain peers start through GNS");
			return;
		}
		const auto Frame = [&] {
			(void)Server.Poll();
			(void)Client.Poll();
			Runtime.Step();
			if (ClientRuntime) ClientRuntime->Step();
			Server.Step(Runtime.GetSimulationTick());
			Client.Step(ClientRuntime ? ClientRuntime->GetSimulationTick() : Runtime.GetSimulationTick());
			std::this_thread::sleep_for(3ms);
		};
		const auto Until = [&](auto Condition) {
			for (int Step = 0; Step < 250 && !Condition() && Client.GetStatus() != GameSessionStatus::Failed; ++Step)
				Frame();
			return Condition();
		};
		if (!Until([&] { return static_cast<bool>(Client.GetClientDataModel()); })) {
			Check(false, "half-spacing terrain bootstrap completes");
			return;
		}
		ClientRuntime = std::make_unique<Engine>(Client.GetClientDataModel(), &ClientRenderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient,
				.AdmitNetworkTerrain = true});
		ClientRuntime->ProcessService->Alive = true;
		ClientTransport->HoldTerrainReady = true;
		Check(Client.AttachClientRuntime(*ClientRuntime), "half-spacing runtime attaches its native collision before acknowledgment");
		const auto ClientScript = Install(*ClientRuntime, Enums::RunContext::Client, R"(
local Players = game:GetService("Players")
local Control = game:GetService("CharacterControlService")
game:GetService("RunService").PreSimulation:Connect(function()
	local Player = Players.LocalPlayer
	if Player and Player.PossessedEntity then
		script:SetAttribute("Submitted", Control:SubmitPossessionIntent(Vector2.new(-1, 0), 0, false))
	end
end)
)");
		const auto Submitted = [&]() -> std::optional<bool> {
			const auto Value = ClientScript->GetAttributeValue("Submitted");
			const auto *Flag = Value ? std::get_if<bool>(&*Value) : nullptr;
			return Flag ? std::optional(*Flag) : std::nullopt;
		};
		RaycastParams Query;
		Query.IncludeTerrain = true;
		const auto WestHit = [&](Engine &Target) {
			return Target.Workspace->ResolveRaycast({-24, 12, 8}, {0, -12, 0}, Query);
		};
		Check(WestHit(Runtime).GetHitInstance() == Space->GetTerrain() &&
			!WestHit(*ClientRuntime).Succeeded(), "the initial replica lacks the western terrain dependency");
		Check(Until([&] { return ClientTransport->TerrainReadyDeferrals > 0 && Submitted() == false; }) &&
			Server.GetMetrics().ReadyPeers == 0 && Actor->GetPosition().x == 0.5f,
			"held client acknowledgment fences native possession input at a safe half-spacing position");

		ServerTransport->HoldTerrainReady = true;
		ClientTransport->HoldTerrainReady = false;
		Check(Until([&] { return ServerTransport->TerrainReadyDeferrals > 0; }) &&
			Server.GetMetrics().ReadyPeers == 1 && Submitted() == false && Actor->GetPosition().x == 0.5f,
			"server acceptance leaves possessed input fenced until confirmation reaches the client");

		const auto BeforeWestReceipt = ClientTransport->TerrainReadyDeferrals;
		ClientTransport->HoldTerrainReady = true;
		ServerTransport->HoldTerrainReady = false;
		Check(Until([&] {
			return Actor->GetPosition().x <= -5.f && ClientTransport->TerrainReadyDeferrals > BeforeWestReceipt;
		}) && Actor->GetPosition().x == -5.f && Submitted() == true && WestHit(*ClientRuntime).HasHit(),
			"confirmed possession crosses signed zero, installs western collision, and stops at the old coverage fence");
		const auto BeforeWestRelease = Server.GetMetrics().PossessionMotionAccepted;
		for (int Step = 0; Step < 12; ++Step) Frame();
		Check(Actor->GetPosition().x == -5.f && Server.GetMetrics().PossessionMotionAccepted == BeforeWestRelease,
			"installed western collision cannot extend authoritative motion while its acknowledgment is held");

		ClientTransport->HoldTerrainReady = false;
		const auto ReplicaActor = [&]() {
			const auto Owner = ClientRuntime->Players->GetLocalPlayer().value_or(nullptr);
			return Owner ? std::dynamic_pointer_cast<Part>(Owner->GetPossessedEntity().value_or(nullptr)) : nullptr;
		};
		Check(Until([&] {
			const auto Replica = ReplicaActor();
			return Actor->GetPosition().x < -6.f && Replica && Replica->GetPosition().x < -6.f;
		}) && Server.GetMetrics().PossessionMotionAccepted > BeforeWestRelease,
			"acknowledged western window resumes native possessed motion and replicates its authoritative pose");
		const auto Position = Actor->GetPosition();
		const auto BeforeOversized = Server.GetMetrics().PossessionMotionAccepted;
		Actor->SetSize({44, 1, 1});
		for (int Step = 0; Step < 16; ++Step) Frame();
		Check(Actor->GetPosition() == Position && Server.GetMetrics().PossessionMotionAccepted == BeforeOversized,
			"an actor larger than available collision stays fenced even after the window is acknowledged");
		std::cout << "[Terrain:Network] Half-spacing possession signed handoff reached x=" << Position.x << '\n';
		Client.Stop();
		ClientRuntime->Destroy();
		Server.Stop();
		Runtime.Destroy();
	}
#endif

	void TestServerSessionSignalLifetime() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		{
			GameSession Server(
				ServerTransport, Configuration(GameSessionRole::Server, "signal-lifetime"), &ServerRuntime
			);
			Check(Server.Start().Succeeded(), "server session for signal-lifetime regression starts");
			Server.Stop();
		}
		ServerRuntime.Destroy();
	}

	void TestSessionOwnershipAndEndpointPolicy() {
		Check(
			IsLoopbackTransportEndpoint({"127.0.0.1", 27020}) && IsLoopbackTransportEndpoint({"127.255.10.9", 27020}) &&
				IsLoopbackTransportEndpoint({"::1", 27020}) &&
				!IsLoopbackTransportEndpoint({"127.example.com", 27020}) &&
				!IsLoopbackTransportEndpoint({"127.0.0.1.example.com", 27020}) &&
				!IsLoopbackTransportEndpoint({"127.0.0.1x", 27020}) &&
				!IsLoopbackTransportEndpoint({"127.0.0.1.", 27020}) &&
				!IsLoopbackTransportEndpoint({"0127.0.0.1", 27020}) &&
				!IsLoopbackTransportEndpoint({"10.0.0.2", 27020}),
			"DevelopmentLocal loopback validation uses parsed address semantics without prefix trust"
		);
		auto Restricted = Configuration(GameSessionRole::Server, "10.0.0.2");
		Restricted.AllowInsecureDevelopmentNetwork = false;
		Check(
			!Restricted.IsValid() && Configuration(GameSessionRole::Server, "10.0.0.2").IsValid(),
			"non-loopback DevelopmentLocal endpoints require the explicit insecure-development override"
		);
		auto DeclaredClient = Configuration(GameSessionRole::Client);
		DeclaredClient.DeclareNormalOrdinaryService = true;
		Check(DeclaredClient.IsValid() && !DeclaredClient.ReliableService,
			"trusted client ordinary source declaration is independent of server funding and the wire profile");
		auto WrongRoleDeclaration = Configuration(GameSessionRole::Server);
		WrongRoleDeclaration.DeclareNormalOrdinaryService = true;
		Check(!WrongRoleDeclaration.IsValid(), "server normal declaration remains selected only by its pooled profile");

		auto World = std::make_shared<DataModel>();
		auto Control = std::dynamic_pointer_cast<CharacterControlService>(World->GetService("CharacterControlService"));
		auto First = Control->AttachRuntime({}, {}, {}, {});
		auto Overlap = Control->AttachRuntime({}, {}, {}, {});
		Check(
			First && First->IsValid() && !Overlap,
			"CharacterControl accepts one explicit scoped runtime owner and rejects overlapping attachment"
		);
		Control->DetachRuntime();
		Check(First && !First->IsValid(), "a stale CharacterControl lease observes broad engine shutdown detachment");
		auto Replacement = Control->AttachRuntime({}, {}, {}, {});
		First.reset();
		Check(
			Replacement && Replacement->IsValid(),
			"releasing a stale CharacterControl lease cannot detach a fresh attachment generation"
		);
		Replacement.reset();

		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto Transport = Network->CreateTransport();
		HeadlessRenderer Renderer(Vector2(320, 240));
		Engine Runtime(
			World,
			&Renderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		{
			GameSession Unstarted(Transport, Configuration(GameSessionRole::Server, "one-shot-session"), &Runtime);
			Check(
				Unstarted.GetStatus() == GameSessionStatus::Created,
				"a newly constructed GameSession owns no started transport or callback lifetime"
			);
		}
		auto AfterUnstarted = Runtime.CharacterControl->AttachRuntime({}, {}, {}, {});
		Check(
			AfterUnstarted && AfterUnstarted->IsValid(),
			"destroying an unstarted GameSession leaves no stale CharacterControl callback owner"
		);
		AfterUnstarted.reset();
		GameSession Session(Transport, Configuration(GameSessionRole::Server, "one-shot-session"), &Runtime);
		Check(Session.Start().Succeeded(), "one-shot GameSession starts once");
		host::detail::FarmLifecycleEvidence BeforeStop;
		BeforeStop.ObserveSession(Session);
		Check(!BeforeStop.Valid(), "logical lifecycle receipt rejects a live session even with no peers");
		Session.Stop();
		host::detail::FarmLifecycleEvidence AfterStopEvidence;
		AfterStopEvidence.ObserveSession(Session);
		Check(AfterStopEvidence.Valid(), "post-Stop native snapshot proves terminal ownership release");
		Check(
			Session.GetStatus() == GameSessionStatus::Closed &&
				Session.Start().Status == TransportOperationStatus::InvalidState,
			"GameSession Stop is terminal and restart is truthfully rejected"
		);
		Session.Stop();
		auto AfterStop = Runtime.CharacterControl->AttachRuntime({}, {}, {}, {});
		Check(
			AfterStop && AfterStop->IsValid(),
			"terminal session cleanup releases only its matching CharacterControl attachment"
		);
		AfterStop.reset();
		Runtime.Destroy();
	}

	void TestRegistrationCommitAfterCapacityFailure() {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		Characters.reserve(MaximumNetworkCharacters + 1);
		for (std::size_t Index = 0; Index <= MaximumNetworkCharacters; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetPosition({static_cast<float>(Index) * 2.0f, 6.0f, 0.0f});
			Character->SetParent(World);
			Characters.push_back(std::move(Character));
		}
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(
			World,
			&Renderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		GameSession Overflow(
			Network->CreateTransport(), Configuration(GameSessionRole::Server, "registration-capacity"), &Runtime
		);
		Check(
			!Overflow.Start().Succeeded() && Overflow.GetStatus() == GameSessionStatus::Failed,
			"Character registry capacity failure aborts server session acquisition before a live callback owner commits"
		);
		Characters.back()->Destroy();
		Characters.pop_back();
		GameSession Retry(
			Network->CreateTransport(), Configuration(GameSessionRole::Server, "registration-capacity-retry"), &Runtime
		);
		Check(
			Retry.Start().Succeeded(),
			"a fresh session can register the exact Character capacity after the failed candidate is removed"
		);
		Retry.Stop();
		Runtime.Destroy();
	}

	void TestClientBootstrapFailureMatrix() {
		using detail::GameSessionFailurePoint;
		using detail::GameSessionTestAccess;
		const std::array Cases{
			std::pair{GameSessionFailurePoint::TransportStart, "transport start"},
			std::pair{GameSessionFailurePoint::SchedulerRegistration, "scheduler registration"},
			std::pair{GameSessionFailurePoint::ReplicationPeerCreation, "replication peer creation"},
			std::pair{GameSessionFailurePoint::LocalPlayerResolution, "LocalPlayer resolution"},
			std::pair{GameSessionFailurePoint::RemoteManagerPeerCreation, "RemoteManager peer creation"},
			std::pair{GameSessionFailurePoint::PredictedCharacterPeerCreation, "predicted Character peer creation"},
			std::pair{GameSessionFailurePoint::RuntimeCallbackAttachment, "runtime callback attachment"},
			std::pair{GameSessionFailurePoint::ClientGraphSynchronization, "client graph synchronization"},
			std::pair{GameSessionFailurePoint::ClientReadySerialization, "ClientReady serialization"},
			std::pair{GameSessionFailurePoint::ClientReadySchedulerAdmission, "ClientReady scheduler admission"},
		};
		for (const auto &[Point, Name] : Cases) {
			auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
			auto ServerWorld = std::make_shared<DataModel>();
			HeadlessRenderer ServerRenderer(Vector2(64, 64));
			Engine ServerRuntime(
				ServerWorld,
				&ServerRenderer,
				nullptr,
				EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
			);
			ServerRuntime.ProcessService->Alive = true;
			GameSession Server(
				Network->CreateTransport(), Configuration(GameSessionRole::Server, "bootstrap-failure"), &ServerRuntime
			);
			GameSession Client(Network->CreateTransport(), Configuration(GameSessionRole::Client, "bootstrap-failure"));
			GameSessionTestAccess::SetFailurePoint(Client, Point);
			Check(Server.Start().Succeeded(), "failure-matrix server starts");
			const auto ClientStart = Client.Start();
			if (Point == GameSessionFailurePoint::TransportStart)
				Check(!ClientStart.Succeeded(), "injected transport acquisition aborts Start");
			else
				Check(ClientStart.Succeeded(), "failure-matrix client transport starts before later injection");

			std::unique_ptr<HeadlessRenderer> ClientRenderer;
			std::unique_ptr<Engine> ClientRuntime;
			for (std::uint64_t Tick = 1; Tick <= 100 && Client.GetStatus() != GameSessionStatus::Failed; ++Tick) {
				Advance(Network, Server, Client, Tick);
				if (!ClientRuntime && Client.GetClientDataModel()) {
					ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
					ClientRuntime = std::make_unique<Engine>(
						Client.GetClientDataModel(),
						ClientRenderer.get(),
						nullptr,
						EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
					);
					ClientRuntime->ProcessService->Alive = true;
					Check(!Client.AttachClientRuntime(*ClientRuntime), Name);
				}
				if (ClientRuntime) ClientRuntime->Step();
				ServerRuntime.Step();
			}
			for (std::uint64_t Tick = 101; Tick <= 112; ++Tick) {
				Advance(Network, Server, Client, Tick);
				ServerRuntime.Step();
			}
			Check(
				Client.GetStatus() == GameSessionStatus::Failed && !Client.GetClientDataModel() &&
					!Client.GetPrimaryConnection() && Client.GetMetrics().ReadyPeers == 0,
				"every injected client bootstrap failure rolls back replica visibility and readiness"
			);
			Check(
				Client.Start().Status == TransportOperationStatus::InvalidState,
				"a failed GameSession is terminal and cannot restart partially destroyed resources"
			);
			Check(
				ServerRuntime.Players->GetPlayers().empty(),
				"client bootstrap failure removes any partially admitted authoritative Player"
			);
			if (ClientRuntime) {
				auto Released = ClientRuntime->CharacterControl->AttachRuntime({}, {}, {}, {});
				Check(
					Released && Released->IsValid(),
					"client bootstrap failure releases any acquired CharacterControl callback lease"
				);
			}
			Client.Stop();
			Server.Stop();
			if (ClientRuntime) ClientRuntime->Destroy();
			ServerRuntime.Destroy();
		}
	}

	void TestPreAcceptanceAndTimeoutRejection() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto RawTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			[](std::string Code, std::string Message) {
				if (Code == "Error") std::cerr << "[Network:SessionTest] " << Message << '\n';
			},
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "hostile-session");
		ServerConfiguration.HandshakeTimeoutTicks = 3;
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(Server.Start().Succeeded(), "hostile-session server starts");
		Check(
			RawTransport
				->Start({
					.Role = TransportRole::Client,
					.Endpoint = ServerConfiguration.Endpoint,
					.AdvertisedLimits = ServerConfiguration.Limits,
				})
				.Succeeded(),
			"raw transport reaches the production session endpoint"
		);
		Network->Pump();
		(void)Server.Poll();
		std::array<TransportEvent, 8> RawEvents;
		const auto EventCount = RawTransport->PollEvents(RawEvents);
		std::optional<ConnectionId> RawConnection;
		for (std::size_t Index = 0; Index < EventCount; ++Index)
			if (const auto *Changed = std::get_if<ConnectionStateEvent>(&RawEvents[Index]);
				Changed && Changed->Current == ConnectionState::Connected)
				RawConnection = Changed->Connection;
		Check(RawConnection.has_value(), "raw transport exposes its generation-safe connection identity");
		if (RawConnection) {
			std::vector<std::byte> Premature{
				std::byte{'G'},
				std::byte{'C'},
				std::byte{'H'},
				std::byte{'R'},
				std::byte{0},
			};
			auto Intent = MakeNetworkMessageIntent(
				*RawConnection,
				DeliveryMode::ReliableOrdered,
				TrafficClass::ReliableApplication,
				{},
				std::move(Premature),
				ServerConfiguration.Limits
			);
			Check(Intent && RawTransport->Send(*Intent).Succeeded(), "raw transport sends a premature gameplay frame");
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
		}
		Check(
			Server.GetMetrics().RejectedPreAcceptanceMessages == 1 && ServerRuntime.Players->GetPlayers().empty(),
			"gameplay before GSES acceptance is rejected without creating a Player"
		);
		Server.Stop();
		(void)RawTransport->Stop({DisconnectReason::LocalShutdown, "hostile-session complete"});
		ServerRuntime.Destroy();

		Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		ServerTransport = Network->CreateTransport();
		RawTransport = Network->CreateTransport();
		ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer TimeoutRenderer(Vector2(320, 240));
		Engine TimeoutRuntime(
			ServerWorld,
			&TimeoutRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		TimeoutRuntime.ProcessService->Alive = true;
		GameSession TimeoutServer(ServerTransport, ServerConfiguration, &TimeoutRuntime);
		Check(
			TimeoutServer.Start().Succeeded() && RawTransport
													 ->Start({
														 .Role = TransportRole::Client,
														 .Endpoint = ServerConfiguration.Endpoint,
														 .AdvertisedLimits = ServerConfiguration.Limits,
													 })
													 .Succeeded(),
			"silent peer reaches the timeout fixture"
		);
		(void)Network->Advance(2ms);
		Network->Pump();
		(void)TimeoutServer.Poll();
		TimeoutServer.Step(3);
		Check(
			TimeoutServer.GetMetrics().HandshakeTimeouts == 1 && TimeoutRuntime.Players->GetPlayers().empty(),
			"silent handshake expires without allocating a Player identity"
		);
		TimeoutServer.Stop();
		(void)RawTransport->Stop({DisconnectReason::LocalShutdown, "timeout fixture complete"});
		TimeoutRuntime.Destroy();
	}

	void TestSpoofedReadyPlayerRejection() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto RawTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "spoofed-ready-session");
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(
			Server.Start().Succeeded() && RawTransport
											  ->Start({
												  .Role = TransportRole::Client,
												  .Endpoint = ServerConfiguration.Endpoint,
												  .AdvertisedLimits = ServerConfiguration.Limits,
											  })
											  .Succeeded(),
			"raw peer reaches the spoofed readiness fixture"
		);
		Network->Pump();
		(void)Server.Poll();
		std::array<TransportEvent, 16> RawEvents;
		auto EventCount = RawTransport->PollEvents(RawEvents);
		std::optional<ConnectionId> RawConnection;
		for (std::size_t Index = 0; Index < EventCount; ++Index)
			if (const auto *Changed = std::get_if<ConnectionStateEvent>(&RawEvents[Index]);
				Changed && Changed->Current == ConnectionState::Connected)
				RawConnection = Changed->Connection;
		Check(RawConnection.has_value(), "spoofed readiness fixture obtains its connection identity");
		if (!RawConnection) {
			Server.Stop();
			ServerRuntime.Destroy();
			return;
		}

		auto Hello = EncodeGameSessionMessage(GameSessionClientHello{0xabcdef12345678ull, ServerConfiguration.Limits});
		auto HelloIntent = Hello ? MakeNetworkMessageIntent(
									   *RawConnection,
									   DeliveryMode::ReliableOrdered,
									   TrafficClass::Control,
									   {},
									   std::move(*Hello),
									   ServerConfiguration.Limits
								   )
								 : std::nullopt;
		Check(
			HelloIntent && RawTransport->Send(*HelloIntent).Succeeded(), "raw peer submits a valid bounded client hello"
		);
		(void)Network->Advance(2ms);
		Network->Pump();
		(void)Server.Poll();
		Server.Step(1);
		(void)Network->Advance(2ms);
		Network->Pump();
		EventCount = RawTransport->PollEvents(RawEvents);
		std::optional<GameSessionServerAccepted> Accepted;
		for (std::size_t Index = 0; Index < EventCount; ++Index) {
			const auto *Received = std::get_if<ReceivedMessageEvent>(&RawEvents[Index]);
			if (!Received || !IsGameSessionFrame(Received->Payload)) continue;
			auto Decoded = DecodeGameSessionMessage(Received->Payload);
			if (Decoded)
				if (const auto *Value = std::get_if<GameSessionServerAccepted>(&*Decoded)) Accepted = *Value;
		}
		Check(Accepted.has_value(), "server returns an accepted identity to the raw peer");
		if (Accepted) {
			auto SpoofedPlayer = Accepted->Player;
			++SpoofedPlayer.Slot;
			auto Ready = EncodeGameSessionMessage(
				GameSessionClientReady{Accepted->SessionEpoch, Accepted->Replication, SpoofedPlayer}
			);
			auto ReadyIntent = Ready ? MakeNetworkMessageIntent(
										   *RawConnection,
										   DeliveryMode::ReliableOrdered,
										   TrafficClass::Control,
										   {},
										   std::move(*Ready),
										   Accepted->NegotiatedLimits
									   )
									 : std::nullopt;
			Check(
				ReadyIntent && RawTransport->Send(*ReadyIntent).Succeeded(),
				"raw peer submits readiness for a different Player ObjectId"
			);
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
		}
		Check(
			Server.GetMetrics().ProtocolRejects == 1 && Server.GetMetrics().ReadyPeers == 0 &&
				ServerRuntime.Players->GetPlayers().empty(),
			"spoofed Player readiness tears down the partially admitted Player without gameplay authority"
		);
		Server.Stop();
		(void)RawTransport->Stop({DisconnectReason::LocalShutdown, "spoofed readiness complete"});
		ServerRuntime.Destroy();
	}

	void TestBurstAdmissionKeepsJournalCursorsCurrent() {
		constexpr std::size_t PeerCount = 192;
		struct RawPeer {
			std::shared_ptr<SimulatedTransport> Transport;
			ConnectionId Connection;
			bool ReadySent = false;
		};
		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		TransportConfiguration.MaximumTransports = PeerCount + 1;
		TransportConfiguration.MaximumConnections = PeerCount;
		TransportConfiguration.MaximumPendingEventsPerTransport = PeerCount * 4;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(64, 64));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "burst-admission");
		ServerConfiguration.StructuralReplication.MaximumTransitionsPerPeerTick = 64;
		ServerConfiguration.StructuralReplication.MaximumTransitionsPerTick = 256;
		ServerConfiguration.StructuralReplication.PeerQuantum = 64;
		ServerConfiguration.StructuralReplication.MaximumJournalRecordsPerPeerTick = 128;
		ServerConfiguration.StructuralReplication.MaximumJournalRecordsPerTick = 512;
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(Server.Start().Succeeded(), "burst-admission server starts");
		std::vector<RawPeer> Peers;
		Peers.reserve(PeerCount);
		for (std::size_t Index = 0; Index < PeerCount; ++Index) {
			auto Transport = Network->CreateTransport();
			Check(
				Transport && Transport
								 ->Start({
									 .Role = TransportRole::Client,
									 .Endpoint = ServerConfiguration.Endpoint,
									 .AdvertisedLimits = ServerConfiguration.Limits,
								 })
								 .Succeeded(),
				"burst-admission raw client starts"
			);
			Network->Pump();
			(void)Server.Poll();
			std::array<TransportEvent, 8> Events;
			const auto Count = Transport->PollEvents(Events);
			ConnectionId Connection;
			for (std::size_t EventIndex = 0; EventIndex < Count; ++EventIndex)
				if (const auto *Changed = std::get_if<ConnectionStateEvent>(&Events[EventIndex]);
					Changed && Changed->Current == ConnectionState::Connected)
					Connection = Changed->Connection;
			Check(Connection.IsValid(), "burst-admission raw client receives a connection identity");
			auto Hello = EncodeGameSessionMessage(
				GameSessionClientHello{static_cast<std::uint64_t>(Index + 1), ServerConfiguration.Limits}
			);
			auto Intent = Hello ? MakeNetworkMessageIntent(
									  Connection,
									  DeliveryMode::ReliableOrdered,
									  TrafficClass::Control,
									  {},
									  std::move(*Hello),
									  ServerConfiguration.Limits
								  )
								: std::nullopt;
			Check(Intent && Transport->Send(*Intent).Succeeded(), "burst-admission raw client submits hello");
			Peers.push_back({std::move(Transport), Connection});
		}
		std::uint64_t LastAdmissionTick = 0;
		for (std::uint64_t Tick = 1; Tick <= 80 && Server.GetMetrics().ReadyPeers != PeerCount; ++Tick) {
			LastAdmissionTick = Tick;
			(void)Network->Advance(1ms);
			Network->Pump();
			(void)Server.Poll();
			Server.Step(Tick);
			(void)Network->Advance(1ms);
			Network->Pump();
			for (auto &Peer : Peers) {
				std::array<TransportEvent, 16> Events;
				const auto Count = Peer.Transport->PollEvents(Events);
				for (std::size_t EventIndex = 0; EventIndex < Count && !Peer.ReadySent; ++EventIndex) {
					const auto *Received = std::get_if<ReceivedMessageEvent>(&Events[EventIndex]);
					if (!Received || !IsGameSessionFrame(Received->Payload)) continue;
					auto Decoded = DecodeGameSessionMessage(Received->Payload);
					const auto *Accepted = Decoded ? std::get_if<GameSessionServerAccepted>(&*Decoded) : nullptr;
					if (!Accepted) continue;
					auto Ready = EncodeGameSessionMessage(
						GameSessionClientReady{Accepted->SessionEpoch, Accepted->Replication, Accepted->Player}
					);
					auto ReadyIntent = Ready ? MakeNetworkMessageIntent(
												   Peer.Connection,
												   DeliveryMode::ReliableOrdered,
												   TrafficClass::Control,
												   {},
												   std::move(*Ready),
												   Accepted->NegotiatedLimits
											   )
											 : std::nullopt;
					Check(
						ReadyIntent && Peer.Transport->Send(*ReadyIntent).Succeeded(),
						"burst-admission raw client submits readiness"
					);
					Peer.ReadySent = true;
				}
			}
		}
		const auto Metrics = Server.GetMetrics();
		std::cout << "[Network:BurstTest] ready=" << Metrics.ReadyPeers << " accepted=" << Metrics.AcceptedPeers
			<< " removed=" << Metrics.PlayersRemoved << " rejected=" << Metrics.ProtocolRejects
			<< " selected=" << Metrics.StructuralTransitionsSelected << " committed=" << Metrics.StructuralTransitionsCommitted
			<< " cap=" << Metrics.StructuralMaximumTransitionsSelectedPerTick
			<< " capTicks=" << Metrics.StructuralGlobalBudgetExhaustions << " ticks=" << LastAdmissionTick << '\n';
		Check(
			Metrics.ReadyPeers == PeerCount && Metrics.AcceptedPeers == PeerCount && Metrics.PlayersRemoved == 0 &&
				Metrics.ProtocolRejects == 0 && ServerRuntime.Players->GetPlayers().size() == PeerCount &&
				Metrics.StructuralMaximumTransitionsSelectedPerTick <=
					ServerConfiguration.StructuralReplication.MaximumTransitionsPerTick &&
				Metrics.StructuralTransitionsSelected == Metrics.StructuralTransitionsCommitted &&
				Metrics.StructuralGlobalBudgetExhaustions != 0,
			"bounded fair burst admission keeps every peer current without exceeding the global structural cap"
		);
		auto DrainJournalTick = [&](std::uint64_t Tick) {
			(void)Network->Advance(1ms); Network->Pump(); (void)Server.Poll(); Server.Step(Tick);
			(void)Network->Advance(1ms); Network->Pump();
			for (auto &Peer : Peers) {
				std::array<TransportEvent, 128> Events;
				(void)Peer.Transport->PollEvents(Events);
			}
		};
		std::uint64_t Tick = LastAdmissionTick;
		for (std::size_t Iteration = 0; Iteration < 1024 && Server.GetMetrics().JournalBacklogRecords != 0; ++Iteration)
			DrainJournalTick(++Tick);
		Check(Server.GetMetrics().JournalBacklogRecords == 0, "bounded journal service drains bootstrap history for every peer");
		// Inject ordinary non-replicated journal records. Archivable itself does
		// not journal, so toggling that property would not exercise this reader.
		for (std::size_t Index = 0; Index < 32; ++Index)
			ChangeJournal::Get().Commit(ServerWorld->GetObjectId(), ServerWorld->GetObjectId(),
				PropertyUpdatedChange{"Archivable", WireValue(Index % 2 != 0), false});
		const auto BeforeJournal = Server.GetMetrics();
		std::size_t JournalTicks = 0;
		do { DrainJournalTick(++Tick); ++JournalTicks; }
		while (Server.GetMetrics().JournalBacklogRecords != 0 && JournalTicks < 64);
		const auto AfterJournal = Server.GetMetrics();
		Check(AfterJournal.JournalBacklogRecords == 0 && AfterJournal.ReadyPeers == PeerCount &&
			AfterJournal.JournalRecordsPerTickHighWater == 512 && AfterJournal.JournalRecordsPerPeerTickHighWater <= 128 &&
			AfterJournal.JournalRecordsExamined - BeforeJournal.JournalRecordsExamined == 32 * PeerCount && JournalTicks > 1 &&
			AfterJournal.StructuralJournalLagFailures == 0,
			"global journal budget rotates fairly across no-frame readers and preserves retention");
		std::cout << "[Network:JournalBudgetTest] ticks=" << JournalTicks << " read="
			<< AfterJournal.JournalRecordsExamined - BeforeJournal.JournalRecordsExamined
			<< " globalHighWater=" << AfterJournal.JournalRecordsPerTickHighWater
			<< " peerHighWater=" << AfterJournal.JournalRecordsPerPeerTickHighWater
			<< " remaining=" << AfterJournal.JournalBacklogRecords << '\n';
		for (auto &Peer : Peers)
			(void)Peer.Transport->Stop({DisconnectReason::LocalShutdown, "burst-admission complete"});
		Server.Stop();
		ServerRuntime.Destroy();
	}

	void TestRejectedStructuralAdmissionIsPeerTerminal(bool WithByteAdmission = false) {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto ClientTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(64, 64));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "structural-rejection");
		if (WithByteAdmission) {
			ServerConfiguration.ReliableService.emplace();
			auto &Profile = *ServerConfiguration.ReliableService;
			Profile.ConnectionRate = Profile.AggregateRate = 8 * 1024 * 1024;
			Profile.BackendRate = 16 * 1024 * 1024;
		}
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		GameSession Client(ClientTransport, Configuration(GameSessionRole::Client, "structural-rejection"));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "structural-rejection session starts");
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		std::uint64_t Tick = 1;
		for (; Tick <= 100 && Server.GetMetrics().ReadyPeers != 1; ++Tick) {
			if (WithByteAdmission) std::this_thread::sleep_for(1ms);
			Advance(Network, Server, Client, Tick);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
				ClientRuntime = std::make_unique<Engine>(
					Client.GetClientDataModel(),
					ClientRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "structural-rejection client runtime attaches");
			}
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
		}
		Check(
			Server.GetMetrics().ReadyPeers == 1 && ServerRuntime.Players->GetPlayers().size() == 1,
			"structural-rejection fixture reaches an owned ready peer"
		);
		detail::GameSessionTestAccess::SetFailurePoint(
			Server, detail::GameSessionFailurePoint::StructuralSchedulerAdmission
		);
		auto Mutation = std::make_shared<Script>();
		Mutation->SetName("TerminalStructuralMutation");
		Mutation->SetParent(ServerWorld);
		if (WithByteAdmission) std::this_thread::sleep_for(2ms);
		Server.Step(Tick);
		Check(
			Server.GetStatus() == GameSessionStatus::Listening && Server.GetMetrics().PlayersRemoved == 1 &&
				ServerRuntime.Players->GetPlayers().empty(),
			"rejected reliable structural admission tears down its Player and peer in the same processing cycle"
		);
		if (WithByteAdmission) {
			const auto Metrics = Server.GetMetrics();
			Check(Metrics.ReliableAdmission.AcceptedBytes > 0 && Metrics.ReliableAdmission.RolledBackBytes > 0 &&
				Metrics.ReliableAdmissionPeerStates == 0, "failed scheduler admission refunds reservation and clears generation-safe accounting");
		}
		Client.Stop();
		Server.Stop();
		host::detail::FarmLifecycleEvidence ClientStopped, ServerStopped;
		ClientStopped.ObserveSession(Client);
		ServerStopped.ObserveSession(Server);
		Check(ClientStopped.Valid() && ServerStopped.Valid(),
			"connected production lifecycle leaves exact terminal session/admission/readers snapshots");
		if (ClientRuntime) ClientRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestAcceptedConnectionChurn() {
		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BaseLatency = 1ms;
		TransportConfiguration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		TransportConfiguration.MaximumTransports = 128;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "session-churn");
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(Server.Start().Succeeded(), "connection churn server starts");
		std::uint64_t Tick = 1;
		for (std::uint64_t Cycle = 1; Cycle <= 100; ++Cycle) {
			auto ClientTransport = Network->CreateTransport();
			GameSession Client(ClientTransport, Configuration(GameSessionRole::Client, "session-churn"));
			Check(Client.Start().Succeeded(), "connection churn client starts");
			for (std::uint64_t Attempt = 1; Attempt <= 120 && Server.GetMetrics().AcceptedPeers != Cycle;
				 ++Attempt, ++Tick) {
				(void)Network->Advance(2ms);
				Network->Pump();
				(void)Server.Poll();
				(void)Client.Poll();
				Server.Step(Tick);
				Client.Step(Attempt);
			}
			Check(Server.GetMetrics().AcceptedPeers == Cycle, "connection churn peer is admitted");
			Client.Stop();
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
			Server.Step(Tick++);
			Check(ServerRuntime.Players->GetPlayers().empty(), "connection churn removes its authoritative Player");
		}
		const auto Metrics = Server.GetMetrics();
		std::cout << "[Network:SessionTest] churnAccepted=" << Metrics.AcceptedPeers
				  << " created=" << Metrics.PlayersCreated << " removed=" << Metrics.PlayersRemoved
				  << " handshakeRejected=" << Metrics.RejectedHandshakes
				  << " protocolRejected=" << Metrics.ProtocolRejects << " timeouts=" << Metrics.HandshakeTimeouts
				  << '\n';
		Check(
			Metrics.AcceptedPeers == 100 && Metrics.PlayersCreated == 100 && Metrics.PlayersRemoved == 100 &&
				ServerRuntime.Players->GetPlayers().empty(),
			"one hundred accepted connection lifetimes return Player gauges to baseline"
		);
		Server.Stop();
		ServerRuntime.Destroy();
	}

	void TestProductionLifecycleComposition(
		bool MeasureHandoff = false, bool WithByteAdmission = false, bool TestControlRebind = false,
		bool SessionActionPredictable = true, bool WithReplicatedTemplate = false
	) {
		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BaseLatency = 1ms;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = std::make_shared<HandoffTransport>(Network->CreateTransport());
		auto ClientTransport = Network->CreateTransport();
		Check(Network && ServerTransport && ClientTransport, "session test allocates bounded transports");
		if (!Network || !ServerTransport || !ClientTransport) return;

		auto ServerWorld = std::make_shared<DataModel>();
		if (WithReplicatedTemplate) {
			auto Storage = ServerWorld->GetService("ReplicatedStorage");
			auto Templates = std::make_shared<Folder>();
			Templates->SetName("StoredCharacterTemplates");
			Templates->SetParent(Storage);
			auto Template = std::make_shared<KinematicCharacter>();
			Template->SetName("StoredCharacter");
			Template->SetParent(Templates);
			auto Root = std::make_shared<MeshPart>();
			Root->SetName("StoredRootPart");
			Root->SetAnchored(true);
			Root->SetParent(Template);
			Template->SetRootPart(Root);
			auto AnimatorValue = std::make_shared<Animator>();
			AnimatorValue->SetParent(Root);
		}
		if (TestControlRebind) {
			auto Marker = std::make_shared<Folder>();
			Marker->SetName("ControlRebindMarker");
			Marker->SetParent(ServerWorld);
		}
		DiskFilesystem SampleFilesystem(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT));
		auto ServerAssets = std::dynamic_pointer_cast<AssetService>(ServerWorld->GetService("AssetService"));
		ServerAssets->LoadProjectAssets(SampleFilesystem);
		const auto RuntimeAssets = ServerAssets->CaptureRuntimeAssets();
		auto ServerActionPolicy = std::make_shared<Script>();
		auto SessionRemote = std::make_shared<RemoteEvent>();
		SessionRemote->SetName("SessionRemote");
		SessionRemote->SetParent(ServerWorld);
		ServerActionPolicy->SetName("SessionActionPolicy");
		ServerActionPolicy->SetRunContext(Enums::RunContext::Server);
		ServerActionPolicy->SetSource(std::string("local SessionActionPredictable = ") +
			(SessionActionPredictable ? "true\n" : "false\n") + R"(
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local SessionRemote = game:FindFirstChild("SessionRemote")
SessionRemote.OnServerEvent:Connect(function(Peer, Message)
	if type(Peer) == "table" and Message == "session-ready" then
		CharacterControl:SetAttribute("SessionRemoteReceived", true)
		SessionRemote:FireClient(Peer.Slot, Peer.Generation, "service-probe", 1)
		SessionRemote:FireClient(Peer.Slot, Peer.Generation, "service-probe", 2)
	end
end)
local function InstallPresentationRig(Player)
	local Character = Player.Character
	if Character == nil or Character:FindFirstChild("SessionRig") ~= nil then
		return
	end
	local Rig = Instance.new("MeshPart")
	Rig.Name = "SessionRig"
	Rig.Mesh = "asset://d549080bd1e64aaee8041f4ece3e9f75"
	Rig.Anchored = true
	Rig.CanCollide = false
	Rig.CanTouch = false
	Rig.Size = Vector3.new(2, 2, 2)
	Rig.CFrame = Character.CFrame
	Rig.Parent = Character
	local Animator = Instance.new("Animator")
	Animator.Name = "SessionAnimator"
	Animator.Parent = Rig
end
Players.PlayerAdded:Connect(InstallPresentationRig)
assert(CharacterControl:RegisterAction(
	"SessionLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	SessionActionPredictable
))
assert(CharacterControl:RegisterAction(
	"DeniedLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetAttribute("ServerActionRegistered", true)
CharacterControl:SetActionPolicy(function(Player, Character, ActionName)
	local Accepted = ActionName == "SessionLunge" and Player.Character == Character
	if ActionName == "SessionLunge" then
		Character:SetAttribute("SessionActionValidated", Accepted)
	elseif ActionName == "DeniedLunge" then
		Character:SetAttribute("DeniedActionObserved", true)
	end
	return Accepted
end)
)");
		ServerActionPolicy->SetParent(ServerWorld);
		auto ClientActionPolicy = std::make_shared<Script>();
		ClientActionPolicy->SetName("SessionActionRequest");
		ClientActionPolicy->SetRunContext(Enums::RunContext::Client);
		ClientActionPolicy->SetSource(std::string("local SessionActionPredictable = ") +
			(SessionActionPredictable ? "true\n" : "false\n") + R"(
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local SessionRemote = game:FindFirstChild("SessionRemote")
SessionRemote.OnClientEvent:Connect(function(Message, Sequence)
	if Message == "service-probe" and (Sequence == 1 or Sequence == 2) then
		CharacterControl:SetAttribute("ServiceProbeReplies", (CharacterControl:GetAttribute("ServiceProbeReplies") or 0) + 1)
	end
end)
assert(CharacterControl:RegisterAction(
	"SessionLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	SessionActionPredictable
))
assert(CharacterControl:RegisterAction(
	"DeniedLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetAttribute("ClientActionRegistered", true)
local Resolved = false
local DeniedDelay = 0
CharacterControl.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if ActionName == "SessionLunge" and Accepted then
		Resolved = true
		Character:SetAttribute("SessionActionResolved", true)
		DeniedDelay = 32
	elseif ActionName == "DeniedLunge" and not Accepted then
		Character:SetAttribute("DeniedActionRejected", true)
	end
end)
CharacterControl.ActionEnded:Connect(function(Character, ActionName)
	if ActionName == "SessionLunge" then
		Character:SetAttribute("SessionActionEnded", true)
	end
end)
local Requested = false
local RemoteSent = false
local SawRootAbsent = false
local ReboundActionRequested = false
RunService.PreSimulation:Connect(function()
	if DeniedDelay > 0 then
		DeniedDelay -= 1
		if DeniedDelay == 0 and Players.LocalPlayer and Players.LocalPlayer.Character then
			if CharacterControl:RequestAction("DeniedLunge") then
				Players.LocalPlayer.Character:SetAttribute("DeniedActionRequested", true)
			end
		end
	end
	if not Requested and Players.LocalPlayer and Players.LocalPlayer.Character then
		Requested = CharacterControl:RequestAction("SessionLunge")
		if Requested then
			Players.LocalPlayer.Character:SetAttribute("SessionActionRequested", true)
		end
	end
	if not RemoteSent and Players.LocalPlayer and Players.LocalPlayer.Character then
		SessionRemote:FireServer("session-ready")
		RemoteSent = true
	end
	local Character = Players.LocalPlayer and Players.LocalPlayer.Character
	if Requested and Character and game:FindFirstChild("ControlRebindMarker") then
		if Character.RootPart == nil then
			SawRootAbsent = true
		elseif SawRootAbsent and not ReboundActionRequested then
			ReboundActionRequested = CharacterControl:RequestAction("SessionLunge")
			if ReboundActionRequested then
				Character:SetAttribute("ReboundActionRequested", true)
			end
		end
	end
end)
)");
		ClientActionPolicy->SetParent(ServerWorld);
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			[](std::string Code, std::string Message) {
				if (Code == "Error") std::cerr << "[Network:SessionTest] " << Message << '\n';
			},
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server);
		if (WithByteAdmission) {
			ServerConfiguration.ReliableService.emplace();
			auto &Profile = *ServerConfiguration.ReliableService;
			Profile.ConnectionRate = Profile.AggregateRate = 8 * 1024 * 1024;
			Profile.BackendRate = 16 * 1024 * 1024;
			Profile.RequireLatencyCompatibility = true;
		}
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		GameSession Client(ClientTransport, Configuration(GameSessionRole::Client));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "production session endpoints start");
		std::shared_ptr<Folder> HandoffMarker;
		if (MeasureHandoff) {
			HandoffMarker = std::make_shared<Folder>();
			HandoffMarker->SetParent(ServerWorld);
		}
		std::size_t LateCharacterTicks = 0, LateRemoteTicks = 0;
		auto TraceAdvance = [&](std::uint64_t Tick) {
			if (WithByteAdmission) std::this_thread::sleep_for(1ms);
			if (!MeasureHandoff) { Advance(Network, Server, Client, Tick); return; }
			// A small real structural mutation ensures the early flush precedes
			// Character authority and the ordinary Luau Remote pump in this step.
			HandoffMarker->SetName("Handoff" + std::to_string(Tick));
			runtime_detail::WorkSample Sample{};
			ServerTransport->Begin();
			Advance(Network, Server, Client, Tick, &Sample);
			const auto Limits = GameSessionConfiguration::DefaultLimits();
			Check(ServerTransport->Bytes <= Limits.MaximumSendBytesPerTick && ServerTransport->Messages <= Limits.MaximumMessagesPerTick,
				"all service points share the negotiated per-step transport allowance");
			const auto &Structural = Sample.Producers[static_cast<std::size_t>(runtime_detail::WorkProducer::Structural)];
			if (Structural.Serviced == 0) return;
			for (const auto Producer : {runtime_detail::WorkProducer::Gchr, runtime_detail::WorkProducer::RemoteEventReliable}) {
				const auto &Value = Sample.Producers[static_cast<std::size_t>(Producer)];
				if (Value.Enqueued == 0) continue;
				Check(ServerTransport->Fresh[static_cast<std::size_t>(Producer)] >= Value.Enqueued,
					"late-produced gameplay reaches actual transport in the same step after drained structural service");
				if (Producer == runtime_detail::WorkProducer::Gchr) ++LateCharacterTicks;
				else ++LateRemoteTicks;
				// Serviced is recorded only after the real transport Send succeeds,
				// not after local scheduler acceptance. At most 120 test rows.
				std::cout << "[Network:LateHandoff] tick=" << Tick << " producer="
					<< runtime_detail::WorkProducerNames[static_cast<std::size_t>(Producer)]
					<< " accepted=" << Value.Enqueued << " sent=" << Value.Serviced
					<< " sentAfterProduction=" << ServerTransport->Fresh[static_cast<std::size_t>(Producer)]
					<< " handoffMaxMs=" << Value.ServiceAgeMaximumNanoseconds / 1e6
					<< " flushes=" << Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::SchedulerFlush)].Calls << '\n';
			}
		};

		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		for (std::uint64_t Tick = 1; Tick <= 80; ++Tick) {
			TraceAdvance(Tick);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				auto ClientAssets = std::dynamic_pointer_cast<AssetService>(
					Client.GetClientDataModel()->GetService("AssetService")
				);
				ClientAssets->LoadRuntimeAssetSnapshot(RuntimeAssets);
				Check(
					PackageBuilder::HydrateClientCode(ServerWorld, Client.GetClientDataModel()) >= 1,
					"trusted packaged client code hydrates the authoritative replicated graph"
				);
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				ClientRuntime = std::make_unique<Engine>(
					Client.GetClientDataModel(),
					ClientRenderer.get(),
					[](std::string Code, std::string Message) {
						if (Code == "Error") std::cerr << "[Network:SessionTest] " << Message << '\n';
					},
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				ClientRuntime->ProcessService->Alive = true;
				Check(
					Client.AttachClientRuntime(*ClientRuntime),
					"client gameplay bridge attaches after trusted bootstrap"
				);
			}
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
			if (Server.GetMetrics().ReadyPeers == 1) break;
		}

		Check(
			ClientRuntime && Client.GetStatus() == GameSessionStatus::Ready,
			"client reaches ready only after replicated runtime attachment"
		);
		if (WithReplicatedTemplate && Client.GetClientDataModel()) {
			auto Storage = Client.GetClientDataModel()->GetService("ReplicatedStorage");
			auto Templates = Storage ? Storage->FindFirstChild("StoredCharacterTemplates", false) : nullptr;
			auto Template = Templates ? std::dynamic_pointer_cast<KinematicCharacter>(
			Templates->FindFirstChild("StoredCharacter", false)) : nullptr;
			Check(Template && Template->GetRootPart() &&
				std::dynamic_pointer_cast<MeshPart>(*Template->GetRootPart()) &&
				(*Template->GetRootPart())->FindFirstChildOfClass("Animator", false),
				"nested ReplicatedStorage Character template keeps its typed RootPart and Animator in the structural replica");
		}
		Check(
			Server.GetMetrics().AcceptedPeers == 1 && Server.GetMetrics().ReadyPeers == 1,
			"server distinguishes accepted and gameplay-ready peer phases"
		);
		auto ServerPlayers = ServerRuntime.Players->GetPlayers();
		Check(ServerPlayers.size() == 1, "server creates exactly one authoritative Player for the accepted peer");
		Check(
			ServerPlayers.empty() || (ServerPlayers[0]->GetAuthenticationIdentity() &&
									  ServerPlayers[0]->GetAuthenticationIdentity()->Provider == "session-development"),
			"3D labels development-local identity without claiming external authentication"
		);
		Check(
			ServerPlayers.empty() || ServerPlayers[0]->GetCharacter().has_value(),
			"server-shipped Luau owns automatic Character assembly"
		);
		Check(
			ClientRuntime && ClientRuntime->Players->GetLocalPlayer().has_value(),
			"client establishes LocalPlayer from the trusted server ObjectId"
		);
		if (ClientRuntime && ClientRuntime->Players->GetLocalPlayer()) {
			auto LocalPlayer = *ClientRuntime->Players->GetLocalPlayer();
			Check(LocalPlayer->GetCharacter().has_value(), "client materializes the server-owned Player.Character");
			Check(LocalPlayer->GetPlayerId() == 1, "session PlayerId survives structural replication");
		}
		Check(
			Server.GetMetrics().CharacterControlBindings == 1,
			"Player.Character drives one authoritative Character control binding"
		);
		if (ClientRuntime && !ServerPlayers.empty() && ServerPlayers[0]->GetCharacter()) {
			auto ServerCharacter = std::dynamic_pointer_cast<KinematicCharacter>(*ServerPlayers[0]->GetCharacter());
			const auto InitialPosition = ServerCharacter ? ServerCharacter->GetPosition() : glm::vec3{};
			for (std::uint64_t Tick = 81; Tick <= 120; ++Tick) {
				ClientRuntime->Step();
				ServerRuntime.Step();
				TraceAdvance(Tick);
			}
			Check(!MeasureHandoff || (LateCharacterTicks > 0 && LateRemoteTicks > 0),
				"late-handoff fixture observes actual Character and Remote acceptance after structural service");
			if (MeasureHandoff) {
				// This variant proves service ordering, not the original timed action
				// presentation scenario. The unchanged scenario runs separately below.
				Check(Server.GetMetrics().ReadyPeers == 1 && Client.GetStatus() == GameSessionStatus::Ready,
					"recorded handoff workload retains the live authenticated session");
				Client.Stop();
				Server.Stop();
				ClientRuntime->Destroy();
				ServerRuntime.Destroy();
				return;
			}
			if (ServerCharacter)
				std::cout << "[Network:SessionTest] actionDeltaX="
						  << ServerCharacter->GetPosition().x - InitialPosition.x
						  << " validated=" << ServerCharacter->GetAttributeValue("SessionActionValidated").has_value()
						  << '\n';
			if (auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				LocalPlayer && (*LocalPlayer)->GetCharacter())
				std::cout
					<< "[Network:SessionTest] actionRequested="
					<< (*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionRequested").has_value()
					<< " resolved="
					<< (*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionResolved").has_value()
					<< '\n';
			std::cout << "[Network:SessionTest] clientActionRegistered="
					  << ClientRuntime->CharacterControl->GetAttributeValue("ClientActionRegistered").has_value()
					  << '\n';
			Check(
				ServerCharacter && ServerCharacter->GetAttributeValue("SessionActionValidated").has_value(),
				"server Luau validates the semantic action against the authoritative Player.Character"
			);
			Check(
				ServerCharacter && ServerCharacter->GetAttributeValue("DeniedActionObserved").has_value(),
				"server Luau observes and rejects a registered production action"
			);
			Check(
				ServerRuntime.CharacterControl->GetAttributeValue("SessionRemoteReceived").has_value(),
				"accepted production peer routes an ordinary reliable RemoteEvent over the shared session lifetime"
			);
			Check(
				Client.GetMetrics().ActionsPresented >= 1 && Client.GetMetrics().ActionPresentationStops >= 1,
				"client starts and terminates bounded action presentation from authoritative phase state"
			);
			if (WithReplicatedTemplate)
				Check(Client.GetMetrics().ClientMaterializationEpochMismatches == 0,
					"stored Character template does not advance the runtime GCHR materialization epoch");
			if (auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				LocalPlayer && (*LocalPlayer)->GetCharacter()) {
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionRequested").has_value(),
					"client game Luau requests the action through the high-level Character bridge"
				);
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionResolved").has_value(),
					"accepted authoritative action resolution returns to client game Luau"
				);
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("DeniedActionRequested").has_value() &&
						(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("DeniedActionRejected").has_value(),
					"rejected authoritative action terminates through the semantic client result bridge"
				);
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionEnded").has_value(),
					"authoritative action lifetime exposes a semantic end independent of visual completion"
				);
			}
			Check(
				ServerCharacter && ServerCharacter->GetPosition().x > InitialPosition.x + 0.4f,
				"game-defined semantic action crosses the production session and uses server-chosen pinned root motion"
			);
			const auto BeforeInput = ServerCharacter ? ServerCharacter->GetPosition() : glm::vec3{};
			(void)ClientRuntime->ProcessEvent(
				KeyEvent{
					.Device = {1},
					.Physical = PhysicalKey::W,
					.Logical = LogicalKey::W,
					.State = ButtonState::Pressed,
				}
			);
			for (std::uint64_t Tick = 121; Tick <= 160; ++Tick) {
				ClientRuntime->Step();
				ServerRuntime.Step();
				Advance(Network, Server, Client, Tick);
			}
			Check(
				ServerCharacter && glm::distance(ServerCharacter->GetPosition(), BeforeInput) > 0.25f,
				"ordinary ActionMap policy reaches authoritative Character movement through the native bridge"
			);
			const auto ServiceMetrics = Client.GetMetrics();
			Check(ServiceMetrics.ClientCharacterMessagesHandled > 1 && ServiceMetrics.ClientRemoteMessagesHandled == 2 &&
				ServiceMetrics.ClientCharacterMaximumServiceGapNanoseconds > 0 &&
				ServiceMetrics.ClientRemoteMaximumServiceGapNanoseconds > 0 &&
				ClientRuntime->CharacterControl->GetAttributeValue("ServiceProbeReplies") == std::optional<WireValue>(2.0),
				"client service metrics observe validated Character and Remote processing without changing delivery");
			Check(host::detail::DecodePhysicalScaleCounter(
				ClientRuntime->CharacterControl->GetAttributeValue("ServiceProbeReplies")) == 2,
				"Luau SetAttribute numeric output is readable by the scale count gate");
			std::uint64_t ReplacementTick = 161;
			if (TestControlRebind && ServerCharacter && ServerCharacter->GetRootPart()) {
				const auto CharacterId = ServerCharacter->GetObjectId();
				const auto RootPart = *ServerCharacter->GetRootPart();
				const auto RootId = RootPart->GetObjectId();
				const auto InitialLocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				const auto InitialLocalCharacter = InitialLocalPlayer && (*InitialLocalPlayer)->GetCharacter()
					? *(*InitialLocalPlayer)->GetCharacter() : nullptr;
				const auto InitialLocalRoot = InitialLocalCharacter ? InitialLocalCharacter->GetRootPart() : std::nullopt;
				const auto LocalCharacterId = InitialLocalCharacter ? InitialLocalCharacter->GetObjectId() : ObjectId{};
				const auto LocalRootId = InitialLocalRoot ? (*InitialLocalRoot)->GetObjectId() : ObjectId{};
				Check(LocalCharacterId.IsValid() && LocalRootId.IsValid(),
					"client begins with a materialized owner Character and RootPart in its own ObjectId scope");
				const auto BindingsBefore = Server.GetMetrics().CharacterControlBindings;
				const auto RevocationsBefore = Server.GetMetrics().CharacterControlRevocations;
				const auto ActionsBefore = detail::GameSessionTestAccess::GetCharacterMetrics(Server).ActionRequestsAccepted;
				ServerCharacter->SetRootPart(std::nullopt);
				for (std::uint64_t Tick = 161; Tick <= 190; ++Tick) {
					ClientRuntime->Step();
					ServerRuntime.Step();
					Advance(Network, Server, Client, Tick);
				}
				const auto LocalPlayerDuringRevoke = ClientRuntime->Players->GetLocalPlayer();
				const auto LocalCharacterDuringRevoke = LocalPlayerDuringRevoke && (*LocalPlayerDuringRevoke)->GetCharacter()
					? *(*LocalPlayerDuringRevoke)->GetCharacter() : nullptr;
				Check(ServerPlayers[0]->GetCharacter() && (*ServerPlayers[0]->GetCharacter())->GetObjectId() == CharacterId &&
					RootPart->GetObjectId() == RootId && Server.GetMetrics().CharacterControlBindings == BindingsBefore &&
					Server.GetMetrics().CharacterControlRevocations == RevocationsBefore + 1 &&
					LocalCharacterDuringRevoke && LocalCharacterDuringRevoke->GetObjectId() == LocalCharacterId &&
					!LocalCharacterDuringRevoke->GetRootPart() &&
					detail::GameSessionTestAccess::GetCharacterMetrics(Server).ActionRequestsAccepted == ActionsBefore &&
					Client.GetStatus() == GameSessionStatus::Ready,
					"same owner Character and RootPart lose one control lease when RootPart materialization becomes false");
				ServerCharacter->SetRootPart(RootPart);
				for (std::uint64_t Tick = 191; Tick <= 220; ++Tick) {
					ClientRuntime->Step();
					ServerRuntime.Step();
					Advance(Network, Server, Client, Tick);
				}
				const auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				const auto LocalCharacter = LocalPlayer && (*LocalPlayer)->GetCharacter()
					? *(*LocalPlayer)->GetCharacter() : nullptr;
				Check(ServerPlayers[0]->GetCharacter() && (*ServerPlayers[0]->GetCharacter())->GetObjectId() == CharacterId &&
					RootPart->GetObjectId() == RootId && Server.GetMetrics().CharacterControlBindings == BindingsBefore + 1 &&
					Server.GetMetrics().CharacterControlRevocations == RevocationsBefore + 1 && LocalCharacter &&
					LocalCharacter->GetObjectId() == LocalCharacterId && LocalCharacter->GetRootPart() &&
					(*LocalCharacter->GetRootPart())->GetObjectId() == LocalRootId &&
					LocalCharacter->GetAttributeValue("ReboundActionRequested").has_value() &&
					detail::GameSessionTestAccess::GetCharacterMetrics(Server).ActionRequestsAccepted == ActionsBefore + 1,
					"same owner Character and RootPart regain a fresh control lease and accept another semantic action");
				ReplacementTick = 221;
			} else if (TestControlRebind) Check(false, "control rebind fixture establishes a root-bearing owner Character");

			auto PreviousCharacter = ServerCharacter;
			ServerPlayers[0]->LoadCharacter();
			for (std::uint64_t Tick = ReplacementTick; Tick < ReplacementTick + 30; ++Tick) {
				ClientRuntime->Step();
				ServerRuntime.Step();
				Advance(Network, Server, Client, Tick);
			}
			Check(
				ServerPlayers[0]->GetCharacter().has_value() &&
					*ServerPlayers[0]->GetCharacter() != PreviousCharacter && PreviousCharacter->GetDestroyed(),
				"server Character replacement destroys the old actor and preserves Player.Character as canonical"
			);
			Check(
				Server.GetMetrics().CharacterControlBindings >= 2 &&
					Server.GetMetrics().CharacterControlRevocations >= 1,
				"Character replacement advances through revoke and fresh control-binding lifetimes"
			);
		}

		const auto ClientConnection = Client.GetPrimaryConnection();
		Check(ClientConnection.has_value(), "client retains its generation-safe underlying ConnectionId");
		if (ClientConnection)
			(void)ClientTransport->Disconnect(
				*ClientConnection, {DisconnectReason::LocalShutdown, "session lifecycle test disconnect"}
			);
		for (std::uint64_t Tick = TestControlRebind ? 251 : 191;
			Tick < (TestControlRebind ? 261 : 201); ++Tick)
			Advance(Network, Server, Client, Tick);
		Check(
			ServerRuntime.Players->GetPlayers().empty(), "disconnect tears down the authoritative Player and Character"
		);
		Check(Server.GetMetrics().PlayersRemoved == 1, "disconnect Player teardown is measured exactly once");
		if (WithByteAdmission) {
			const auto Metrics = Server.GetMetrics();
			Check(Metrics.ReliableAdmission.AcceptedBytes > 0 && Metrics.ReliableAdmissionPeerStates == 0 &&
				Metrics.ReliableAdmission.GlobalCreditHighWater <= MaximumReliableServiceGroupBytes &&
				Metrics.StructuralMaximumTransitionsSelectedPerTick <= 8192,
				"profile-enabled action/Remote/Character lifecycle preserves byte, peer cleanup and 3J bounds");
			std::cout << "[Network:ByteAdmissionSession] accepted=" << Metrics.ReliableAdmission.AcceptedBytes
				<< " deferred=" << Metrics.ReliableAdmission.SizeDeferrals
				<< " waitMaxUs=" << Metrics.ReliableAdmission.MaximumAdmissionWaitMicroseconds << '\n';
		}
		Check(
			Client.GetStatus() == GameSessionStatus::Failed && !Client.GetClientDataModel(),
			"failed client session exposes no accepted replica through ordinal status ordering"
		);
		if (ClientRuntime) {
			auto ReleasedAttachment = ClientRuntime->CharacterControl->AttachRuntime({}, {}, {}, {});
			Check(
				ReleasedAttachment && ReleasedAttachment->IsValid(),
				"client failure rollback releases its runtime callback attachment before Failed is observable"
			);
		}

		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestTwoClientIdentityAndControlIsolation() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto FirstTransport = Network->CreateTransport();
		auto SecondTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		const auto Storage = ServerWorld->GetService("ReplicatedStorage");
		auto Mailbox = std::make_shared<RemoteEvent>();
		Mailbox->SetName("PlayerMailbox");
		Mailbox->SetParent(Storage);
		auto Request = std::make_shared<RemoteFunction>();
		Request->SetName("PlayerRequest");
		Request->SetParent(Storage);
		auto ServerScript = std::make_shared<Script>();
		ServerScript->SetName("PlayerRemoteServer");
		ServerScript->SetRunContext(Enums::RunContext::Server);
		ServerScript->SetSource(R"(
local Players = game:GetService("Players")
local Storage = game:GetService("ReplicatedStorage")
local Mailbox = Storage.PlayerMailbox
local Request = Storage.PlayerRequest
local State = {}
local function Add(Player)
	State[Player] = true
end
for _, Player in Players:GetPlayers() do Add(Player) end
Players.PlayerAdded:Connect(Add)
Players.PlayerRemoving:Connect(function(Player) State[Player] = nil end)
Mailbox.OnPlayerEvent:Connect(function(Sender, Claimed)
	assert(State[Sender] and State[Claimed] and Sender ~= Claimed, "server sender must come from the session")
	local Accepted, Status = Mailbox:TryFireClient(Sender, Sender)
	assert(Accepted, "Player event submission returned " .. tostring(Status))
	task.defer(function()
		local Ok, Echo = Request:TryInvokeClient(Sender, 2, Sender.PlayerId)
		assert(Ok, "Player invocation returned " .. tostring(Echo))
		assert(Echo == Sender.PlayerId, "Player invocation reached the wrong recipient")
		script:SetAttribute("TargetedInvocations", (script:GetAttribute("TargetedInvocations") or 0) + 1)
	end)
end)
Request:SetPlayerHandler(function(Sender)
	assert(State[Sender], "Player request sender must match the existing state key")
	return nil, Sender
end)
)");
		ServerScript->SetParent(ServerWorld);
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto SpatialPlacement = ServerRuntime.Players->PlayerAdded->Connect([](std::shared_ptr<Player> PlayerValue) {
			if (!PlayerValue || !PlayerValue->GetCharacter()) return;
			auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			if (CharacterValue)
				CharacterValue->SetPosition({static_cast<float>(PlayerValue->GetPlayerId() - 1) * 1000.0f, 6.0f, 0.0f});
		});
		GameSession Server(
			ServerTransport, Configuration(GameSessionRole::Server, "two-client-session"), &ServerRuntime
		);
		GameSession First(FirstTransport, Configuration(GameSessionRole::Client, "two-client-session"));
		auto SecondConfiguration = Configuration(GameSessionRole::Client, "two-client-session");
		SecondConfiguration.ClientNonce += 1;
		GameSession Second(SecondTransport, SecondConfiguration);
		std::size_t UnreadyPlayers = 0;
		const auto UnreadyConnection = ServerRuntime.Players->PlayerAdded->Connect(
			[&](const std::shared_ptr<Player> &Value) {
				if (Value && !Server.ResolveRemotePeer(*Value)) ++UnreadyPlayers;
			});
		Check(
			Server.Start().Succeeded() && First.Start().Succeeded() && Second.Start().Succeeded(),
			"two-client session endpoints start on one server lifetime"
		);

		std::unique_ptr<HeadlessRenderer> FirstRenderer;
		std::unique_ptr<HeadlessRenderer> SecondRenderer;
		std::unique_ptr<Engine> FirstRuntime;
		std::unique_ptr<Engine> SecondRuntime;
		for (std::uint64_t Tick = 1; Tick <= 160 && Server.GetMetrics().ReadyPeers != 2; ++Tick) {
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			if (!FirstRuntime && First.GetClientDataModel()) {
				FirstRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				FirstRuntime = std::make_unique<Engine>(
					First.GetClientDataModel(),
					FirstRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				FirstRuntime->ProcessService->Alive = true;
				Check(First.AttachClientRuntime(*FirstRuntime), "first client attaches to its trusted Player");
			}
			if (!SecondRuntime && Second.GetClientDataModel()) {
				SecondRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				SecondRuntime = std::make_unique<Engine>(
					Second.GetClientDataModel(),
					SecondRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				SecondRuntime->ProcessService->Alive = true;
				Check(Second.AttachClientRuntime(*SecondRuntime), "second client attaches to its trusted Player");
			}
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}

		Check(
			FirstRuntime && SecondRuntime && Server.GetMetrics().ReadyPeers == 2,
			"two clients independently reach gameplay readiness"
		);
		auto FirstLocal = FirstRuntime ? FirstRuntime->Players->GetLocalPlayer() : std::nullopt;
		auto SecondLocal = SecondRuntime ? SecondRuntime->Players->GetLocalPlayer() : std::nullopt;
		Check(
			FirstLocal && SecondLocal && (*FirstLocal)->GetPlayerId() != (*SecondLocal)->GetPlayerId(),
			"each client resolves a distinct LocalPlayer from its explicit accepted Player identity"
		);

		auto ServerPlayers = ServerRuntime.Players->GetPlayers();
		std::shared_ptr<Player> FirstServerPlayer;
		std::shared_ptr<Player> SecondServerPlayer;
		std::shared_ptr<KinematicCharacter> FirstCharacter;
		std::shared_ptr<KinematicCharacter> SecondCharacter;
		for (const auto &PlayerValue : ServerPlayers) {
			if (FirstLocal && PlayerValue->GetPlayerId() == (*FirstLocal)->GetPlayerId()) {
				FirstServerPlayer = PlayerValue;
				FirstCharacter = std::dynamic_pointer_cast<KinematicCharacter>(PlayerValue->GetCharacter().value_or(nullptr));
			}
			if (SecondLocal && PlayerValue->GetPlayerId() == (*SecondLocal)->GetPlayerId()) {
				SecondServerPlayer = PlayerValue;
				SecondCharacter = std::dynamic_pointer_cast<KinematicCharacter>(PlayerValue->GetCharacter().value_or(nullptr));
			}
		}
		const auto FirstPeer = FirstServerPlayer ? Server.ResolveRemotePeer(*FirstServerPlayer) : std::nullopt;
		const auto Unassociated = std::make_shared<Player>();
		Check(!Server.ResolveRemotePeer(*Unassociated) && !Unassociated->Id.IsValid(),
			"peer lookup of a detached Player leaves its identity unpublished");
		const auto SecondPeer = SecondServerPlayer ? Server.ResolveRemotePeer(*SecondServerPlayer) : std::nullopt;
		Check(UnreadyPlayers == 2 && FirstPeer && SecondPeer && FirstPeer->Connection != SecondPeer->Connection &&
			Server.ResolveRemotePlayer(*FirstPeer) == FirstServerPlayer &&
			Server.ResolveRemotePlayer(*SecondPeer) == SecondServerPlayer,
			"ready server peers resolve to their current distinct Players and PlayerAdded does not imply readiness");
		Check(FirstPeer && !Server.ResolveRemotePlayer({FirstPeer->Connection, ReplicationEpoch{}}) &&
			!Server.ResolveRemotePlayer({{FirstPeer->Connection.Slot, FirstPeer->Connection.Generation + 1},
				FirstPeer->Epoch}) && FirstLocal && !Server.ResolveRemotePeer(**FirstLocal) &&
			!First.ResolveRemotePlayer(*FirstPeer),
			"Player bridge rejects wrong epochs, generations, scopes and client-side lookup");

		const auto InstallClientProbe = [](Engine *Runtime) -> std::shared_ptr<Script> {
			if (!Runtime) return nullptr;
			auto Value = std::make_shared<Script>();
			Value->SetRunContext(Enums::RunContext::Client);
			Value->SetSource(R"(
local Players = game:GetService("Players")
local Storage = game:GetService("ReplicatedStorage")
local Mailbox = Storage.PlayerMailbox
local Request = Storage.PlayerRequest
local Local = Players.LocalPlayer
local ExpectedId = Local.PlayerId
local Cache = {[Local] = true}
Mailbox.OnClientEvent:Connect(function(Recipient)
	assert(Cache[Recipient], "Player event must carry the canonical local Player wrapper")
	script:SetAttribute("Deliveries", (script:GetAttribute("Deliveries") or 0) + 1)
end)
Request:SetClientHandler(function(Marker)
	assert(not pcall(function() return Local.PlayerId end), "request handler must remain network-only")
	assert(Marker == ExpectedId, "Player request reached the wrong client")
	return Marker
end)
assert(not pcall(function() Mailbox:TryFireClient(Local, "unauthorized") end))
local Claimed
for _, Player in Players:GetPlayers() do
	if Player ~= Local then Claimed = Player end
end
assert(Claimed, "client fixture needs a second visible Player")
local Accepted, Status = Mailbox:TryFireServer(Claimed)
assert(Accepted, "client event submission returned " .. tostring(Status))
local Ok, Empty, Recipient = Request:TryInvokeServer(2)
assert(Ok, "server invocation returned " .. tostring(Empty))
assert(Empty == nil and Cache[Recipient], "returns must preserve leading nil and local Player table-key identity")
script:SetAttribute("ReturnedPlayer", true)
)");
			Value->SetParent(Runtime->DataModel);
			return Value;
		};
		const auto FirstProbe = InstallClientProbe(FirstRuntime.get());
		const auto SecondProbe = InstallClientProbe(SecondRuntime.get());
		const auto FirstConnection = First.GetPrimaryConnection();
		std::vector<std::shared_ptr<KinematicCharacter>> DistantCharacters;
		for (int Index = 0; Index < 32; ++Index) {
			auto Distant = std::make_shared<KinematicCharacter>();
			Distant->SetPosition({100000.0f + Index * 1024.0f, 0.0f, 0.0f});
			Distant->SetParent(ServerWorld);
			DistantCharacters.push_back(std::move(Distant));
		}
		for (std::uint64_t Tick = 161; Tick <= 180; ++Tick) {
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			runtime_detail::WorkSample GraphWork{};
			{
				runtime_detail::WorkCapture Capture(&GraphWork);
				Server.Step(Tick);
			}
			Check(GraphWork.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::GraphCharacters)] <= 4,
				"steady graph synchronization does not inspect distant registered Characters for each peer");
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		const auto GetCount = [](const Instance &Value, std::string_view Name) {
			const auto Attribute = Value.GetAttributeValue(Name);
			if (!Attribute) return 0.0;
			if (const auto *Integer = std::get_if<int>(&*Attribute)) return static_cast<double>(*Integer);
			if (const auto *Number = std::get_if<double>(&*Attribute)) return *Number;
			return 0.0;
		};
		Check(FirstProbe && SecondProbe && GetCount(*FirstProbe, "Deliveries") == 1 &&
			GetCount(*SecondProbe, "Deliveries") == 1 &&
			FirstProbe->GetAttributeValue("ReturnedPlayer").has_value() &&
			SecondProbe->GetAttributeValue("ReturnedPlayer").has_value() &&
			GetCount(*ServerScript, "TargetedInvocations") == 2,
			"both clients complete Player-targeted events and requests with local replica table-key identity");
		auto FindClientCharacter = [](Engine *RuntimeValue, std::uint32_t PlayerId) {
			if (!RuntimeValue) return std::shared_ptr<KinematicCharacter>{};
			for (const auto &PlayerValue : RuntimeValue->Players->GetPlayers())
				if (PlayerValue->GetPlayerId() == PlayerId && PlayerValue->GetCharacter())
					return std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			return std::shared_ptr<KinematicCharacter>{};
		};
		auto FirstOwnReplica = FirstLocal
								   ? FindClientCharacter(
										 FirstRuntime.get(), static_cast<std::uint32_t>((*FirstLocal)->GetPlayerId())
									 )
								   : nullptr;
		auto FirstRemoteReplica =
			SecondLocal
				? FindClientCharacter(FirstRuntime.get(), static_cast<std::uint32_t>((*SecondLocal)->GetPlayerId()))
				: nullptr;
		auto SecondOwnReplica = SecondLocal
									? FindClientCharacter(
										  SecondRuntime.get(), static_cast<std::uint32_t>((*SecondLocal)->GetPlayerId())
									  )
									: nullptr;
		auto SecondRemoteReplica =
			FirstLocal
				? FindClientCharacter(SecondRuntime.get(), static_cast<std::uint32_t>((*FirstLocal)->GetPlayerId()))
				: nullptr;
		Check(
			FirstOwnReplica && FirstOwnReplica->GetRootPart() && SecondOwnReplica && SecondOwnReplica->GetRootPart() &&
				FirstRemoteReplica && !FirstRemoteReplica->GetRootPart() && SecondRemoteReplica &&
				!SecondRemoteReplica->GetRootPart(),
			"two peers keep their owner Character pinned while distant remote Character descendants unpublish"
		);
		if (SecondCharacter) SecondCharacter->SetPosition({100.0f, 6.0f, 0.0f});
		for (std::uint64_t Tick = 181; Tick <= 200; ++Tick) {
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		FirstRemoteReplica = SecondLocal
								 ? FindClientCharacter(
									   FirstRuntime.get(), static_cast<std::uint32_t>((*SecondLocal)->GetPlayerId())
								   )
								 : nullptr;
		SecondRemoteReplica = FirstLocal
								  ? FindClientCharacter(
										SecondRuntime.get(), static_cast<std::uint32_t>((*FirstLocal)->GetPlayerId())
									)
								  : nullptr;
		Check(
			FirstRemoteReplica && FirstRemoteReplica->GetRootPart() && SecondRemoteReplica &&
				SecondRemoteReplica->GetRootPart(),
			"remote Characters reenter both client materialization views at current authoritative position"
		);
		const auto FirstStart = FirstCharacter ? FirstCharacter->GetPosition() : glm::vec3{};
		const auto SecondStart = SecondCharacter ? SecondCharacter->GetPosition() : glm::vec3{};
		if (FirstRuntime)
			(void)FirstRuntime->ProcessEvent(
				KeyEvent{
					.Device = {1},
					.Physical = PhysicalKey::W,
					.Logical = LogicalKey::W,
					.State = ButtonState::Pressed,
				}
			);
		for (std::uint64_t Tick = 201; Tick <= 250; ++Tick) {
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		Check(
			FirstCharacter && HorizontalDistance(FirstCharacter->GetPosition(), FirstStart) > 0.25f,
			"first connection commands its own Player.Character"
		);
		Check(
			SecondCharacter && HorizontalDistance(SecondCharacter->GetPosition(), SecondStart) < 0.05f,
			"first connection cannot move the other Player's Character"
		);

		std::uint64_t NextTick = 251;
		if (SecondCharacter && FirstConnection && SecondCharacter->GetRootPart()) {
			const auto RootId = (*SecondCharacter->GetRootPart())->GetObjectId();
			RetirementEvidenceCapture RetirementTrace;
			auto StepRelevance = [&](int Steps) {
				for (int Index = 0; Index < Steps; ++Index) {
					if (FirstRuntime) FirstRuntime->Step();
					if (SecondRuntime) SecondRuntime->Step();
					ServerRuntime.Step();
					Network->Pump();
					(void)Server.Poll(); (void)First.Poll(); (void)Second.Poll();
					Server.Step(NextTick); First.Step(NextTick); Second.Step(NextTick);
					++NextTick;
					(void)Network->Advance(20ms);
				}
			};
			auto RootRetirements = [&] {
				return std::count_if(RetirementTrace.Records.begin(),
					RetirementTrace.Records.begin() + RetirementTrace.Count, [&](const auto &Record) {
						return Record.Object == RootId && Record.Connection == *FirstConnection;
					});
			};
			SecondCharacter->SetPosition({100000.0f, 6.0f, 0.0f});
			StepRelevance(20);
			Check(!RetirementTrace.Overflow && RootRetirements() == 1,
				"accepted GRPL unpublish retires the remote RootPart relationship once");
		} else Check(false, "remote RootPart unpublish regression has two live recipients");

		if (FirstConnection)
			(void)FirstTransport->Disconnect(
				*FirstConnection, {DisconnectReason::LocalShutdown, "two-client isolation disconnect"}
			);
		for (std::uint64_t Tick = NextTick; Tick < NextTick + 15; ++Tick) {
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		Check(
			SecondLocal && ServerRuntime.Players->GetPlayers().size() == 1 &&
				ServerRuntime.Players->GetPlayers().front()->GetPlayerId() == (*SecondLocal)->GetPlayerId(),
			"disconnect removes only the associated Player and preserves the other connection lifetime"
		);
		Check(FirstPeer && FirstServerPlayer && !Server.ResolveRemotePlayer(*FirstPeer) &&
			!Server.ResolveRemotePeer(*FirstServerPlayer) && SecondPeer &&
			Server.ResolveRemotePlayer(*SecondPeer) == SecondServerPlayer,
			"disconnect invalidates both directions of only the retired Player association");

		First.Stop();
		Second.Stop();
		Server.Stop();
		if (FirstRuntime) FirstRuntime->Destroy();
		if (SecondRuntime) SecondRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestCharacterRetirementAcrossStructuralFrames() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(World, &Renderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		Runtime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "character-retirement");
		ServerConfiguration.StructuralReplication.MaximumTransitionsPerPeerTick = 64;
		ServerConfiguration.StructuralReplication.PeerQuantum = 64;
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		GameSession Server(Network->CreateTransport(), ServerConfiguration, &Runtime);
		GameSession Client(Network->CreateTransport(), Configuration(GameSessionRole::Client, "character-retirement"));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "retirement regression sessions start");
		std::uint64_t Tick = 1;
		auto Step = [&] {
			Advance(Network, Server, Client, Tick++);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
				ClientRuntime = std::make_unique<Engine>(Client.GetClientDataModel(), ClientRenderer.get(), nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
				Check(Client.AttachClientRuntime(*ClientRuntime), "retirement client runtime attaches");
			}
		};
		for (int Index = 0; Index < 60; ++Index) Step();
		const auto Players = Runtime.Players->GetPlayers();
		if (Players.size() == 1 && Players.front()->GetCharacter() && ClientRuntime) {
			auto CharacterValue = *Players.front()->GetCharacter();
			const auto OldId = CharacterValue->GetObjectId();
			for (int Index = 0; Index < 40; ++Index) {
				auto Child = std::make_shared<Folder>();
				Child->SetName("RetirementChild" + std::to_string(Index));
				Child->SetParent(CharacterValue);
			}
			for (int Index = 0; Index < 30; ++Index) Step();
			const auto Local = ClientRuntime->Players->GetLocalPlayer();
			const auto Previous = Local && (*Local)->GetCharacter() ? *(*Local)->GetCharacter() : nullptr;
			Check(Previous && Previous->GetDescendants().size() >= 40,
				"retirement regression materializes a dependency group within the per-peer structural cap");
			const auto RevokesBefore = Server.GetMetrics().CharacterControlRevocations;
			RetirementEvidenceCapture RetirementTrace;
			Players.front()->RemoveCharacter();
			for (int Index = 0; Index < 30 && Client.GetStatus() != GameSessionStatus::Failed; ++Index) Step();
			std::cout << "[Network:RetirementTest] ready=" << (Client.GetStatus() == GameSessionStatus::Ready)
				<< " localCharacter=" << (Local && (*Local)->GetCharacter().has_value())
				<< " previousDestroyed=" << (Previous && Previous->GetDestroyed())
				<< " oldServerRegistered=" << static_cast<bool>(ObjectRegistry::Get().Lookup(OldId))
				<< " revokes=" << Server.GetMetrics().CharacterControlRevocations - RevokesBefore
				<< " failure=" << Client.GetFailure() << '\n';
			Check(Client.GetStatus() == GameSessionStatus::Ready && Local && !(*Local)->GetCharacter() &&
				Previous && Previous->GetDestroyed() && !ObjectRegistry::Get().Lookup(OldId) &&
				Server.GetMetrics().CharacterControlRevocations == RevokesBefore + 1,
				"relationship-driven recursive destruction and later bounded Leave frames retire each registry only once");
			const auto OldRetirements = std::count_if(RetirementTrace.Records.begin(),
				RetirementTrace.Records.begin() + RetirementTrace.Count, [&](const auto &Record) {
					return Record.Object == OldId;
				});
			Check(!RetirementTrace.Overflow && OldRetirements == 1 &&
				std::any_of(RetirementTrace.Records.begin(),
					RetirementTrace.Records.begin() + RetirementTrace.Count, [&](const auto &Record) {
						return Record.Object == OldId && Record.Connection.IsValid() && Record.Tick > 0 &&
							Record.Sequence == 0;
					}),
				"accepted GRPL destroy retires the exact old recipient/Character generation once");
			if (Client.GetStatus() != GameSessionStatus::Failed) {
				Players.front()->LoadCharacter();
				for (int Index = 0; Index < 30; ++Index) Step();
				Check(Client.GetStatus() == GameSessionStatus::Ready && Local && (*Local)->GetCharacter() &&
					*(*Local)->GetCharacter() != Previous && Players.front()->GetCharacter() &&
					(*Players.front()->GetCharacter())->GetObjectId() != OldId,
					"fresh Character materializes after delayed old-lifetime retirement");
				if (Players.front()->GetCharacter()) {
					const auto NewId = (*Players.front()->GetCharacter())->GetObjectId();
					Check(std::none_of(RetirementTrace.Records.begin(),
						RetirementTrace.Records.begin() + RetirementTrace.Count, [&](const auto &Record) {
							return Record.Object == NewId;
						}), "old-generation GRPL destroy never retires the new Character identity");
				}
			}
		} else Check(false, "retirement regression establishes a Player and Character");
		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		Runtime.Destroy();
	}

	void TestSpeculativeRelevanceMaterialization() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(World, &Renderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		Runtime.ProcessService->Alive = true;
		auto Npc = std::make_shared<KinematicCharacter>();
		auto Root = std::make_shared<Part>();
		Root->SetAnchored(true);
		Root->SetParent(Npc);
		Npc->SetRootPart(Root);
		Npc->SetParent(Runtime.Workspace);
		for (int Index = 0; Index < 96; ++Index) {
			auto Child = std::make_shared<Folder>();
			Child->SetName("PlanningWork" + std::to_string(Index));
			Child->SetParent(Npc);
		}
		auto Settings = Configuration(GameSessionRole::Server, "speculative-relevance");
		auto Ground = std::make_shared<Part>();
		Ground->SetAnchored(true);
		Ground->SetSize({1024, 1, 1024});
		Ground->SetParent(Runtime.Workspace);
		// Real bounded planning intentionally spans more than one server step.
		// Relevance can reverse before a planned Leave has ever been accepted.
		Settings.StructuralReplication.PlanningWorkPerTick = 64;
		Settings.StructuralReplication.PlanningPeerQuantum = 64;
		Settings.HandshakeTimeoutTicks = 600;
		GameSession Server(Network->CreateTransport(), Settings, &Runtime);
		GameSession Client(Network->CreateTransport(), Configuration(GameSessionRole::Client, "speculative-relevance"));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "speculative relevance sessions start");
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		std::uint64_t Tick = 1;
		auto Step = [&] {
			Advance(Network, Server, Client, Tick++);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
				ClientRuntime = std::make_unique<Engine>(Client.GetClientDataModel(), ClientRenderer.get(), nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "speculative relevance client attaches");
			}
			if (ClientRuntime) ClientRuntime->Step();
			Runtime.Step();
		};
		for (int Index = 0; Index < 500; ++Index) Step();
		std::cout << "[Network:SpeculativeRelevance] ready=" << Server.GetMetrics().ReadyPeers
			<< " materialized=" << Server.GetMetrics().MaterializedCharacters
			<< " server_failure=" << Server.GetFailure() << " client_failure=" << Client.GetFailure() << '\n';
		const auto Identities = Server.GetPeerIdentities();
		if (Identities.size() == 1 && ClientRuntime && Server.GetMetrics().MaterializedCharacters >= 2) {
			const auto Connection = Identities.front().Connection;
			const auto Before = Server.GetMetrics().MaterializedCharacters;
			const auto BeforeOverflows = Client.GetMetrics().ClientCharacterHistoryOverflows;
			const auto BeforeBytes = Client.GetMetrics().ClientStructuralBytesReceived;
			const std::array Far{glm::vec3{10000, 0, 0}};
			Check(Server.SetTrustedReplicationFocus(Connection, Far), "far trusted focus is accepted");
			Step();
			std::cout << "[Network:SpeculativeRelevance] before=" << Before
				<< " after=" << Server.GetMetrics().MaterializedCharacters
				<< " bytes=" << Client.GetMetrics().ClientStructuralBytesReceived - BeforeBytes << '\n';
			Check(Server.GetMetrics().MaterializedCharacters == Before,
				"an unaccepted speculative Leave cannot retire a committed Character materialization");
			const std::array Near{glm::vec3{0, 0, 0}};
			Check(Server.SetTrustedReplicationFocus(Connection, Near), "near trusted focus cancels the speculative Leave");
			for (int Index = 0; Index < 500; ++Index) Step();
			const auto StaleBefore = Client.GetMetrics().ClientCharacterStaleStatesDropped;
			for (int Index = 0; Index < 30; ++Index) Step();
			std::cout << "[Network:SpeculativeRelevance] stale_delta="
				<< Client.GetMetrics().ClientCharacterStaleStatesDropped - StaleBefore
				<< " history_overflows=" << Client.GetMetrics().ClientCharacterHistoryOverflows
				<< " expected_epoch=" << Client.GetMetrics().ClientLastExpectedMaterializationEpoch
				<< " received_epoch=" << Client.GetMetrics().ClientLastReceivedMaterializationEpoch << '\n';
			Check(Server.GetMetrics().MaterializedCharacters == Before &&
				Client.GetMetrics().ClientCharacterStaleStatesDropped == StaleBefore &&
				Client.GetMetrics().ClientCharacterHistoryOverflows == BeforeOverflows,
				"Leave cancellation preserves the shared GCHR epoch and ongoing owner state service");
			Check(detail::GameSessionTestAccess::RequestClientCharacterAction(Client, 1, Tick) &&
				Client.GetMetrics().ClientActionSubmissionSuspended == 0,
				"speculative Leave cancellation cannot suspend a locally valid owner action request");
			for (int Index = 0; Index < 8; ++Index) {
				Check(Server.SetTrustedReplicationFocus(Connection, Far), "oscillating far focus is accepted");
				Step();
				Check(Server.SetTrustedReplicationFocus(Connection, Near), "oscillating near focus is accepted");
				Step();
			}
			for (int Index = 0; Index < 100; ++Index) Step();
			const auto OscillationStale = Client.GetMetrics().ClientCharacterStaleStatesDropped;
			for (int Index = 0; Index < 30; ++Index) Step();
			Check(Server.GetMetrics().MaterializedCharacters == Before &&
				Client.GetMetrics().ClientCharacterStaleStatesDropped == OscillationStale,
				"repeated unaccepted relevance reversals cannot accumulate materialization epochs");
			Check(Server.SetTrustedReplicationFocus(Connection, Far), "lasting far focus is accepted");
			for (int Index = 0; Index < 500; ++Index) Step();
			Check(Server.GetMetrics().MaterializedCharacters == Before - 1,
				"an actually accepted structural Leave still retires the NPC publication lifetime");
			Check(Server.SetTrustedReplicationFocus(Connection, Near), "lasting near focus is accepted");
			for (int Index = 0; Index < 500; ++Index) Step();
			const auto ReentryStale = Client.GetMetrics().ClientCharacterStaleStatesDropped;
			for (int Index = 0; Index < 30; ++Index) Step();
			Check(Server.GetMetrics().MaterializedCharacters == Before &&
				Client.GetMetrics().ClientCharacterStaleStatesDropped == ReentryStale,
				"accepted Leave and fresh Enter maintain matching GCHR materialization epochs");
		} else Check(false, "speculative relevance fixture materializes both owner and NPC");
		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		Runtime.Destroy();
	}

	void TestPlayerGuiPrivateSession() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto World = std::make_shared<DataModel>();
		const auto Starter = World->GetService("StarterGui");
		const auto PlayersService = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		PlayersService->SetDefaultControllerEnabled(false);
		PlayersService->SetDefaultCameraEnabled(false);
		PlayersService->SetCharacterAutoLoads(false);
		auto Hud = std::make_shared<ScreenGui>();
		Hud->SetName("PrivateHud");
		Hud->SetParent(Starter);
		auto Briefing = std::make_shared<ScreenGui>();
		Briefing->SetName("Briefing");
		Briefing->SetResetOnSpawn(true);
		Briefing->SetParent(Starter);
		auto Source = std::make_shared<Script>();
		Source->SetName("LocalLogic");
		Source->SetRunContext(Enums::RunContext::Client);
		Source->SetSource(R"(script.Parent:SetAttribute("LocalLogicRan", true))");
		Source->SetParent(Briefing);
		auto Module = std::make_shared<ModuleScript>();
		Module->SetName("StateModule");
		Module->SetSource("return 17");
		Module->SetParent(Briefing);
		Hud->MarkPersistenceSubtreeArchivable();
		Briefing->MarkPersistenceSubtreeArchivable();
		auto FirstConfig = Configuration(GameSessionRole::Client, "player-gui-private");
		FirstConfig.HydratePlayerGuiCode = PackageBuilder::MakePlayerGuiCodeHydrator(World);
		auto SecondConfig = FirstConfig;
		++SecondConfig.ClientNonce;
		HeadlessRenderer ServerRenderer(Vector2(32, 32));
		HeadlessRenderer FirstRenderer(Vector2(32, 32));
		HeadlessRenderer SecondRenderer(Vector2(32, 32));
		HeadlessRenderer RejoinedRenderer(Vector2(32, 32));
		Engine ServerRuntime(World, &ServerRenderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		ServerRuntime.ProcessService->Alive = true;
		std::unique_ptr<Engine> FirstRuntime;
		std::unique_ptr<Engine> SecondRuntime;
		std::unique_ptr<Engine> RejoinedRuntime;
		GameSession Server(Network->CreateTransport(), Configuration(GameSessionRole::Server, "player-gui-private"), &ServerRuntime);
		GameSession First(Network->CreateTransport(), FirstConfig);
		GameSession Second(Network->CreateTransport(), SecondConfig);
		Check(Server.Start().Succeeded() && First.Start().Succeeded() && Second.Start().Succeeded(),
			"private GUI qualification starts one server and two simulated clients");
		auto Attach = [&](GameSession &Session, HeadlessRenderer &Renderer, std::unique_ptr<Engine> &Runtime) {
			if (Runtime || !Session.GetClientDataModel()) return;
			Runtime = std::make_unique<Engine>(Session.GetClientDataModel(), &Renderer, nullptr,
				EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
			Runtime->ProcessService->Alive = true;
			Check(Session.AttachClientRuntime(*Runtime), "client runtime attaches with its owned PlayerGui");
		};
		std::uint64_t Tick = 0;
		std::uint64_t RejoinedTick = 0;
		auto Pump = [&](std::uint64_t Count, GameSession *Rejoining = nullptr) {
			for (std::uint64_t Step = 0; Step < Count; ++Step) {
				(void)Network->Advance(2ms);
				Network->Pump();
				(void)Server.Poll();
				(void)First.Poll();
				(void)Second.Poll();
				if (Rejoining) {
					(void)Rejoining->Poll();
					Attach(*Rejoining, RejoinedRenderer, RejoinedRuntime);
				}
				Attach(First, FirstRenderer, FirstRuntime);
				Attach(Second, SecondRenderer, SecondRuntime);
				ServerRuntime.Step();
				if (FirstRuntime) FirstRuntime->Step();
				if (SecondRuntime) SecondRuntime->Step();
				if (RejoinedRuntime) RejoinedRuntime->Step();
				++Tick;
				Server.Step(Tick);
				First.Step(Tick);
				Second.Step(Tick);
				// A new client starts a new handshake clock even when the server is already running.
				if (Rejoining) Rejoining->Step(++RejoinedTick);
				(void)Network->Advance(20ms);
			}
		};
		Pump(75);
		Check(FirstRuntime && SecondRuntime && Server.GetMetrics().ReadyPeers == 2,
			"both clients reach readiness independently of Character provisioning");
		if (!FirstRuntime || !SecondRuntime) return;
		auto Owner = FirstRuntime->Players->GetLocalPlayer().value();
		auto Other = SecondRuntime->Players->GetLocalPlayer().value();
		auto Gui = Owner->GetPlayerGui().value();
		auto OtherGui = Other->GetPlayerGui().value();
		auto FirstHud = std::dynamic_pointer_cast<ScreenGui>(Gui->FindFirstChild("PrivateHud", false));
		auto OldBriefing = Gui->FindFirstChild("Briefing", false);
		Check(FirstHud && OldBriefing && OtherGui->FindFirstChild("PrivateHud", false) &&
			!First.GetClientDataModel()->FindFirstChildOfClass("StarterGui", false) && !Owner->GetCharacter(),
			"both clients receive their own templates without receiving StarterGui");
		if (!FirstHud || !OldBriefing) return;
		Check(FirstRuntime->Players->GetPlayers().size() == 2 && SecondRuntime->Players->GetPlayers().size() == 2,
			"public Player identities still materialize for both peers");
		for (const auto &Candidate : FirstRuntime->Players->GetPlayers())
			if (Candidate != Owner) Check(!Candidate->GetPlayerGui(), "the first client sees no foreign PlayerGui");
		for (const auto &Candidate : SecondRuntime->Players->GetPlayers())
			if (Candidate != Other) Check(!Candidate->GetPlayerGui(), "the second client sees no foreign PlayerGui");
		const auto FirstModule = std::dynamic_pointer_cast<ModuleScript>(OldBriefing->FindFirstChild("StateModule", false));
		const auto FirstCode = std::dynamic_pointer_cast<Script>(OldBriefing->FindFirstChild("LocalLogic", false));
		Check(FirstModule && FirstModule->GetSource() == "return 17" && FirstCode && FirstCode->HasSource() &&
			FirstCode->GetSourceId() != Source->GetSourceId() && FirstCode->GetGuiTemplateSourceId() == Source->GetSourceId() &&
			OldBriefing->GetAttributeValue("LocalLogicRan") == std::optional<WireValue>(true),
			"trusted packaged code hydrates on the owning replica using provenance and distinct live source identity");

		const auto ServerPlayers = PlayersService->GetPlayers();
		const auto ServerOwner = std::ranges::find_if(ServerPlayers, [&](const auto &Candidate) {
			return Candidate->GetPlayerId() == Owner->GetPlayerId();
		});
		if (ServerOwner == ServerPlayers.end()) { Check(false, "server owner is present"); return; }
		auto ServerGui = (*ServerOwner)->GetPlayerGui().value();
		Check(!ServerGui->FindFirstChild("Briefing", false)->GetAttributeValue("LocalLogicRan"),
			"server-side template and PlayerGui Client Script never execute on the server");
		auto LateContent = std::make_shared<ScreenGui>();
		LateContent->SetName("ServerAdded");
		LateContent->SetParent(ServerGui);
		const auto SetProvenance = [&](LuaSourceContainer &Candidate, const std::string &Digest) {
			Candidate.SetGuiTemplateSourceId(Source->GetSourceId());
			Candidate.SetGuiTemplateSourceDigest(Digest);
			Candidate.SetGuiTemplateSourceVersion(Candidate.GetSourceVersion());
		};
		auto WrongRole = std::make_shared<ModuleScript>();
		WrongRole->SetName("WrongRole");
		SetProvenance(*WrongRole, Source->GetSourceDigest());
		WrongRole->SetParent(LateContent);
		auto WrongContext = std::make_shared<Script>();
		WrongContext->SetName("WrongContext");
		WrongContext->SetRunContext(Enums::RunContext::Server);
		SetProvenance(*WrongContext, Source->GetSourceDigest());
		WrongContext->SetParent(LateContent);
		auto WrongDigest = std::make_shared<Script>();
		WrongDigest->SetName("WrongDigest");
		WrongDigest->SetRunContext(Enums::RunContext::Client);
		WrongDigest->SetSource("return false");
		SetProvenance(*WrongDigest, WrongDigest->GetSourceDigest());
		WrongDigest->SetParent(LateContent);
		auto WrongVersion = std::make_shared<Script>();
		WrongVersion->SetName("WrongVersion");
		WrongVersion->SetRunContext(Enums::RunContext::Client);
		SetProvenance(*WrongVersion, Source->GetSourceDigest());
		WrongVersion->SetSourceVersion(WrongVersion->GetSourceVersion() + 1);
		WrongVersion->SetParent(LateContent);
		auto UntrustedScript = std::make_shared<Script>();
		UntrustedScript->SetName("UntrustedScript");
		UntrustedScript->SetRunContext(Enums::RunContext::Client);
		UntrustedScript->SetSource(R"(script.Parent:SetAttribute("UntrustedRan", true))");
		UntrustedScript->SetParent(LateContent);
		if (const auto ServerModule = std::dynamic_pointer_cast<ModuleScript>(
			ServerGui->FindFirstChild("StateModule", true))) ServerModule->SetSource("return 23");
		Pump(20);
		const auto LateReplica = Gui->FindFirstChild("ServerAdded", false);
		const auto WrongReplica = LateReplica ? std::dynamic_pointer_cast<ModuleScript>(
			LateReplica->FindFirstChild("WrongRole", false)) : nullptr;
		const auto UntrustedReplica = LateReplica ? std::dynamic_pointer_cast<Script>(
			LateReplica->FindFirstChild("UntrustedScript", false)) : nullptr;
		const auto WrongContextReplica = LateReplica ? std::dynamic_pointer_cast<Script>(
			LateReplica->FindFirstChild("WrongContext", false)) : nullptr;
		const auto WrongDigestReplica = LateReplica ? std::dynamic_pointer_cast<Script>(
			LateReplica->FindFirstChild("WrongDigest", false)) : nullptr;
		const auto WrongVersionReplica = LateReplica ? std::dynamic_pointer_cast<Script>(
			LateReplica->FindFirstChild("WrongVersion", false)) : nullptr;
		Check(LateReplica && WrongReplica && !WrongReplica->HasSource() &&
			WrongContextReplica && !WrongContextReplica->HasSource() &&
			WrongDigestReplica && !WrongDigestReplica->HasSource() &&
			WrongVersionReplica && !WrongVersionReplica->HasSource() && FirstModule && !FirstModule->HasSource() &&
			UntrustedReplica && !UntrustedReplica->HasSource() && !LateReplica->GetAttributeValue("UntrustedRan") &&
			!OtherGui->FindFirstChild("ServerAdded", false),
			"private code with wrong class, role, digest or source version stays inactive without broadcasting");

		auto LocalReset = std::make_shared<ScreenGui>();
		LocalReset->SetResetOnSpawn(true);
		auto LocalKeep = std::make_shared<ScreenGui>();
		bool ReplicaDestroyBlocked = false;
		bool ReplicaMoveBlocked = false;
		{
			ScriptSecurityScope Security(ScriptSecurityContext::ClientRuntime());
			FirstHud->SetEnabled(false);
			LocalReset->SetParent(Gui);
			LocalKeep->SetParent(Gui);
			auto LocalTransient = std::make_shared<ScreenGui>();
			LocalTransient->SetParent(Gui);
			LocalTransient->Destroy();
			Check(LocalTransient->GetDestroyed(), "owning client can remove a GUI node created locally");
			try { FirstHud->Destroy(); } catch (const std::runtime_error &) { ReplicaDestroyBlocked = true; }
			try { FirstHud->SetParent(std::nullopt); } catch (const std::runtime_error &) { ReplicaMoveBlocked = true; }
		}
		Check(ReplicaDestroyBlocked && ReplicaMoveBlocked && !FirstHud->GetDestroyed() &&
			FirstHud->GetParent().value_or(nullptr) == Gui && !FirstHud->GetEnabled(),
			"owner can change presentation properties while server GUI identity and hierarchy stay valid");
		auto BindCharacter = [&] {
			auto Value = std::make_shared<KinematicCharacter>();
			Value->SetParent(ServerRuntime.Workspace);
			(*ServerOwner)->SetCharacter(Value);
		};
		BindCharacter();
		Pump(20);
		const auto FirstCharacter = Owner->GetCharacter().value_or(nullptr);
		Check(FirstCharacter && !LocalReset->GetDestroyed() && !OldBriefing->GetDestroyed(),
			"the ready client receives its first authoritative Character binding with the original GUI intact");
		(*ServerOwner)->RemoveCharacter();
		BindCharacter();
		Pump(35);
		const auto NewBriefing = Gui->FindFirstChild("Briefing", false);
		const auto ReplacementCharacter = Owner->GetCharacter().value_or(nullptr);
		Check(ReplacementCharacter && ReplacementCharacter != FirstCharacter && First.GetStatus() == GameSessionStatus::Ready,
			"the replacement Character reaches the owner while the client session stays ready");
		Check(NewBriefing && NewBriefing != OldBriefing && OldBriefing->GetDestroyed() &&
			NewBriefing->GetAttributeValue("LocalLogicRan") == std::optional<WireValue>(true),
			"respawn hydrates and executes the fresh server-authored reset clone on an already ready client");
		Check(LocalReset->GetDestroyed() && !LocalKeep->GetDestroyed(),
			"the ready client retires only its own opted-in roots on an authoritative respawn");
		Check(!FirstHud->GetEnabled() &&
			std::dynamic_pointer_cast<ScreenGui>(ServerGui->FindFirstChild("PrivateHud", false))->GetEnabled(),
			"persistent HUD presentation edits survive the respawn and stay local to the client");
		First.Stop();
		Pump(15);
		Check(Gui->GetDestroyed() && ServerGui->GetDestroyed() && !OtherGui->GetDestroyed(),
			"disconnect retires the owned GUI on both endpoints and preserves the other player's GUI");
		GameSession Rejoined(Network->CreateTransport(), FirstConfig);
		Check(Rejoined.Start().Succeeded(), "disconnected identity can establish a new session");
		Pump(75, &Rejoined);
		const auto RejoinedPlayer = RejoinedRuntime ? RejoinedRuntime->Players->GetLocalPlayer().value_or(nullptr) : nullptr;
		const auto ServerRejoined = RejoinedPlayer && std::ranges::any_of(Server.GetPeerIdentities(), [&](const auto &Peer) {
			return Peer.Ready && Peer.Nonce == FirstConfig.ClientNonce &&
				Peer.PlayerId == static_cast<std::uint32_t>(RejoinedPlayer->GetPlayerId());
		});
		Check(Rejoined.GetStatus() == GameSessionStatus::Ready && ServerRejoined,
			"the returning client and its corresponding server peer both reach gameplay readiness");
		const auto RejoinedGui = RejoinedPlayer ? RejoinedPlayer->GetPlayerGui().value_or(nullptr) : nullptr;
		const auto RejoinedHud = RejoinedGui ? std::dynamic_pointer_cast<ScreenGui>(
			RejoinedGui->FindFirstChild("PrivateHud", false)) : nullptr;
		const auto RejoinedBriefing = RejoinedGui ? RejoinedGui->FindFirstChild("Briefing", false) : nullptr;
		Check(RejoinedPlayer && RejoinedPlayer->GetPlayerId() != Owner->GetPlayerId() &&
			RejoinedGui && RejoinedGui->GetObjectId() != Gui->GetObjectId() && RejoinedHud && RejoinedHud->GetEnabled() &&
			RejoinedBriefing && RejoinedBriefing->GetAttributeValue("LocalLogicRan") == std::optional<WireValue>(true),
			"reconnection provisions fresh GUI identities and template state");
		Rejoined.Stop();
		Second.Stop();
		Server.Stop();
		FirstRuntime->Destroy();
		SecondRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestServerCharacterAutoLoadsPolicy() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto ClientTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		ServerRuntime.Players->SetCharacterAutoLoads(false);
		GameSession Server(ServerTransport, Configuration(GameSessionRole::Server, "autoload-policy"), &ServerRuntime);
		GameSession Client(ClientTransport, Configuration(GameSessionRole::Client, "autoload-policy"));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "CharacterAutoLoads=false session starts");
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		for (std::uint64_t Tick = 1; Tick <= 100 && Server.GetMetrics().ReadyPeers != 1; ++Tick) {
			Advance(Network, Server, Client, Tick);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				ClientRuntime = std::make_unique<Engine>(
					Client.GetClientDataModel(),
					ClientRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "CharacterAutoLoads=false client attaches");
			}
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
		}
		auto ServerPlayers = ServerRuntime.Players->GetPlayers();
		Check(
			ServerPlayers.size() == 1 && !ServerPlayers.front()->GetCharacter() &&
				Server.GetMetrics().CharacterControlBindings == 0,
			"server CharacterAutoLoads=false permits a ready Player without inventing a Character"
		);
		if (!ServerPlayers.empty()) ServerPlayers.front()->LoadCharacter();
		for (std::uint64_t Tick = 101; Tick <= 125; ++Tick) {
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
			Advance(Network, Server, Client, Tick);
		}
		Check(
			!ServerPlayers.empty() && ServerPlayers.front()->GetCharacter() &&
				Server.GetMetrics().CharacterControlBindings == 1,
			"explicit server LoadCharacter materializes and binds after a Player-only ready phase"
		);
		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestGenericPossessionRuntime(bool NativeTransport = false, std::size_t PeerCount = 2, bool Measure = false,
		const test_detail::GameplayScalingOptions *Scale = nullptr) {
		using ScalingCapture = test_detail::GameplayScalingCapture;
		using Phase = ScalingCapture::Phase;
		std::optional<ScalingCapture> Scaling;
		if (Scale) Scaling.emplace(*Scale);
		auto Network = NativeTransport ? nullptr : SimulatedNetwork::Create({.BaseLatency = 6ms});
		const auto CreateTransport = [&]() -> std::shared_ptr<IGameTransport> {
			if (Network) return Network->CreateTransport();
#if defined(GARGANTUAN_WITH_GNS)
			return std::make_shared<GameNetworkingSocketsTransport>();
#else
			throw std::runtime_error("The native possession suite requires GNS");
#endif
		};
		auto World = std::make_shared<DataModel>();
		auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		HeadlessRenderer Renderer(Vector2(320, 240));
		Engine Runtime(World, &Renderer, nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		Runtime.ProcessService->Alive = true;
		auto Drone = std::make_shared<Part>();
		Drone->SetName("PossessionDrone");
		Drone->SetAnchored(true);
		Drone->SetParent(Runtime.Workspace);
		auto Rover = std::make_shared<Part>();
		Rover->SetName("PossessionRover");
		Rover->SetAnchored(true);
		Rover->SetParent(Runtime.Workspace);
		std::vector<std::shared_ptr<Part>> Targets{Drone, Rover};
		for (std::size_t Index = Targets.size(); Index < PeerCount; ++Index) {
			auto Target = std::make_shared<Part>();
			Target->SetName("PossessionBody" + std::to_string(Index));
			Target->SetAnchored(true);
			Target->SetParent(Runtime.Workspace);
			Targets.push_back(std::move(Target));
		}
		const auto AddScript = [](const std::shared_ptr<DataModel> &Target, Enums::RunContext Context,
								  std::string Source) {
			auto Value = std::make_shared<Script>();
			Value->SetRunContext(Context);
			Value->SetSource(std::move(Source));
			Value->SetParent(Target);
			return Value;
		};
		auto ServerScript = AddScript(World, Enums::RunContext::Server, R"(
local Players = game:GetService("Players")
local Control = game:GetService("CharacterControlService")
game:GetService("RunService").PreSimulation:Connect(function()
	for _, Player in Players:GetPlayers() do
		local Entity = Player.PossessedEntity
		local Input = Control:GetPossessionInput(Player)
		if Entity and Input and Input.Fresh then
			local Move = Input.Movement
			local Target = CFrame.new(Entity.Position + Vector3.new(Move.X, 0, Move.Y) * 0.25)
			if Control:MovePossessedPart(Player, Target) then
				assert(not Control:MovePossessedPart(Player, Target), "movement must be limited to once per tick")
			end
		end
	end
end)
)");
		auto ServerConfiguration = Configuration(
			GameSessionRole::Server, NativeTransport ? "127.0.0.1" : "generic-possession");
		std::unique_ptr<GameSession> ServerSession;
		std::shared_ptr<IGameTransport> ServerTransport;
		for (std::uint16_t Port = 37910; Port < 37926; ++Port) {
			if (NativeTransport) ServerConfiguration.Endpoint.Port = Port;
			ServerTransport = CreateTransport();
			ServerSession = std::make_unique<GameSession>(ServerTransport, ServerConfiguration, &Runtime);
			if (ServerSession->Start().Succeeded()) break;
			ServerSession.reset();
			if (!NativeTransport) break;
		}
		Check(ServerSession != nullptr, "generic possession server starts");
		if (!ServerSession) return;
		auto &Server = *ServerSession;
		struct ClientFixture {
			std::unique_ptr<HeadlessRenderer> Renderer;
			std::unique_ptr<Engine> Runtime;
			std::unique_ptr<GameSession> Session;
		};
		std::vector<ClientFixture> Clients(PeerCount);
		const auto StartClient = [&](std::size_t Index, std::uint64_t NonceOffset) {
			auto ClientConfiguration = Configuration(
				GameSessionRole::Client, NativeTransport ? "127.0.0.1" : "generic-possession");
			ClientConfiguration.Endpoint = ServerConfiguration.Endpoint;
			ClientConfiguration.ClientNonce += NonceOffset;
			Clients[Index].Session = std::make_unique<GameSession>(CreateTransport(), ClientConfiguration);
			Check(Clients[Index].Session->Start().Succeeded(), "generic possession client starts");
		};
		for (std::size_t Index = 0; Index < Clients.size(); ++Index) StartClient(Index, Index);
		std::uint64_t Tick = 0;
		runtime_detail::WorkSample *FrameWork = nullptr;
		bool PaceMeasuredFrames = false;
		std::chrono::steady_clock::time_point Deadline{};
		std::uint64_t ServerAllocationBytes = 0;
		const auto SampleServer = [&](auto &&Operation) {
			if (!FrameWork) {
				Operation();
				return;
			}
			const auto Before = GameplayWorkloadAllocationBytes();
			runtime_detail::WorkCapture Capture(FrameWork);
			Operation();
			ServerAllocationBytes += GameplayWorkloadAllocationBytes() - Before;
		};
		const auto Sample = [&](Phase Work, auto &&Operation) {
			if (Scaling) Scaling->Measure(Work, Operation, GameplayWorkloadAllocationCount, GameplayWorkloadAllocationBytes);
			else Operation();
		};
		const auto Frame = [&] {
			if (Network) {
				Sample(Phase::Pump, [&] {
					(void)Network->Advance(17ms);
					Network->Pump();
				});
			} else if (PaceMeasuredFrames) {
				Deadline += 16'667us;
				const auto BeforeWait = std::chrono::steady_clock::now();
				Sample(Phase::Wait, [&] { std::this_thread::sleep_until(Deadline); });
				Scaling->RecordDeadline(Deadline, BeforeWait);
			} else Sample(Phase::Wait, [] { std::this_thread::sleep_for(16ms); });
			Sample(Phase::ServerPoll, [&] { SampleServer([&] { (void)Server.Poll(); }); });
			Sample(Phase::ClientPoll, [&] {
				for (std::size_t Index = 0; Index < Clients.size(); ++Index) {
					auto &Client = Clients[Index];
					if (Scaling) Scaling->SetEndpoint(static_cast<std::uint32_t>(Index + 1));
					(void)Client.Session->Poll();
					if (!Client.Runtime && Client.Session->GetClientDataModel()) {
						Client.Renderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
						Client.Runtime = std::make_unique<Engine>(Client.Session->GetClientDataModel(),
							Client.Renderer.get(), nullptr,
							EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
						Client.Runtime->ProcessService->Alive = true;
						Check(Client.Session->AttachClientRuntime(*Client.Runtime),
							"generic possession client attaches through the normal bootstrap");
						(void)AddScript(Client.Runtime->DataModel, Enums::RunContext::Client, R"(
local Players = game:GetService("Players")
local Control = game:GetService("CharacterControlService")
game:GetService("RunService").PreSimulation:Connect(function()
	local Player = Players.LocalPlayer
	local Entity = Player and Player.PossessedEntity
	if Entity then
		local Intent = Entity.Name == "PossessionDrone" and Vector2.new(1, 0) or Vector2.new(0, 1)
		Control:SubmitPossessionIntent(Intent, 0, false)
	end
end)
)");
					}
				}
			});
			Sample(Phase::ServerEngine, [&] { SampleServer([&] { Runtime.Step(); }); });
			Sample(Phase::ClientEngine, [&] {
				for (std::size_t Index = 0; Index < Clients.size(); ++Index) {
					if (Scaling) Scaling->SetEndpoint(static_cast<std::uint32_t>(Index + 1));
					if (Clients[Index].Runtime) Clients[Index].Runtime->Step();
				}
			});
			Sample(Phase::ServerSession, [&] { SampleServer([&] { Server.Step(++Tick); }); });
			Sample(Phase::ClientSession, [&] {
				for (std::size_t Index = 0; Index < Clients.size(); ++Index) {
					auto &Client = Clients[Index];
					if (Scaling) Scaling->SetEndpoint(static_cast<std::uint32_t>(Index + 1));
					Client.Session->Step(Client.Runtime ? Client.Runtime->GetSimulationTick() : Tick);
				}
			});
		};
		while (Tick < 100 && Server.GetMetrics().ReadyPeers != PeerCount) Frame();
		auto LivePlayers = PlayersValue->GetPlayers();
		Check(LivePlayers.size() == PeerCount && Server.GetMetrics().ReadyPeers == PeerCount,
			"all peers establish distinct server Players");
		if (LivePlayers.size() == PeerCount && Server.GetMetrics().ReadyPeers == PeerCount) {
			const auto AssignedAt = Tick;
			for (std::size_t Index = 0; Index < PeerCount; ++Index)
				LivePlayers[Index]->SetPossession(Targets[Index]);
			if (Scale) {
				bool Rejected = false;
				try {
					LivePlayers[1]->SetPossession(Drone);
				} catch (const std::invalid_argument &) {
					Rejected = true;
				}
				Check(Rejected && LivePlayers[0]->GetPossessedEntity() == Drone && LivePlayers[1]->GetPossessedEntity() == Rover,
					"a workload peer cannot take another peer's live possession");
			}
			std::uint64_t AllBoundAt = 0;
			for (int Index = 0; Index < 25; ++Index) {
				Frame();
				if (Measure && AllBoundAt == 0 && Server.GetMetrics().PossessionInput.Bindings == PeerCount)
					AllBoundAt = Tick;
			}
			Check(Drone->GetPosition().x > 1 && Drone->GetPosition().z == 0 &&
					Rover->GetPosition().z > 1 && Rover->GetPosition().x == 0,
				"independent Luau intents move only the matching server-owned bodies");
			auto Metrics = Server.GetMetrics().PossessionInput;
			Check(Metrics.Bindings == PeerCount && Metrics.InputsConsumed > PeerCount && Metrics.InputBytesReceived > 0,
				"server reports bounded generic input admission and consumption");
			const auto GameplayMetrics = Server.GetMetrics();
			Check(GameplayMetrics.PossessionMotionAccepted > 2 && GameplayMetrics.PossessionPoseSnapshotsAccepted > 2 &&
					GameplayMetrics.PossessionPoseBytesAccepted > 0,
				"authoritative movement used the bounded realtime pose path");
			for (auto &Client : Clients) {
				auto ClientPlayer = Client.Runtime ? Client.Runtime->Players->GetLocalPlayer().value_or(nullptr) : nullptr;
				auto ProjectedEntity = ClientPlayer ? ClientPlayer->GetPossessedEntity().value_or(nullptr) : nullptr;
				auto ProjectedPart = std::dynamic_pointer_cast<Part>(ProjectedEntity);
				Check(ProjectedPart && glm::length(ProjectedPart->GetPosition()) > 0.5f &&
						!ClientPlayer->GetCharacter(),
					"each ready replica resolves its possessed entity and receives authoritative movement");
				if (Client.Runtime) {
					auto Remote = Client.Runtime->Workspace->FindFirstChild(
						ProjectedPart && ProjectedPart->GetName() == "PossessionDrone"
							? "PossessionRover" : "PossessionDrone", std::nullopt);
					auto RemotePart = std::dynamic_pointer_cast<Part>(Remote);
					Check(RemotePart && glm::length(RemotePart->GetPosition()) > 0.5f,
						"an interested peer receives the other possessed body's authoritative pose");
				}
			}
			if (!Measure && Network) {
				for (int Index = 0; Index < 24; ++Index) {
					(void)Network->Advance(17ms);
					Network->Pump();
					for (auto &Client : Clients) {
						(void)Client.Session->Poll();
						Client.Runtime->Step();
						Client.Session->Step(Client.Runtime->GetSimulationTick());
					}
				}
				for (int Index = 0; Index < 12; ++Index) Frame();
				const auto DroneAfterStall = Drone->GetPosition();
				const auto RoverAfterStall = Rover->GetPosition();
				for (int Index = 0; Index < 8; ++Index) Frame();
				Check(Drone->GetPosition().x > DroneAfterStall.x + 0.5f &&
						Rover->GetPosition().z > RoverAfterStall.z + 0.5f,
					"both controllers recover after the server loop stalls while client ticks advance");
			}
			if (Scaling) {
				constexpr std::size_t ExtraWarmupTicks = 155;
				for (std::size_t Index = 0; Index < ExtraWarmupTicks; ++Index) Frame();
				const auto Before = Server.GetMetrics();
				struct PeerSample {
					ConnectionId Connection;
					std::uint64_t Index;
					NetworkStatistics Transport;
					SchedulerStatistics Scheduler;
				};
				const auto BaselineNonce = Configuration(GameSessionRole::Client).ClientNonce;
				std::vector<PeerSample> PeerBefore;
				PeerBefore.reserve(PeerCount);
				for (const auto &Peer : Server.GetPeerIdentities()) {
					if (!Peer.Ready) continue;
					const auto Transport = ServerTransport->GetStatistics(Peer.Connection);
					const auto Scheduler = detail::GameSessionTestAccess::GetSchedulerStatistics(Server, Peer.Connection);
					Check(Transport && Scheduler && Peer.Nonce >= BaselineNonce && Peer.Nonce < BaselineNonce + PeerCount,
						"workload transport and scheduler expose the live peer");
					if (Transport && Scheduler)
						PeerBefore.push_back({Peer.Connection, Peer.Nonce - BaselineNonce + 1, *Transport, *Scheduler});
				}
				Check(PeerBefore.size() == PeerCount, "every workload peer has a counter baseline");
				std::vector<glm::vec3> PreviousPositions;
				PreviousPositions.reserve(PeerCount);
				for (const auto &Target : Targets) PreviousPositions.push_back(Target->GetPosition());

				using ClientCounters = detail::GameSessionTestAccess::GameplayClientCounters;
				std::vector<ClientCounters> PriorClients;
				PriorClients.reserve(Clients.size());
				for (const auto &Client : Clients)
					PriorClients.push_back(detail::GameSessionTestAccess::GetGameplayClientCounters(*Client.Session));
				std::uint64_t Produced = 0, Applied = 0;
				const auto TickBegin = Tick;
				auto PriorServer = Before;
				Scaling->Start();
				PaceMeasuredFrames = Scale->DeadlinePacing;
				Deadline = std::chrono::steady_clock::now();
				for (std::size_t Index = 0; Index < Scale->Ticks; ++Index) {
					Scaling->BeginFrame(Index, Tick + 1);
					Frame();
					std::uint64_t FrameProduced = 0, FrameApplied = 0, StructuralApply = 0, MaximumStructuralApply = 0;
					for (std::size_t Client = 0; Client < Clients.size(); ++Client) {
						const auto Current = detail::GameSessionTestAccess::GetGameplayClientCounters(*Clients[Client].Session);
						auto &Prior = PriorClients[Client];
						FrameProduced += Current.Produced - Prior.Produced;
						FrameApplied += Current.Applied - Prior.Applied;
						const auto ApplyCost = Current.StructuralApplyNanoseconds - Prior.StructuralApplyNanoseconds;
						StructuralApply += ApplyCost;
						MaximumStructuralApply = std::max(MaximumStructuralApply, ApplyCost);
						Prior = Current;
					}
					Produced += FrameProduced;
					Applied += FrameApplied;
					const auto Current = Server.GetMetrics();
					Scaling->RecordWorkload(Current.PossessionPoseSnapshotsAccepted - PriorServer.PossessionPoseSnapshotsAccepted,
						Current.PossessionPoseBytesAccepted - PriorServer.PossessionPoseBytesAccepted, FrameProduced,
						Current.PossessionInput.InputsAccepted - PriorServer.PossessionInput.InputsAccepted,
						Current.PossessionInput.InputsConsumed - PriorServer.PossessionInput.InputsConsumed,
						Current.PossessionInput.InputsCoalesced - PriorServer.PossessionInput.InputsCoalesced,
						FrameApplied, StructuralApply, MaximumStructuralApply);
					PriorServer = Current;
				}
				Scaling->Stop();
				PaceMeasuredFrames = false;
				const auto After = Server.GetMetrics();
				Check(PlayersValue->GetPlayers().size() == PeerCount && After.PlayersRemoved == Before.PlayersRemoved,
					"every workload peer remains connected");
				Check(After.ProtocolRejects == Before.ProtocolRejects &&
					After.PossessionInput.UnauthorizedInputs == Before.PossessionInput.UnauthorizedInputs &&
					After.PossessionInput.MalformedInputs == Before.PossessionInput.MalformedInputs,
					"the measured workload emits only authorized, well-formed inputs");
				for (std::size_t Index = 0; Index < PeerCount; ++Index) {
					const auto Delta = Targets[Index]->GetPosition() - PreviousPositions[Index];
					Check(LivePlayers[Index]->GetPossessedEntity() == Targets[Index] &&
						(Index == 0 ? Delta.x > 0 && Delta.z == 0 : Delta.z > 0 && Delta.x == 0),
						"every workload peer independently advances its authoritative entity");
				}
				for (const auto &Peer : PeerBefore) {
					const auto Transport = ServerTransport->GetStatistics(Peer.Connection);
					const auto Scheduler = detail::GameSessionTestAccess::GetSchedulerStatistics(Server, Peer.Connection);
					Check(Transport && Scheduler, "every workload peer still exposes its final counters");
					if (!Transport || !Scheduler) continue;
					const auto Delta = [](auto Start, auto End) -> std::optional<std::uint64_t> {
						if (!Start || !End || *End < *Start) return {};
						return *End - *Start;
					};
					const auto PrintOptional = [](auto Value) {
						if (Value) std::cout << *Value;
						else std::cout << "NA";
					};
					std::cout << "[Gameplay:Peer] " << Peer.Index << ',' << Peer.Connection.Slot << ',' << Peer.Connection.Generation << ',';
					PrintOptional(Delta(Peer.Transport.BytesSent, Transport->BytesSent));
					std::cout << ',';
					PrintOptional(Delta(Peer.Transport.BytesReceived, Transport->BytesReceived));
					std::cout << ',';
					PrintOptional(Delta(Peer.Transport.DroppedUnreliableMessages, Transport->DroppedUnreliableMessages));
					std::cout << ',';
					PrintOptional(Delta(Peer.Transport.DuplicatedUnreliableMessages, Transport->DuplicatedUnreliableMessages));
					std::cout << ',';
					PrintOptional(Transport->QueuedReliableBytes);
					std::cout << ',';
					if (Transport->EstimatedRoundTripTime) std::cout << Transport->EstimatedRoundTripTime->count();
					else std::cout << "NA";
					std::cout << ',' << Scheduler->QueuedMessages << ',' << Scheduler->QueuedUnreliableBytes
						<< ',' << Scheduler->IntentsRejected - Peer.Scheduler.IntentsRejected
						<< ',' << Scheduler->UnreliableMessagesDroppedBeforeTransport - Peer.Scheduler.UnreliableMessagesDroppedBeforeTransport
						<< ',' << Scheduler->SequencedStatesSuperseded - Peer.Scheduler.SequencedStatesSuperseded
						<< ',' << Scheduler->TransportBackpressureEvents - Peer.Scheduler.TransportBackpressureEvents << '\n';
				}
				std::cout << "[Gameplay:Scale] peers=" << PeerCount << " ticks=" << Scale->Ticks
					<< " intended_ticks=" << Scale->Ticks << " executed_ticks=" << Tick - TickBegin
					<< " warmup_ticks=" << 25 + ExtraWarmupTicks << " profile=" << Scale->Profile
					<< " transport=" << (NativeTransport ? "gns-loopback" : "simulated")
					<< " pacing=" << (Scale->DeadlinePacing ? "deadline" : "relative")
					<< " inputs_produced=" << Produced << " client_poses_applied=" << Applied
					<< " poses=" << After.PossessionPoseSnapshotsAccepted - Before.PossessionPoseSnapshotsAccepted
					<< " pose_bytes=" << After.PossessionPoseBytesAccepted - Before.PossessionPoseBytesAccepted
					<< " inputs=" << After.PossessionInput.InputsAccepted - Before.PossessionInput.InputsAccepted
					<< " inputs_consumed=" << After.PossessionInput.InputsConsumed - Before.PossessionInput.InputsConsumed
					<< " inputs_coalesced=" << After.PossessionInput.InputsCoalesced - Before.PossessionInput.InputsCoalesced
					<< " input_bytes=" << After.PossessionInput.InputBytesReceived - Before.PossessionInput.InputBytesReceived
					<< " stale_inputs=" << After.PossessionInput.StaleInputs - Before.PossessionInput.StaleInputs
					<< " expired_inputs=" << After.PossessionInput.InputsExpired - Before.PossessionInput.InputsExpired
					<< " rate_limited_inputs=" << After.PossessionInput.RateLimitedInputs - Before.PossessionInput.RateLimitedInputs
					<< " protocol_rejects=" << After.ProtocolRejects - Before.ProtocolRejects
					<< " unauthorized_inputs=" << After.PossessionInput.UnauthorizedInputs - Before.PossessionInput.UnauthorizedInputs
					<< " malformed_inputs=" << After.PossessionInput.MalformedInputs - Before.PossessionInput.MalformedInputs
					<< " motion_commits=" << After.PossessionMotionAccepted - Before.PossessionMotionAccepted
					<< " input_admission_ns=" << After.PossessionInput.AdmissionNanoseconds - Before.PossessionInput.AdmissionNanoseconds
					<< " motion_ns=" << After.PossessionMotionNanoseconds - Before.PossessionMotionNanoseconds
					<< " maximum_input_age_ticks=" << After.PossessionInput.MaximumConsumedInputAgeTicks << '\n';
				Scaling->Print(std::cout);
			} else if (Measure) {
				const auto Before = Server.GetMetrics();
				std::array<std::uint64_t, 120> SimulationCosts{};
				std::array<std::uint64_t, 120> SessionCosts{};
				std::uint64_t PhysicsTime = 0;
				for (std::size_t Index = 0; Index < SimulationCosts.size(); ++Index) {
					runtime_detail::WorkSample Work{};
					FrameWork = &Work;
					Frame();
					FrameWork = nullptr;
					const auto &EngineWork = Work[static_cast<std::size_t>(runtime_detail::WorkPhase::EngineStep)];
					const auto &SessionWork = Work[static_cast<std::size_t>(runtime_detail::WorkPhase::SessionStep)];
					SimulationCosts[Index] = EngineWork.Nanoseconds;
					SessionCosts[Index] = SessionWork.Nanoseconds;
					PhysicsTime += Work[static_cast<std::size_t>(runtime_detail::WorkPhase::Physics)].Nanoseconds;
				}
				std::ranges::sort(SimulationCosts);
				std::ranges::sort(SessionCosts);
				const auto After = Server.GetMetrics();
				std::cout << "[Gameplay:Qualification] peers=" << PeerCount << " ticks=" << SimulationCosts.size()
					<< " engine_p50_ns=" << SimulationCosts[59] << " engine_p95_ns=" << SimulationCosts[113]
					<< " engine_p99_ns=" << SimulationCosts[118] << " session_p99_ns=" << SessionCosts[118]
					<< " physics_total_ns=" << PhysicsTime
					<< " input_ns=" << After.PossessionInput.AdmissionNanoseconds - Before.PossessionInput.AdmissionNanoseconds
					<< " controller_ns=" << After.PossessionMotionNanoseconds - Before.PossessionMotionNanoseconds
					<< " pose_bytes=" << After.PossessionPoseBytesAccepted - Before.PossessionPoseBytesAccepted
					<< " poses=" << After.PossessionPoseSnapshotsAccepted - Before.PossessionPoseSnapshotsAccepted
					<< " maximum_input_age_ticks=" << After.PossessionInput.MaximumConsumedInputAgeTicks
					<< " possession_bind_ticks=" << (AllBoundAt == 0 ? 0 : AllBoundAt - AssignedAt);
#if defined(GARGANTUAN_GAMEPLAY_WORKLOAD_ALLOCATIONS)
				std::cout << " server_scalar_new_bytes=" << ServerAllocationBytes;
#endif
				std::cout << '\n';
			}
			LivePlayers[0]->SetPossession(std::nullopt);
			LivePlayers[1]->SetPossession(Drone);
			for (int Index = 0; Index < 15; ++Index) Frame();
			Metrics = Server.GetMetrics().PossessionInput;
			Check(Metrics.Bindings == PeerCount + 1 && Metrics.Revocations == 2 &&
					!LivePlayers[0]->GetPossessedEntity() && LivePlayers[1]->GetPossessedEntity() == Drone,
				"same-tick transfer retires both old leases and binds the new owner");
			Drone->Destroy();
			for (int Index = 0; Index < 8; ++Index) Frame();
			Check(!LivePlayers[1]->GetPossessedEntity() && Server.GetMetrics().PossessionInput.Revocations == 3,
				"entity destruction retires its input lease");
			if (!Measure) {
				for (const auto &Client : Clients)
					Check(Client.Runtime && !Client.Runtime->Workspace->FindFirstChild("PossessionDrone", std::nullopt),
						"structural removal retires every presented replica of the old identity");
			}
			if (!Measure && Clients.front().Runtime) {
				auto &Departing = Clients.front();
				const auto LocalBefore = Departing.Runtime->Players->GetLocalPlayer().value_or(nullptr);
				const auto PreviousPlayerId = LocalBefore ? LocalBefore->GetPlayerId() : 0;
				Departing.Session->Stop();
				Departing.Runtime->Destroy();
				Departing.Runtime.reset();
				Departing.Renderer.reset();
				for (int Index = 0; Index < 60 && PlayersValue->GetPlayers().size() == PeerCount; ++Index) Frame();
				Check(PlayersValue->GetPlayers().size() == PeerCount - 1, "disconnect retires the server Player");
				StartClient(0, PeerCount + 1);
				std::shared_ptr<Player> Reconnected;
				for (int Index = 0; Index < 60 && !Reconnected; ++Index) {
					Frame();
					for (const auto &PeerValue : Server.GetPeerIdentities()) {
						if (!PeerValue.Ready || PeerValue.Nonce !=
								Configuration(GameSessionRole::Client).ClientNonce + PeerCount + 1) continue;
						for (const auto &ServerPlayer : PlayersValue->GetPlayers())
							if (ServerPlayer->GetPlayerId() == static_cast<int>(PeerValue.PlayerId))
								Reconnected = ServerPlayer;
					}
				}
				Check(PreviousPlayerId > 0 && Reconnected && Reconnected->GetPlayerId() != PreviousPlayerId &&
						!Reconnected->GetPossessedEntity(),
					"reconnect creates a different server identity with no inherited possession");
				if (Reconnected) {
					const auto PreviousRoverPose = Rover->GetPosition();
					Reconnected->SetPossession(Rover);
					for (int Index = 0; Index < 25; ++Index) Frame();
					Check(Server.GetMetrics().PossessionInput.Bindings == PeerCount + 2 &&
							Rover->GetPosition().z > PreviousRoverPose.z,
						"reconnected Player moves through a fresh generic lease");
				}
			}
		}
		for (auto &Client : Clients) Client.Session->Stop();
		if (Scale) {
			for (std::size_t Index = 0; Index < 120 && !PlayersValue->GetPlayers().empty(); ++Index) Frame();
			Check(PlayersValue->GetPlayers().empty() && Server.GetPeerIdentities().empty(),
				"all workload peers complete disconnect cleanup");
		}
		Server.Stop();
		for (auto &Client : Clients)
			if (Client.Runtime) Client.Runtime->Destroy();
		Runtime.Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--generic-possession") {
			TestGenericPossessionRuntime();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--generic-possession-workload") {
			TestGenericPossessionRuntime(false, 2, true);
			TestGenericPossessionRuntime(false, 8, true);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--generic-possession-scale") {
			if (ArgumentCount != 6 && ArgumentCount != 7)
				throw std::invalid_argument("expected --generic-possession-scale <sim|gns> <2|8|16|24|32|40> <1..6000> <off|on> [relative|deadline]");
			const auto Parse = [](std::string_view Text) {
				std::size_t Value = 0;
				const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
				if (Error != std::errc{} || End != Text.data() + Text.size())
					throw std::invalid_argument("invalid workload integer");
				return Value;
			};
			const std::string_view Transport = Arguments[2];
			const auto Peers = Parse(Arguments[3]);
			const auto Ticks = Parse(Arguments[4]);
			const std::string_view Profile = Arguments[5];
			const std::string_view Pacing = ArgumentCount == 7 ? Arguments[6] : "relative";
			constexpr std::array Cohorts{2u, 8u, 16u, 24u, 32u, 40u};
			if ((Transport != "sim" && Transport != "gns") || std::ranges::find(Cohorts, Peers) == Cohorts.end() ||
				Ticks == 0 || Ticks > 6000 || (Profile != "off" && Profile != "on") ||
				(Pacing != "relative" && (Pacing != "deadline" || Transport != "gns")))
				throw std::invalid_argument("unsupported workload configuration");
			const gargantuan::test_detail::GameplayScalingOptions Options{Ticks, Profile == "on", Pacing == "deadline"};
			TestGenericPossessionRuntime(Transport == "gns", Peers, true, &Options);
			return Failures == 0 ? 0 : 1;
		}
#if defined(GARGANTUAN_WITH_GNS)
		if (ArgumentCount == 2 && (std::string_view(Arguments[1]) == "--terrain-gns" ||
				std::string_view(Arguments[1]) == "--terrain-gns-narrow")) {
			const bool Narrow = std::string_view(Arguments[1]) == "--terrain-gns-narrow";
			TestNetworkTerrainBootstrap(Narrow);
			if (!Narrow) TestNetworkTerrainActorEnvelope();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--terrain-gns-envelope") {
			TestNetworkTerrainActorEnvelope();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--terrain-gns-churn") {
			auto Cycles = DefaultTerrainChurnCycles;
			if (ArgumentCount == 3) {
				const std::string_view Text = Arguments[2];
				const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Cycles);
				if (Error != std::errc{} || End != Text.data() + Text.size() || Cycles == 0 || Cycles > MaximumTerrainChurnCycles)
					throw std::invalid_argument("expected --terrain-gns-churn [1..120]");
			} else if (ArgumentCount != 2) {
				throw std::invalid_argument("expected --terrain-gns-churn [1..120]");
			}
			TestNetworkTerrainBootstrap(false, Cycles);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--generic-possession-gns") {
			TestGenericPossessionRuntime(true);
			return Failures == 0 ? 0 : 1;
		}
#endif
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--relevance-materialization") {
			TestSpeculativeRelevanceMaterialization();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--farm-publication-overhead") {
			constexpr int Samples = 500'000;
			const auto Sample = [] {
				for (int Index = 0; Index < Samples; ++Index)
					runtime_detail::RecordPublicationLatency({.Stage = "CharacterDue", .Connection = {1, 1},
						.Object = {9, 1}, .Tick = static_cast<std::uint64_t>(Index), .Due = 1, .Kind = 5});
			};
			const auto OffBegin = std::chrono::steady_clock::now();
			Sample();
			const auto OffEnd = std::chrono::steady_clock::now();
			const auto Directory = std::filesystem::temp_directory_path() /
				("farm-publication-overhead-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directory(Directory);
			const auto Path = Directory / "publication-service.bin";
			std::chrono::steady_clock::time_point OnBegin, OnEnd;
			{
				host::detail::FarmPublicationEvidence Evidence(true, "overhead", -1, 0, Path);
				OnBegin = std::chrono::steady_clock::now();
				Sample();
				OnEnd = std::chrono::steady_clock::now();
				if (Evidence.Count() != Samples) throw std::runtime_error("farm publication overhead sample lost records");
			}
			std::filesystem::remove(Path);
			CharacterStateFrame Frame{.ServerTick = 31, .FrameSequence = CharacterStateFrameSequence{1},
				.StateCount = 8};
			for (int Index = 0; Index < 8; ++Index)
				Frame.States[Index] = CharacterAuthoritativeState{.Character = {static_cast<std::uint32_t>(9 + Index), 1},
					.ControlEpoch = CharacterControlEpoch{7}, .StateSequence = RealtimeStateSequence{12},
					.AuthoritativeTick = 31};
			const auto Packet = EncodeCharacterMessage(Frame);
			if (!Packet) throw std::runtime_error("farm publication packet benchmark fixture is invalid");
			constexpr int Packets = 100'000;
			const auto PacketSample = [&] {
				for (int Index = 0; Index < Packets; ++Index)
					runtime_detail::RecordPublicationPacket("SchedulerAccepted", {1, 1}, *Packet);
			};
			const auto PacketOffBegin = std::chrono::steady_clock::now();
			PacketSample();
			const auto PacketOffEnd = std::chrono::steady_clock::now();
			std::chrono::steady_clock::time_point PacketOnBegin, PacketOnEnd;
			{
				host::detail::FarmPublicationEvidence Evidence(true, "overhead", -1, 0, Path);
				PacketOnBegin = std::chrono::steady_clock::now();
				PacketSample();
				PacketOnEnd = std::chrono::steady_clock::now();
				if (Evidence.Count() != Packets * 8)
					throw std::runtime_error("farm publication packet sample lost records");
			}
			std::filesystem::remove(Path);
			std::filesystem::remove(Directory);
			const auto OffNs = std::chrono::duration_cast<std::chrono::nanoseconds>(OffEnd - OffBegin).count();
			const auto OnNs = std::chrono::duration_cast<std::chrono::nanoseconds>(OnEnd - OnBegin).count();
			std::cout << "[Qualification:Publication] benchmark_samples=" << Samples
				<< " off_ns_per_event=" << double(OffNs) / Samples
				<< " on_ns_per_event=" << double(OnNs) / Samples
				<< " packet_states=8 packet_samples=" << Packets
				<< " packet_off_ns=" << double(std::chrono::duration_cast<std::chrono::nanoseconds>(
					PacketOffEnd - PacketOffBegin).count()) / Packets
				<< " packet_on_ns=" << double(std::chrono::duration_cast<std::chrono::nanoseconds>(
					PacketOnEnd - PacketOnBegin).count()) / Packets << '\n';
			return 0;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--character-retirement") {
			TestCharacterRetirementAcrossStructuralFrames();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--player-gui") {
			TestPlayerGuiPrivateSession();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--remote-player") {
			TestTwoClientIdentityAndControlIsolation();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--control-rebind") {
			TestProductionLifecycleComposition(false, false, true);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--nonpredictable-action") {
			TestProductionLifecycleComposition(false, false, false, false);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--replicated-character-template") {
			TestProductionLifecycleComposition(false, false, false, false, true);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--scale-counter") {
			TestPhysicalScaleCounterDecoding();
			TestRecoveryConvergenceBound();
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--late-handoff") {
			TestProductionLifecycleComposition(true);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--byte-admission") {
			TestRejectedStructuralAdmissionIsPeerTerminal(true);
			TestProductionLifecycleComposition(true, true);
			TestProductionLifecycleComposition(false, true);
			return Failures == 0 ? 0 : 1;
		}
		if (ArgumentCount != 1) throw std::invalid_argument("Unknown game-session test selection");
		TestGroundedNetworkLocomotion();
		TestPhysicalScaleCounterDecoding();
		TestRecoveryConvergenceBound();
		TestPublicationLatencyBounds();
		TestFarmPublicationEvidence();
		TestFarmF1Evidence();
		TestFarmServiceTimingEvidence();
		TestFarmOrdinaryEvidence();
		TestProtocolBounds();
		TestAuthenticatedAdmission();
		TestServerSessionSignalLifetime();
		TestSessionOwnershipAndEndpointPolicy();
		TestRegistrationCommitAfterCapacityFailure();
		TestClientBootstrapFailureMatrix();
		TestPreAcceptanceAndTimeoutRejection();
		TestSpoofedReadyPlayerRejection();
		TestBurstAdmissionKeepsJournalCursorsCurrent();
		TestRejectedStructuralAdmissionIsPeerTerminal();
		TestRejectedStructuralAdmissionIsPeerTerminal(true);
		TestProductionLifecycleComposition(true, true);
		TestProductionLifecycleComposition(false, true);
		TestAcceptedConnectionChurn();
		for (int Cycle = 0; Cycle < 100; ++Cycle)
			TestProductionLifecycleComposition();
		TestProductionLifecycleComposition(false, false, false, false);
		TestProductionLifecycleComposition(false, false, false, false, true);
		TestTwoClientIdentityAndControlIsolation();
		TestPlayerGuiPrivateSession();
		TestProductionLifecycleComposition(false, false, true);
		TestCharacterRetirementAcrossStructuralFrames();
		TestServerCharacterAutoLoadsPolicy();
		TestGenericPossessionRuntime();
		TestSpeculativeRelevanceMaterialization();
	} catch (const std::exception &Error) {
		std::cerr << "Unexpected game-session test exception: " << Error.what() << '\n';
		++Failures;
	}
	if (Failures == 0) std::cout << "Game session tests passed\n";
	return Failures == 0 ? 0 : 1;
}
