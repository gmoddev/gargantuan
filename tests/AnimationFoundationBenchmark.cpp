#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ProximityPrompt.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/InteractionService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {
	std::atomic<std::uint64_t> SemanticBenchmarkAllocations = 0;
}

void *operator new(std::size_t Size) {
	SemanticBenchmarkAllocations.fetch_add(1, std::memory_order_relaxed);
	if (auto *Value = std::malloc(Size)) return Value;
	throw std::bad_alloc();
}

void operator delete(void *Value) noexcept {
	std::free(Value);
}

void operator delete(void *Value, std::size_t) noexcept {
	std::free(Value);
}

namespace gargantuan {
	struct InteractionServiceTestAccess {
		static void Attach(
			InteractionService &Service,
			const std::shared_ptr<DataModel> &World,
			const std::shared_ptr<Players> &PlayersValue,
			const std::shared_ptr<ActionMap> &Actions,
			const std::shared_ptr<SemanticSpatialResolver> &Spatial
		) {
			Service.AttachRuntime(World, PlayersValue, Actions, Spatial, false);
		}
		static void ProcessDirty(InteractionService &Service) {
			Service.ProcessDirtyPrompts();
		}
		static void Shutdown(InteractionService &Service) {
			Service.ShutdownRuntime();
		}
	};
}

namespace {
	using Json = nlohmann::ordered_json;
	using Clock = std::chrono::steady_clock;
	using namespace gargantuan;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::uint64_t Nanoseconds(Clock::time_point Started) {
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Started).count());
	}

	struct Percentiles {
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
	};

	Percentiles Summarize(std::vector<std::uint64_t> Samples) {
		if (Samples.empty()) return {};
		std::ranges::sort(Samples);
		auto At = [&](double Fraction) {
			const auto Position = std::min(
				Samples.size() - 1,
				static_cast<std::size_t>(std::ceil(Fraction * static_cast<double>(Samples.size())) - 1.0));
			return static_cast<double>(Samples[Position]) / 1'000.0;
		};
		return {At(0.50), At(0.95), At(0.99)};
	}

	class BenchmarkAudioBackend final : public IAudioBackend {
	  public:
		[[nodiscard]] bool IsAvailable() const override { return true; }
		[[nodiscard]] std::uint32_t GetSampleRate() const override { return 48'000; }
		[[nodiscard]] std::size_t GetQueuedFrames() override { return 0; }
		[[nodiscard]] bool Submit(std::span<const float> InterleavedStereo) override {
			Metrics.SubmittedFrames += InterleavedStereo.size() / 2;
			return true;
		}
		void Clear() override {}
		void Shutdown() override {}
		[[nodiscard]] std::string GetDiagnostic() const override { return {}; }
		[[nodiscard]] AudioBackendMetrics GetMetrics() const override { return Metrics; }

	  private:
		AudioBackendMetrics Metrics;
	};

	void AppendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
		Bytes.push_back(static_cast<std::uint8_t>(Value));
		Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
	}

	void AppendU32(std::vector<std::uint8_t> &Bytes, std::uint32_t Value) {
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.push_back(static_cast<std::uint8_t>(Value >> (Shift * 8)));
	}

	std::vector<std::uint8_t> MakeWave() {
		constexpr std::uint32_t Frames = 48'000;
		constexpr std::uint32_t SampleRate = 48'000;
		const auto DataBytes = Frames * sizeof(std::int16_t);
		std::vector<std::uint8_t> Bytes;
		Bytes.reserve(44 + DataBytes);
		Bytes.insert(Bytes.end(), {'R', 'I', 'F', 'F'});
		AppendU32(Bytes, 36 + DataBytes);
		Bytes.insert(Bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
		AppendU32(Bytes, 16);
		AppendU16(Bytes, 1);
		AppendU16(Bytes, 1);
		AppendU32(Bytes, SampleRate);
		AppendU32(Bytes, SampleRate * sizeof(std::int16_t));
		AppendU16(Bytes, sizeof(std::int16_t));
		AppendU16(Bytes, 16);
		Bytes.insert(Bytes.end(), {'d', 'a', 't', 'a'});
		AppendU32(Bytes, DataBytes);
		for (std::uint32_t Frame = 0; Frame < Frames; ++Frame) {
			const auto Phase = static_cast<double>(Frame) * 440.0 * 6.283185307179586 / SampleRate;
			AppendU16(Bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::sin(Phase) * 8'192.0)));
		}
		return Bytes;
	}

	void AppendFloat(std::vector<std::uint8_t> &Bytes, float Value) {
		const auto Bits = std::bit_cast<std::uint32_t>(Value);
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.push_back(static_cast<std::uint8_t>(Bits >> (Shift * 8)));
	}

	void AlignFour(std::vector<std::uint8_t> &Bytes) {
		while (Bytes.size() % 4 != 0) Bytes.push_back(0);
	}

	struct FixtureBuilder {
		std::vector<std::uint8_t> Binary;
		Json Views = Json::array();
		Json Accessors = Json::array();

		std::size_t AddFloatView(std::span<const float> Values) {
			AlignFour(Binary);
			const auto Offset = Binary.size();
			for (const auto Value : Values) AppendFloat(Binary, Value);
			Views.push_back({{"buffer", 0}, {"byteOffset", Offset}, {"byteLength", Values.size_bytes()}});
			return Views.size() - 1;
		}

		std::size_t AddU16View(std::span<const std::uint16_t> Values) {
			AlignFour(Binary);
			const auto Offset = Binary.size();
			for (const auto Value : Values) AppendU16(Binary, Value);
			Views.push_back({{"buffer", 0}, {"byteOffset", Offset}, {"byteLength", Values.size_bytes()}});
			return Views.size() - 1;
		}

		std::size_t AddAccessor(
			std::size_t View,
			std::uint32_t ComponentType,
			std::size_t Count,
			std::string Type
		) {
			Accessors.push_back({{"bufferView", View}, {"componentType", ComponentType},
				{"count", Count}, {"type", std::move(Type)}});
			return Accessors.size() - 1;
		}
	};

	void WriteBytes(const std::filesystem::path &Path, std::span<const std::uint8_t> Bytes) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		if (!Output) throw std::runtime_error("Could not write animation benchmark fixture");
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Text) {
		WriteBytes(Path, std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size()));
	}

	void WriteRigFixture(const std::filesystem::path &Root, std::size_t BoneCount, std::size_t VertexCount) {
		Require(BoneCount > 0 && BoneCount <= AssetLimits::MaximumSkeletonBones,
			"benchmark bone count is out of range");
		VertexCount = std::max<std::size_t>(3, VertexCount / 3 * 3);
		FixtureBuilder Builder;
		std::vector<float> Positions;
		std::vector<float> Normals;
		std::vector<std::uint16_t> Joints;
		std::vector<float> Weights;
		std::vector<std::uint16_t> Indices;
		Positions.reserve(VertexCount * 3);
		Normals.reserve(VertexCount * 3);
		Joints.reserve(VertexCount * 4);
		Weights.reserve(VertexCount * 4);
		Indices.reserve(VertexCount);
		for (std::size_t Vertex = 0; Vertex < VertexCount; ++Vertex) {
			const auto TriangleVertex = Vertex % 3;
			const auto Triangle = Vertex / 3;
			Positions.insert(Positions.end(), {
				static_cast<float>(Triangle % 32) * 0.02f + (TriangleVertex == 1 ? 0.01f : 0.0f),
				static_cast<float>(Triangle / 32) * 0.02f + (TriangleVertex == 2 ? 0.01f : 0.0f),
				0.0f,
			});
			Normals.insert(Normals.end(), {0.0f, 0.0f, 1.0f});
			const auto Joint = static_cast<std::uint16_t>(Vertex % BoneCount);
			Joints.insert(Joints.end(), {Joint, Joint, Joint, Joint});
			Weights.insert(Weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
			Indices.push_back(static_cast<std::uint16_t>(Vertex));
		}

		const auto PositionAccessor = Builder.AddAccessor(Builder.AddFloatView(Positions), 5126, VertexCount, "VEC3");
		const auto NormalAccessor = Builder.AddAccessor(Builder.AddFloatView(Normals), 5126, VertexCount, "VEC3");
		const auto JointAccessor = Builder.AddAccessor(Builder.AddU16View(Joints), 5123, VertexCount, "VEC4");
		const auto WeightAccessor = Builder.AddAccessor(Builder.AddFloatView(Weights), 5126, VertexCount, "VEC4");
		const auto IndexAccessor = Builder.AddAccessor(Builder.AddU16View(Indices), 5123, VertexCount, "SCALAR");

		std::vector<float> InverseBinds;
		InverseBinds.reserve(BoneCount * 16);
		for (std::size_t Bone = 0; Bone < BoneCount; ++Bone) {
			for (std::size_t Column = 0; Column < 4; ++Column) for (std::size_t Row = 0; Row < 4; ++Row) {
				float Value = Column == Row ? 1.0f : 0.0f;
				if (Column == 3 && Row == 1 && Bone > 0) Value = -0.01f;
				InverseBinds.push_back(Value);
			}
		}
		const auto InverseBindAccessor = Builder.AddAccessor(
			Builder.AddFloatView(InverseBinds), 5126, BoneCount, "MAT4");
		const std::array Times{0.0f, 1.0f};
		const auto TimeAccessor = Builder.AddAccessor(Builder.AddFloatView(Times), 5126, Times.size(), "SCALAR");
		std::vector<std::size_t> TranslationAccessors;
		TranslationAccessors.reserve(BoneCount);
		for (std::size_t Bone = 0; Bone < BoneCount; ++Bone) {
			const auto BindY = Bone == 0 ? 0.0f : 0.01f;
			const std::array Values{0.0f, BindY, 0.0f, 0.01f, BindY, 0.0f};
			TranslationAccessors.push_back(Builder.AddAccessor(
				Builder.AddFloatView(Values), 5126, Times.size(), "VEC3"));
		}
		AlignFour(Builder.Binary);

		Json Nodes = Json::array();
		for (std::size_t Bone = 0; Bone < BoneCount; ++Bone) {
			Json Node{{"name", "B" + std::to_string(Bone)}};
			if (Bone > 0) Node["translation"] = {0.0, 0.01, 0.0};
			if (Bone == 0 && BoneCount > 1) {
				Node["children"] = Json::array();
				for (std::size_t Child = 1; Child < BoneCount; ++Child) Node["children"].push_back(Child);
			}
			Nodes.push_back(std::move(Node));
		}
		Nodes.push_back({{"name", "BenchmarkMesh"}, {"mesh", 0}, {"skin", 0}});
		Json SkinJoints = Json::array();
		for (std::size_t Bone = 0; Bone < BoneCount; ++Bone) SkinJoints.push_back(Bone);
		Json Samplers = Json::array();
		Json Channels = Json::array();
		for (std::size_t Bone = 0; Bone < BoneCount; ++Bone) {
			Samplers.push_back({{"input", TimeAccessor}, {"output", TranslationAccessors[Bone]},
				{"interpolation", "LINEAR"}});
			Channels.push_back({{"sampler", Bone}, {"target", {{"node", Bone}, {"path", "translation"}}}});
		}
		Json Document{
			{"asset", {{"version", "2.0"}, {"generator", "Gargantuan Animation benchmark"}}},
			{"buffers", Json::array({{{"byteLength", Builder.Binary.size()},
				{"uri", "rig-" + std::to_string(BoneCount) + ".bin"}}})},
			{"bufferViews", std::move(Builder.Views)},
			{"accessors", std::move(Builder.Accessors)},
			{"nodes", std::move(Nodes)},
			{"skins", Json::array({{{"name", "BenchmarkRig"}, {"joints", std::move(SkinJoints)},
				{"skeleton", 0}, {"inverseBindMatrices", InverseBindAccessor}}})},
			{"meshes", Json::array({{{"name", "Benchmark Mesh"}, {"primitives", Json::array({{
				{"attributes", {{"POSITION", PositionAccessor}, {"NORMAL", NormalAccessor},
					{"JOINTS_0", JointAccessor}, {"WEIGHTS_0", WeightAccessor}}},
				{"indices", IndexAccessor},
			}})}}})},
			{"animations", Json::array({{{"name", "Benchmark Clip"},
				{"samplers", std::move(Samplers)}, {"channels", std::move(Channels)}}})},
		};
		const auto Stem = "rig-" + std::to_string(BoneCount);
		WriteBytes(Root / "assets" / (Stem + ".bin"), Builder.Binary);
		WriteText(Root / "assets" / (Stem + ".gltf"), Document.dump());
	}

	struct ImportedRig {
		std::string Mesh;
		std::string Animation;
		std::size_t ArtifactBytes = 0;
	};

	std::string ImportAudio(AssetService &Assets, SourceMount &Mount) {
		auto Result = Assets.ImportProjectAsset(Mount, "assets/semantic-tone.wav",
			AssetKind::Audio, "Semantic Anchor Tone");
		Require(Result.Ok && Result.Records.size() == 1,
			"semantic anchor benchmark audio fixture did not import");
		return Result.Records.front().Reference.Value;
	}

	ImportedRig ImportRig(
		AssetService &Assets,
		SourceMount &Mount,
		std::size_t BoneCount
	) {
		auto Result = Assets.ImportProjectAsset(Mount, "assets/rig-" + std::to_string(BoneCount) + ".gltf",
			AssetKind::Mesh, "Benchmark Rig " + std::to_string(BoneCount));
		Require(Result.Ok, "animation benchmark fixture did not import: " + Result.Diagnostic.Code +
			" (" + Result.Diagnostic.Message + ")");
		ImportedRig Rig;
		for (const auto &Record : Result.Records) {
			if (Record.Kind == AssetKind::Mesh) Rig.Mesh = Record.Reference.Value;
			if (Record.Kind == AssetKind::Animation) Rig.Animation = Record.Reference.Value;
		}
		Require(!Rig.Mesh.empty() && !Rig.Animation.empty(), "benchmark import omitted Mesh or Animation");
		std::unordered_map<std::string, bool> ImportedArtifacts;
		for (const auto &Record : Result.Records)
			ImportedArtifacts.emplace(Record.ContentId.ToString() + ".gasset", true);
		for (const auto &Artifact : Assets.CaptureProjectAssets().Artifacts)
			if (Artifact.Bytes && ImportedArtifacts.contains(
				std::filesystem::path(Artifact.RelativePath).filename().string()))
				Rig.ArtifactBytes += Artifact.Bytes->size();
		return Rig;
	}

	struct ScenarioResult {
		double TotalMillisecondsPerFrame = 0.0;
		double RuntimeMillisecondsPerFrame = 0.0;
		double TrackAdvanceMillisecondsPerFrame = 0.0;
		double KeyframeLookupMillisecondsPerFrame = 0.0;
		double InterpolationMillisecondsPerFrame = 0.0;
		double TrackBlendingMillisecondsPerFrame = 0.0;
		double BindPoseFallbackMillisecondsPerFrame = 0.0;
		double SamplingBlendMillisecondsPerFrame = 0.0;
		double HierarchyMillisecondsPerFrame = 0.0;
		double SkinMatrixMillisecondsPerFrame = 0.0;
		double SkinningMillisecondsPerFrame = 0.0;
		double SemanticJointCacheMillisecondsPerFrame = 0.0;
		double PoseBuildMillisecondsPerFrame = 0.0;
		double PolicyMillisecondsPerFrame = 0.0;
		double PoseWorkerCpuMillisecondsPerFrame = 0.0;
		double PoseJobSchedulingMillisecondsPerFrame = 0.0;
		double PoseJobWaitMillisecondsPerFrame = 0.0;
		double PoseMergeMillisecondsPerFrame = 0.0;
		double PublisherMillisecondsPerFrame = 0.0;
		double ProjectionMillisecondsPerFrame = 0.0;
		double BindingBookkeepingMicrosecondsPerFrame = 0.0;
		double TransformResolutionMicrosecondsPerFrame = 0.0;
		Percentiles SemanticStepMicroseconds;
		Percentiles SoundUpdateMicroseconds;
		Percentiles InteractionUpdateMicroseconds;
		Percentiles TotalSemanticMicroseconds;
		std::uint64_t RuntimeBufferAllocationDelta = 0;
		std::uint64_t RuntimeSteadyStateAllocationDelta = 0;
		std::uint64_t SemanticAllocationDelta = 0;
		std::uint64_t SpatialAllocationDelta = 0;
		std::uint64_t SoundAllocationDelta = 0;
		std::uint64_t InteractionAllocationDelta = 0;
		std::uint64_t AnchorResolutionDelta = 0;
		std::uint64_t RigVisitDelta = 0;
		std::uint64_t PoseEvaluationDelta = 0;
		std::uint64_t PoseJobsScheduledDelta = 0;
		std::uint64_t PoseJobBatchDelta = 0;
		std::uint64_t StalePoseJobDropDelta = 0;
		std::size_t PoseWorkerCapacity = 0;
	};

	ScenarioResult RunScenario(
		const std::shared_ptr<DataModel> &World,
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset,
		std::span<const RenderMeshCreate> SourceMeshes,
		const std::string &AudioReference,
		std::size_t RigCount,
		std::size_t BoneCount,
		std::size_t TrackCount,
		std::size_t AnchorsPerRig,
		std::size_t Frames,
		std::size_t AnchorJointOffset = 0,
		bool DetailedProfiling = false,
		std::size_t PoseWorkerCount = 1
	) {
		std::vector<std::shared_ptr<MeshPart>> Rigs;
		std::vector<std::shared_ptr<Animator>> Animators;
		std::vector<std::shared_ptr<AnimationTrack>> Tracks;
		std::vector<std::shared_ptr<Attachment>> Anchors;
		std::vector<std::shared_ptr<ProximityPrompt>> Prompts;
		Rigs.reserve(RigCount);
		Animators.reserve(RigCount);
		Tracks.reserve(RigCount * TrackCount);
		Anchors.reserve(RigCount * AnchorsPerRig);
		Prompts.reserve(AnchorsPerRig == 0 ? 0 : RigCount);
		AnimationRuntime Runtime(
			Assets,
			{},
			AnimationRuntimeOptions{
				.DetailedProfiling = DetailedProfiling,
				.ParallelPoseEvaluation = PoseWorkerCount != 1,
				.PoseWorkerCount = PoseWorkerCount,
			}
		);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets, &Runtime);
		auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		auto Actions = std::dynamic_pointer_cast<ActionMap>(World->GetService("ActionMap"));
		auto Interaction = std::dynamic_pointer_cast<InteractionService>(World->GetService("InteractionService"));
		InteractionServiceTestAccess::Attach(*Interaction, World, PlayersValue, Actions, Spatial);
		std::shared_ptr<Sound> SemanticSound;
		for (std::size_t RigIndex = 0; RigIndex < RigCount; ++RigIndex) {
			auto Rig = std::make_shared<MeshPart>();
			Rig->SetName("BenchmarkRig" + std::to_string(RigIndex));
			Rig->SetMesh(Asset.Mesh);
			Rig->SetAnchored(true);
			Rig->SetParent(WorkspaceValue);
			auto AnimatorValue = std::make_shared<Animator>();
			AnimatorValue->SetParent(Rig);
			Runtime.RegisterAnimator(AnimatorValue);
			for (std::size_t TrackIndex = 0; TrackIndex < TrackCount; ++TrackIndex) {
				auto Track = AnimatorValue->CreateTrack(Asset.Animation);
				Track->SetLooped(true);
				Track->SetWeight(1.0f / static_cast<float>(TrackCount));
				Track->Play();
				Tracks.push_back(std::move(Track));
			}
			for (std::size_t AnchorIndex = 0; AnchorIndex < AnchorsPerRig; ++AnchorIndex) {
				auto Anchor = std::make_shared<Attachment>();
				const auto JointIndex = (AnchorIndex + AnchorJointOffset) % BoneCount;
				Anchor->SetJointPath(JointIndex == 0 ? "B0" : "B0/B" + std::to_string(JointIndex));
				Anchor->SetCFrame(CFrame(static_cast<float>(AnchorIndex) * 0.001f, 0.0f, 0.0f));
				Anchor->SetParent(Rig);
				Spatial->RegisterAttachment(Anchor);
				if (AnchorIndex == 0) {
					auto Prompt = std::make_shared<ProximityPrompt>();
					Prompt->SetMaxActivationDistance(64.0f);
					Prompt->SetParent(Anchor);
					Prompts.push_back(std::move(Prompt));
					if (!SemanticSound) {
						SemanticSound = std::make_shared<Sound>();
						SemanticSound->SetSoundId(AudioReference);
						SemanticSound->SetLooped(true);
						SemanticSound->SetParent(Anchor);
					}
				}
				Anchors.push_back(std::move(Anchor));
			}
			Rigs.push_back(std::move(Rig));
			Animators.push_back(std::move(AnimatorValue));
		}
		auto Audio = std::make_unique<AudioRuntime>(
			Assets, std::make_unique<BenchmarkAudioBackend>(), AudioRuntime::DiagnosticCallback{}, Spatial);
		if (SemanticSound) {
			Audio->RegisterSound(SemanticSound);
			SemanticSound->Play();
		}

		RenderPublisher Publisher;
		Publisher.SetAssetMeshChanges(std::vector<RenderMeshCreate>(SourceMeshes.begin(), SourceMeshes.end()), {});
		RenderProjection Projection;
		Runtime.Step(0.0f);
		Spatial->Step();
		InteractionServiceTestAccess::ProcessDirty(*Interaction);
		if (SemanticSound) Audio->Step(CFrame());
		Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
		Runtime.ClearChanges();
		auto Initial = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
		Require(Initial->AnimationPoseUpdates.size() == RigCount, "scenario full publication omitted an animated rig");
		(void)Projection.Apply(*Initial);
		for (std::size_t Warmup = 0; Warmup < 5; ++Warmup) {
			Runtime.Step(1.0f / 60.0f);
			Spatial->Step();
			InteractionServiceTestAccess::ProcessDirty(*Interaction);
			if (SemanticSound) Audio->Step(CFrame());
			Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
			Runtime.ClearChanges();
			(void)Projection.Apply(*Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360));
		}
		const auto MetricsBefore = Runtime.GetMetrics();
		const auto SpatialBefore = Spatial->GetMetrics();
		std::uint64_t TotalNanoseconds = 0;
		std::uint64_t RuntimeSteadyStateAllocations = 0;
		std::uint64_t PublisherNanoseconds = 0;
		std::uint64_t ProjectionNanoseconds = 0;
		std::uint64_t SemanticAllocations = 0;
		std::uint64_t SpatialAllocations = 0;
		std::uint64_t SoundAllocations = 0;
		std::uint64_t InteractionAllocations = 0;
		std::vector<std::uint64_t> SemanticSamples;
		std::vector<std::uint64_t> SoundSamples;
		std::vector<std::uint64_t> InteractionSamples;
		std::vector<std::uint64_t> TotalSemanticSamples;
		SemanticSamples.reserve(Frames);
		SoundSamples.reserve(Frames);
		InteractionSamples.reserve(Frames);
		TotalSemanticSamples.reserve(Frames);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto FrameStarted = Clock::now();
			const auto RuntimeAllocationsBefore = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
			Runtime.Step(1.0f / 60.0f);
			RuntimeSteadyStateAllocations += SemanticBenchmarkAllocations.load(std::memory_order_relaxed) -
											 RuntimeAllocationsBefore;
			const auto SemanticStarted = Clock::now();
			auto AllocationsBefore = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
			Spatial->Step();
			const auto SpatialAllocationCount =
				SemanticBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			SemanticAllocations += SpatialAllocationCount;
			SpatialAllocations += SpatialAllocationCount;
			const auto SemanticNanoseconds = Nanoseconds(SemanticStarted);
			SemanticSamples.push_back(SemanticNanoseconds);
			const auto InteractionStarted = Clock::now();
			AllocationsBefore = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
			InteractionServiceTestAccess::ProcessDirty(*Interaction);
			const auto InteractionAllocationCount =
				SemanticBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			SemanticAllocations += InteractionAllocationCount;
			InteractionAllocations += InteractionAllocationCount;
			const auto InteractionNanoseconds = Nanoseconds(InteractionStarted);
			InteractionSamples.push_back(InteractionNanoseconds);
			std::uint64_t SoundNanoseconds = 0;
			if (SemanticSound) {
				const auto SoundStarted = Clock::now();
				AllocationsBefore = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
				Audio->Step(CFrame());
				const auto SoundAllocationCount =
					SemanticBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
				SemanticAllocations += SoundAllocationCount;
				SoundAllocations += SoundAllocationCount;
				SoundNanoseconds = Nanoseconds(SoundStarted);
			}
			SoundSamples.push_back(SoundNanoseconds);
			TotalSemanticSamples.push_back(SemanticNanoseconds + InteractionNanoseconds + SoundNanoseconds);
			const auto PublisherStarted = Clock::now();
			Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
			Runtime.ClearChanges();
			auto Publication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
			PublisherNanoseconds += Nanoseconds(PublisherStarted);
			Require(Publication->AnimationPoseUpdates.size() == RigCount,
				"scenario incremental publication count differs from animated rig count");
			Require(Publication->MeshVertexUpdates.empty() && Publication->MeshCreates.empty(),
				"GPU animation scenario unexpectedly published CPU-skinned dynamic vertices");
			const auto ProjectionStarted = Clock::now();
			(void)Projection.Apply(*Publication);
			ProjectionNanoseconds += Nanoseconds(ProjectionStarted);
			TotalNanoseconds += Nanoseconds(FrameStarted);
		}
		const auto MetricsAfter = Runtime.GetMetrics();
		const auto SpatialAfter = Spatial->GetMetrics();
		const auto PerFrameMilliseconds = [Frames](std::uint64_t NanosecondTotal) {
			return static_cast<double>(NanosecondTotal) / 1'000'000.0 / static_cast<double>(Frames);
		};
		ScenarioResult Result{
			.TotalMillisecondsPerFrame = PerFrameMilliseconds(TotalNanoseconds),
			.RuntimeMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.EvaluationCpuNanoseconds - MetricsBefore.EvaluationCpuNanoseconds),
			.TrackAdvanceMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.TrackAdvanceCpuNanoseconds - MetricsBefore.TrackAdvanceCpuNanoseconds
			),
			.KeyframeLookupMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.KeyframeLookupCpuNanoseconds - MetricsBefore.KeyframeLookupCpuNanoseconds
			),
			.InterpolationMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.InterpolationCpuNanoseconds - MetricsBefore.InterpolationCpuNanoseconds
			),
			.TrackBlendingMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.TrackBlendingCpuNanoseconds - MetricsBefore.TrackBlendingCpuNanoseconds
			),
			.BindPoseFallbackMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.BindPoseFallbackCpuNanoseconds - MetricsBefore.BindPoseFallbackCpuNanoseconds
			),
			.SamplingBlendMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.SamplingAndBlendingCpuNanoseconds - MetricsBefore.SamplingAndBlendingCpuNanoseconds
			),
			.HierarchyMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.HierarchyCpuNanoseconds - MetricsBefore.HierarchyCpuNanoseconds),
			.SkinMatrixMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.SkinMatrixCpuNanoseconds - MetricsBefore.SkinMatrixCpuNanoseconds),
			.SkinningMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.SkinningCpuNanoseconds - MetricsBefore.SkinningCpuNanoseconds),
			.SemanticJointCacheMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.SemanticJointCacheCpuNanoseconds - MetricsBefore.SemanticJointCacheCpuNanoseconds
			),
			.PoseBuildMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.PosePublicationCpuNanoseconds - MetricsBefore.PosePublicationCpuNanoseconds
			),
			.PolicyMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.PolicyCpuNanoseconds - MetricsBefore.PolicyCpuNanoseconds),
			.PoseWorkerCpuMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.PoseWorkerCpuNanoseconds - MetricsBefore.PoseWorkerCpuNanoseconds),
			.PoseJobSchedulingMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.PoseJobSchedulingCpuNanoseconds - MetricsBefore.PoseJobSchedulingCpuNanoseconds
			),
			.PoseJobWaitMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.PoseJobWaitNanoseconds - MetricsBefore.PoseJobWaitNanoseconds),
			.PoseMergeMillisecondsPerFrame =
				PerFrameMilliseconds(MetricsAfter.PoseMergeCpuNanoseconds - MetricsBefore.PoseMergeCpuNanoseconds),
			.PublisherMillisecondsPerFrame = PerFrameMilliseconds(PublisherNanoseconds),
			.ProjectionMillisecondsPerFrame = PerFrameMilliseconds(ProjectionNanoseconds),
			.BindingBookkeepingMicrosecondsPerFrame = static_cast<double>(
														  SpatialAfter.BindingBookkeepingCpuNanoseconds -
														  SpatialBefore.BindingBookkeepingCpuNanoseconds
													  ) /
													  1'000.0 / static_cast<double>(Frames),
			.TransformResolutionMicrosecondsPerFrame = static_cast<double>(
														   SpatialAfter.TransformResolutionCpuNanoseconds -
														   SpatialBefore.TransformResolutionCpuNanoseconds
													   ) /
													   1'000.0 / static_cast<double>(Frames),
			.SemanticStepMicroseconds = Summarize(std::move(SemanticSamples)),
			.SoundUpdateMicroseconds = Summarize(std::move(SoundSamples)),
			.InteractionUpdateMicroseconds = Summarize(std::move(InteractionSamples)),
			.TotalSemanticMicroseconds = Summarize(std::move(TotalSemanticSamples)),
			.RuntimeBufferAllocationDelta = MetricsAfter.BufferAllocations - MetricsBefore.BufferAllocations,
			.RuntimeSteadyStateAllocationDelta = RuntimeSteadyStateAllocations,
			.SemanticAllocationDelta = SemanticAllocations,
			.SpatialAllocationDelta = SpatialAllocations,
			.SoundAllocationDelta = SoundAllocations,
			.InteractionAllocationDelta = InteractionAllocations,
			.AnchorResolutionDelta = SpatialAfter.AnchorResolutions - SpatialBefore.AnchorResolutions,
			.RigVisitDelta = SpatialAfter.RigsVisited - SpatialBefore.RigsVisited,
			.PoseEvaluationDelta = MetricsAfter.PoseEvaluations - MetricsBefore.PoseEvaluations,
			.PoseJobsScheduledDelta = MetricsAfter.PoseJobsScheduled - MetricsBefore.PoseJobsScheduled,
			.PoseJobBatchDelta = MetricsAfter.PoseJobBatches - MetricsBefore.PoseJobBatches,
			.StalePoseJobDropDelta = MetricsAfter.StalePoseJobDrops - MetricsBefore.StalePoseJobDrops,
			.PoseWorkerCapacity = MetricsAfter.PoseWorkerCapacity,
		};
		if (AnchorsPerRig == 0) {
			Require(SpatialAfter.IndexedRigs == 0 && Result.AnchorResolutionDelta == 0 && Result.RigVisitDelta == 0,
				"zero-anchor scenario performed semantic rig or Attachment work");
		} else {
			Require(SpatialAfter.IndexedSemanticAnchors == RigCount * AnchorsPerRig,
				"semantic anchor scenario registration count is incomplete");
		}
		Audio->Shutdown();
		InteractionServiceTestAccess::Shutdown(*Interaction);
		Spatial->Shutdown();
		Runtime.Shutdown();
		for (auto &AnimatorValue : Animators) AnimatorValue->Destroy();
		for (auto &Rig : Rigs) Rig->Destroy();
		Tracks.clear();
		Animators.clear();
		Prompts.clear();
		Anchors.clear();
		SemanticSound.reset();
		Rigs.clear();
		std::cout << "[Animation:Benchmark] scenario rigs=" << RigCount << " bones=" << BoneCount
				  << " tracks=" << TrackCount << " anchorsPerRig=" << AnchorsPerRig
				  << " anchorJointOffset=" << AnchorJointOffset << " totalMs=" << Result.TotalMillisecondsPerFrame
				  << " runtimeMs=" << Result.RuntimeMillisecondsPerFrame
				  << " trackAdvanceMs=" << Result.TrackAdvanceMillisecondsPerFrame
				  << " keyframeLookupMs=" << Result.KeyframeLookupMillisecondsPerFrame
				  << " interpolationMs=" << Result.InterpolationMillisecondsPerFrame
				  << " trackBlendingMs=" << Result.TrackBlendingMillisecondsPerFrame
				  << " bindFallbackMs=" << Result.BindPoseFallbackMillisecondsPerFrame
				  << " sampleBlendMs=" << Result.SamplingBlendMillisecondsPerFrame
				  << " hierarchyMs=" << Result.HierarchyMillisecondsPerFrame
				  << " skinMatrixMs=" << Result.SkinMatrixMillisecondsPerFrame
				  << " cpuSkinMs=" << Result.SkinningMillisecondsPerFrame
				  << " semanticJointCacheMs=" << Result.SemanticJointCacheMillisecondsPerFrame
				  << " poseBuildMs=" << Result.PoseBuildMillisecondsPerFrame
				  << " policyMs=" << Result.PolicyMillisecondsPerFrame
				  << " poseWorkerCpuMs=" << Result.PoseWorkerCpuMillisecondsPerFrame
				  << " poseJobSchedulingMs=" << Result.PoseJobSchedulingMillisecondsPerFrame
				  << " poseJobWaitMs=" << Result.PoseJobWaitMillisecondsPerFrame
				  << " poseMergeMs=" << Result.PoseMergeMillisecondsPerFrame
				  << " publisherMs=" << Result.PublisherMillisecondsPerFrame
				  << " projectionMs=" << Result.ProjectionMillisecondsPerFrame
				  << " bindingBookkeepingUs=" << Result.BindingBookkeepingMicrosecondsPerFrame
				  << " transformResolutionUs=" << Result.TransformResolutionMicrosecondsPerFrame
				  << " semanticStepUsP50=" << Result.SemanticStepMicroseconds.P50
				  << " semanticStepUsP95=" << Result.SemanticStepMicroseconds.P95
				  << " semanticStepUsP99=" << Result.SemanticStepMicroseconds.P99
				  << " soundUpdateUsP50=" << Result.SoundUpdateMicroseconds.P50
				  << " soundUpdateUsP95=" << Result.SoundUpdateMicroseconds.P95
				  << " soundUpdateUsP99=" << Result.SoundUpdateMicroseconds.P99
				  << " interactionUpdateUsP50=" << Result.InteractionUpdateMicroseconds.P50
				  << " interactionUpdateUsP95=" << Result.InteractionUpdateMicroseconds.P95
				  << " interactionUpdateUsP99=" << Result.InteractionUpdateMicroseconds.P99
				  << " totalSemanticUsP50=" << Result.TotalSemanticMicroseconds.P50
				  << " totalSemanticUsP95=" << Result.TotalSemanticMicroseconds.P95
				  << " totalSemanticUsP99=" << Result.TotalSemanticMicroseconds.P99
				  << " anchorResolutions=" << Result.AnchorResolutionDelta << " rigVisits=" << Result.RigVisitDelta
				  << " poseEvaluations=" << Result.PoseEvaluationDelta
				  << " poseJobsScheduled=" << Result.PoseJobsScheduledDelta
				  << " poseJobBatches=" << Result.PoseJobBatchDelta
				  << " stalePoseJobDrops=" << Result.StalePoseJobDropDelta
				  << " poseWorkerCapacity=" << Result.PoseWorkerCapacity
				  << " runtimeBufferAllocations=" << Result.RuntimeBufferAllocationDelta
				  << " runtimeSteadyStateAllocations=" << Result.RuntimeSteadyStateAllocationDelta
				  << " semanticAllocations=" << Result.SemanticAllocationDelta
				  << " spatialAllocations=" << Result.SpatialAllocationDelta
				  << " soundAllocations=" << Result.SoundAllocationDelta
				  << " interactionAllocations=" << Result.InteractionAllocationDelta << '\n';
		return Result;
	}

	enum class PolicyBenchmarkScenario : std::uint8_t {
		AllNearVisible,
		NearAndFarVisible,
		VisibleAndOffscreen,
		AllOffscreenVisual,
		AllOffscreenSemantic,
		SemanticAndFarVisual,
	};

	const char *PolicyScenarioName(PolicyBenchmarkScenario Scenario) {
		switch (Scenario) {
		case PolicyBenchmarkScenario::AllNearVisible:
			return "A-all-near-visible";
		case PolicyBenchmarkScenario::NearAndFarVisible:
			return "B-100-near-400-far";
		case PolicyBenchmarkScenario::VisibleAndOffscreen:
			return "C-50-visible-450-offscreen";
		case PolicyBenchmarkScenario::AllOffscreenVisual:
			return "D-all-offscreen-visual";
		case PolicyBenchmarkScenario::AllOffscreenSemantic:
			return "E-all-offscreen-semantic";
		case PolicyBenchmarkScenario::SemanticAndFarVisual:
			return "F-50-semantic-450-far";
		}
		return "unknown";
	}

	void RunPolicyScenario(
		const std::shared_ptr<DataModel> &World,
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset,
		std::span<const RenderMeshCreate> SourceMeshes,
		PolicyBenchmarkScenario Scenario,
		bool Quick
	) {
		constexpr std::size_t RigCount = 500;
		const auto Frames = Quick ? 60u : 240u;
		std::vector<std::shared_ptr<MeshPart>> Rigs;
		std::vector<std::shared_ptr<Animator>> Animators;
		std::vector<std::shared_ptr<AnimationTrack>> Tracks;
		std::vector<std::shared_ptr<Attachment>> Anchors;
		std::vector<ObjectId> VisibleObjects;
		Rigs.reserve(RigCount);
		Animators.reserve(RigCount);
		Tracks.reserve(RigCount);
		Anchors.reserve(RigCount);
		VisibleObjects.reserve(RigCount);
		AnimationRuntime Runtime(Assets);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets, &Runtime);
		for (std::size_t RigIndex = 0; RigIndex < RigCount; ++RigIndex) {
			bool NearVisible = false;
			bool FarVisible = false;
			bool Semantic = false;
			switch (Scenario) {
			case PolicyBenchmarkScenario::AllNearVisible:
				NearVisible = true;
				break;
			case PolicyBenchmarkScenario::NearAndFarVisible:
				NearVisible = RigIndex < 100;
				FarVisible = !NearVisible;
				break;
			case PolicyBenchmarkScenario::VisibleAndOffscreen:
				NearVisible = RigIndex < 50;
				break;
			case PolicyBenchmarkScenario::AllOffscreenVisual:
				break;
			case PolicyBenchmarkScenario::AllOffscreenSemantic:
				Semantic = true;
				break;
			case PolicyBenchmarkScenario::SemanticAndFarVisual:
				Semantic = RigIndex < 50;
				FarVisible = !Semantic;
				break;
			}
			auto Rig = std::make_shared<MeshPart>();
			Rig->SetMesh(Asset.Mesh);
			Rig->SetAnchored(true);
			Rig->SetCFrame(CFrame(NearVisible ? 24.0f : 240.0f, static_cast<float>(RigIndex % 20) * 0.01f, 0.0f));
			Rig->SetParent(WorkspaceValue);
			auto AnimatorValue = std::make_shared<Animator>();
			AnimatorValue->SetParent(Rig);
			Runtime.RegisterAnimator(AnimatorValue);
			auto Track = AnimatorValue->CreateTrack(Asset.Animation);
			Track->SetLooped(true);
			Track->Play();
			if (NearVisible || FarVisible) VisibleObjects.push_back(Rig->GetObjectId());
			if (Semantic) {
				auto Anchor = std::make_shared<Attachment>();
				Anchor->SetJointPath("B0/B1");
				Anchor->SetParent(Rig);
				Spatial->RegisterAttachment(Anchor);
				Anchors.push_back(std::move(Anchor));
			}
			Tracks.push_back(std::move(Track));
			Animators.push_back(std::move(AnimatorValue));
			Rigs.push_back(std::move(Rig));
		}
		std::ranges::sort(VisibleObjects);
		std::uint64_t VisibilityPublication = 1;
		auto StepRuntime = [&](float DeltaTime) {
			Spatial->PrepareAnimationRequirements();
			AnimationUpdateContext Context{
				.Environment = AnimationRuntimeEnvironment::Graphical,
				.ViewOrigin = glm::vec3(0.0f),
				.HasViewOrigin = true,
				.VisibilityGeneration = 1,
				.VisibilityPublication = VisibilityPublication++,
				.VisibilityComplete = true,
				.VisibleObjects = VisibleObjects,
				.SemanticRequirementsComplete = Spatial->AreAnimationRequirementsComplete(),
				.SemanticRequiredObjects = Spatial->GetAnimationRequiredRigs(),
			};
			Runtime.Step(DeltaTime, Context);
		};

		RenderPublisher Publisher;
		Publisher.SetAssetMeshChanges(std::vector<RenderMeshCreate>(SourceMeshes.begin(), SourceMeshes.end()), {});
		RenderProjection Projection;
		StepRuntime(0.0f);
		Spatial->Step();
		Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
		Runtime.ClearChanges();
		auto Initial = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
		Require(Initial->AnimationPoseUpdates.size() == RigCount, "policy scenario initial publication omitted a rig");
		(void)Projection.Apply(*Initial);
		for (std::size_t Warmup = 0; Warmup < 90; ++Warmup) {
			StepRuntime(1.0f / 60.0f);
			Spatial->Step();
			Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
			Runtime.ClearChanges();
			(void)Projection.Apply(*Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360));
		}

		const auto MetricsBefore = Runtime.GetMetrics();
		const auto SpatialBefore = Spatial->GetMetrics();
		std::uint64_t RuntimeAllocations = 0;
		std::uint64_t PosePublications = 0;
		std::uint64_t FullRateAnimatorFrames = 0;
		std::uint64_t ReducedAnimatorFrames = 0;
		std::uint64_t FrozenAnimatorFrames = 0;
		std::uint64_t SemanticAnimatorFrames = 0;
		std::vector<std::uint64_t> TotalSamples;
		std::vector<std::uint64_t> RuntimeSamples;
		TotalSamples.reserve(Frames);
		RuntimeSamples.reserve(Frames);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto TotalStarted = Clock::now();
			const auto RuntimeBefore = Runtime.GetMetrics().EvaluationCpuNanoseconds;
			const auto AllocationsBefore = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
			StepRuntime(1.0f / 60.0f);
			RuntimeAllocations += SemanticBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			const auto CurrentMetrics = Runtime.GetMetrics();
			RuntimeSamples.push_back(CurrentMetrics.EvaluationCpuNanoseconds - RuntimeBefore);
			FullRateAnimatorFrames += CurrentMetrics.FullRateAnimators;
			ReducedAnimatorFrames += CurrentMetrics.ReducedRateAnimators;
			FrozenAnimatorFrames += CurrentMetrics.FrozenVisualAnimators;
			SemanticAnimatorFrames += CurrentMetrics.SemanticRequiredAnimators;
			Spatial->Step();
			Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
			PosePublications += Runtime.GetPoseUpdates().size();
			Runtime.ClearChanges();
			auto Publication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
			Require(
				Publication->AnimationPoseUpdates.size() <= RigCount,
				"policy scenario published an unbounded pose count"
			);
			(void)Projection.Apply(*Publication);
			TotalSamples.push_back(Nanoseconds(TotalStarted));
		}
		const auto MetricsAfter = Runtime.GetMetrics();
		const auto SpatialAfter = Spatial->GetMetrics();
		const auto PerFrameMilliseconds = [Frames](std::uint64_t Value) {
			return static_cast<double>(Value) / 1'000'000.0 / static_cast<double>(Frames);
		};
		const auto TotalPercentiles = Summarize(std::move(TotalSamples));
		const auto RuntimePercentiles = Summarize(std::move(RuntimeSamples));
		const auto PoseEvaluations = MetricsAfter.PoseEvaluations - MetricsBefore.PoseEvaluations;
		Require(
			MetricsAfter.BufferAllocations == MetricsBefore.BufferAllocations,
			"policy scenario grew animation pose buffers after warmup"
		);
		std::cout
			<< "[Animation:PolicyBenchmark] scenario=" << PolicyScenarioName(Scenario) << " frames=" << Frames
			<< " fullRateAvg=" << static_cast<double>(FullRateAnimatorFrames) / Frames
			<< " reducedAvg=" << static_cast<double>(ReducedAnimatorFrames) / Frames
			<< " frozenAvg=" << static_cast<double>(FrozenAnimatorFrames) / Frames
			<< " semanticAvg=" << static_cast<double>(SemanticAnimatorFrames) / Frames
			<< " poseEvaluationsPerFrame=" << static_cast<double>(PoseEvaluations) / Frames
			<< " posePublicationsPerFrame=" << static_cast<double>(PosePublications) / Frames << " trackAdvanceMs="
			<< PerFrameMilliseconds(MetricsAfter.TrackAdvanceCpuNanoseconds - MetricsBefore.TrackAdvanceCpuNanoseconds)
			<< " policyMs="
			<< PerFrameMilliseconds(MetricsAfter.PolicyCpuNanoseconds - MetricsBefore.PolicyCpuNanoseconds)
			<< " sampleBlendMs="
			<< PerFrameMilliseconds(
				   MetricsAfter.SamplingAndBlendingCpuNanoseconds - MetricsBefore.SamplingAndBlendingCpuNanoseconds
			   )
			<< " hierarchyMs="
			<< PerFrameMilliseconds(MetricsAfter.HierarchyCpuNanoseconds - MetricsBefore.HierarchyCpuNanoseconds)
			<< " skinMatrixMs="
			<< PerFrameMilliseconds(MetricsAfter.SkinMatrixCpuNanoseconds - MetricsBefore.SkinMatrixCpuNanoseconds)
			<< " poseWorkerCpuMs="
			<< PerFrameMilliseconds(MetricsAfter.PoseWorkerCpuNanoseconds - MetricsBefore.PoseWorkerCpuNanoseconds)
			<< " poseJobSchedulingMs="
			<< PerFrameMilliseconds(
				   MetricsAfter.PoseJobSchedulingCpuNanoseconds - MetricsBefore.PoseJobSchedulingCpuNanoseconds
			   )
			<< " poseJobWaitMs="
			<< PerFrameMilliseconds(MetricsAfter.PoseJobWaitNanoseconds - MetricsBefore.PoseJobWaitNanoseconds)
			<< " poseMergeMs="
			<< PerFrameMilliseconds(MetricsAfter.PoseMergeCpuNanoseconds - MetricsBefore.PoseMergeCpuNanoseconds)
			<< " poseJobsPerFrame="
			<< static_cast<double>(MetricsAfter.PoseJobsScheduled - MetricsBefore.PoseJobsScheduled) / Frames
			<< " poseJobBatchesPerFrame="
			<< static_cast<double>(MetricsAfter.PoseJobBatches - MetricsBefore.PoseJobBatches) / Frames
			<< " stalePoseJobDrops=" << MetricsAfter.StalePoseJobDrops - MetricsBefore.StalePoseJobDrops
			<< " poseWorkerCapacity=" << MetricsAfter.PoseWorkerCapacity << " anchorUpdatesPerFrame="
			<< static_cast<double>(SpatialAfter.AnchorResolutions - SpatialBefore.AnchorResolutions) / Frames
			<< " totalUsP50=" << TotalPercentiles.P50 << " totalUsP95=" << TotalPercentiles.P95
			<< " totalUsP99=" << TotalPercentiles.P99 << " runtimeUsP50=" << RuntimePercentiles.P50
			<< " runtimeUsP95=" << RuntimePercentiles.P95 << " runtimeUsP99=" << RuntimePercentiles.P99
			<< " runtimeBufferAllocations=" << MetricsAfter.BufferAllocations - MetricsBefore.BufferAllocations
			<< " runtimeGeneralAllocations=" << RuntimeAllocations << '\n';

		Spatial->Shutdown();
		Runtime.Shutdown();
		for (auto &AnimatorValue : Animators)
			AnimatorValue->Destroy();
		for (auto &Rig : Rigs)
			Rig->Destroy();
		Tracks.clear();
		Anchors.clear();
		Animators.clear();
		Rigs.clear();
	}

	void RunJobScaling(
		const std::shared_ptr<DataModel> &World,
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset,
		std::span<const RenderMeshCreate> SourceMeshes,
		const std::string &AudioReference,
		bool Quick
	) {
		const auto Frames = Quick ? 10u : 60u;
		const auto NormalWorkerCapacity = std::max(1u, std::thread::hardware_concurrency());
		for (const auto RigCount : {10u, 100u, 500u, 1000u}) {
			auto Serial = RunScenario(
				World,
				WorkspaceValue,
				Assets,
				Asset,
				SourceMeshes,
				AudioReference,
				RigCount,
				64,
				1,
				0,
				Frames,
				0,
				false,
				1
			);
			auto Parallel = RunScenario(
				World,
				WorkspaceValue,
				Assets,
				Asset,
				SourceMeshes,
				AudioReference,
				RigCount,
				64,
				1,
				0,
				Frames,
				0,
				false,
				0
			);
			Require(
				Serial.RuntimeBufferAllocationDelta == 0 && Serial.RuntimeSteadyStateAllocationDelta == 0 &&
					Parallel.RuntimeBufferAllocationDelta == 0 && Parallel.RuntimeSteadyStateAllocationDelta == 0,
				"pose job scaling allocated runtime buffers or general storage after warmup"
			);
			Require(
				Serial.StalePoseJobDropDelta == 0 && Parallel.StalePoseJobDropDelta == 0,
				"stable pose job scaling unexpectedly dropped a result"
			);
			const auto Speedup = Serial.RuntimeMillisecondsPerFrame /
								 std::max(Parallel.RuntimeMillisecondsPerFrame, 1.0e-9);
			std::cout << "[Animation:JobBenchmark] rigs=" << RigCount << " bones=64 tracks=1 frames=" << Frames
					  << " serialWorkers=1"
					  << " normalWorkerCapacity=" << NormalWorkerCapacity
					  << " activeWorkerCapacity=" << Parallel.PoseWorkerCapacity
					  << " serialMainThreadWallMs=" << Serial.RuntimeMillisecondsPerFrame
					  << " parallelMainThreadWallMs=" << Parallel.RuntimeMillisecondsPerFrame
					  << " parallelMainThreadActiveMs="
					  << std::max(0.0, Parallel.RuntimeMillisecondsPerFrame - Parallel.PoseJobWaitMillisecondsPerFrame)
					  << " serialTotalWallMs=" << Serial.TotalMillisecondsPerFrame
					  << " parallelTotalWallMs=" << Parallel.TotalMillisecondsPerFrame << " runtimeSpeedup=" << Speedup
					  << " workerCpuMs=" << Parallel.PoseWorkerCpuMillisecondsPerFrame
					  << " schedulingMs=" << Parallel.PoseJobSchedulingMillisecondsPerFrame
					  << " waitMs=" << Parallel.PoseJobWaitMillisecondsPerFrame
					  << " mergeMs=" << Parallel.PoseMergeMillisecondsPerFrame
					  << " jobsPerFrame=" << static_cast<double>(Parallel.PoseJobsScheduledDelta) / Frames
					  << " batchesPerFrame=" << static_cast<double>(Parallel.PoseJobBatchDelta) / Frames
					  << " staleDrops=" << Parallel.StalePoseJobDropDelta << '\n';
		}
	}

	void RunCpuSkinningScaling(bool Quick) {
		for (const auto VertexCount : {1'000u, 10'000u, 50'000u, 100'000u}) {
			auto Vertices = std::make_shared<std::vector<RenderVertex>>(VertexCount);
			auto Influences = std::make_shared<std::vector<ImportedSkinInfluence>>(VertexCount);
			for (std::size_t Vertex = 0; Vertex < VertexCount; ++Vertex) {
				(*Vertices)[Vertex].Position = {static_cast<float>(Vertex % 251) * 0.01f,
					static_cast<float>((Vertex / 251) % 251) * 0.01f, 0.0f};
				(*Vertices)[Vertex].Normal = {0.0f, 1.0f, 0.0f};
				(*Vertices)[Vertex].Tangent = {1.0f, 0.0f, 0.0f, 1.0f};
				(*Influences)[Vertex].Joints = {static_cast<std::uint16_t>(Vertex % 64),
					static_cast<std::uint16_t>((Vertex + 1) % 64), 0, 0};
				(*Influences)[Vertex].Weights = {0.75f, 0.25f, 0.0f, 0.0f};
			}
			ImportedMesh Mesh;
			Mesh.Vertices = Vertices;
			Mesh.SkinInfluences = Influences;
			std::vector<RenderSkinPaletteEntry> Palette(64);
			for (std::size_t Bone = 0; Bone < Palette.size(); ++Bone)
				Palette[Bone].PositionMatrix = glm::translate(
					glm::mat4(1.0f), glm::vec3(static_cast<float>(Bone) * 0.001f, 0.0f, 0.0f));
			std::vector<RenderVertex> Output;
			RenderBounds Bounds;
			Require(AnimationRuntime::SkinMeshCpu(Mesh, Palette, Output, Bounds), "CPU skinning benchmark warmup failed");
			const auto Capacity = Output.capacity();
			const auto Iterations = Quick ? 2u : 10u;
			const auto Started = Clock::now();
			for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration)
				Require(AnimationRuntime::SkinMeshCpu(Mesh, Palette, Output, Bounds), "CPU skinning benchmark failed");
			const auto Elapsed = Nanoseconds(Started);
			const auto Milliseconds = static_cast<double>(Elapsed) / 1'000'000.0 / Iterations;
			const auto VerticesPerSecond = static_cast<double>(VertexCount) / (Milliseconds / 1000.0);
			Require(Output.capacity() == Capacity, "steady CPU skinning grew its output allocation");
			std::cout << "[Animation:Benchmark] cpuSkin vertices=" << VertexCount << " ms=" << Milliseconds
				<< " verticesPerSecond=" << static_cast<std::uint64_t>(VerticesPerSecond)
				<< " outputBufferAllocations=0\n";
		}
	}

	void RunStaticWorldRegression(
		const std::shared_ptr<DataModel> &World,
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset,
		std::span<const RenderMeshCreate> SourceMeshes,
		const std::string &AudioReference
	) {
		constexpr std::size_t StaticObjectCount = 50'000;
		constexpr std::size_t AnimatedRigCount = 500;
		constexpr std::size_t VisibleRigCount = 50;
		std::vector<std::shared_ptr<Part>> StaticObjects;
		std::vector<std::shared_ptr<MeshPart>> Rigs;
		std::vector<std::shared_ptr<Animator>> Animators;
		std::vector<std::shared_ptr<AnimationTrack>> Tracks;
		std::vector<ObjectId> VisibleObjects;
		StaticObjects.reserve(StaticObjectCount);
		Rigs.reserve(AnimatedRigCount);
		Animators.reserve(AnimatedRigCount);
		Tracks.reserve(AnimatedRigCount);
		VisibleObjects.reserve(VisibleRigCount);
		auto Runtime = std::make_unique<AnimationRuntime>(Assets);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets, Runtime.get());
		auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		auto Actions = std::dynamic_pointer_cast<ActionMap>(World->GetService("ActionMap"));
		auto Interaction = std::dynamic_pointer_cast<InteractionService>(World->GetService("InteractionService"));
		InteractionServiceTestAccess::Attach(*Interaction, World, PlayersValue, Actions, Spatial);
		for (std::size_t Index = 0; Index < StaticObjectCount; ++Index) {
			auto Object = std::make_shared<Part>();
			Object->SetAnchored(true);
			Object->SetCFrame(CFrame(glm::vec3(static_cast<float>(Index % 250), -10.0f,
				static_cast<float>(Index / 250))));
			Object->SetParent(WorkspaceValue);
			StaticObjects.push_back(std::move(Object));
		}
		std::shared_ptr<Attachment> Anchor;
		std::shared_ptr<ProximityPrompt> Prompt;
		std::shared_ptr<Sound> SoundValue;
		for (std::size_t RigIndex = 0; RigIndex < AnimatedRigCount; ++RigIndex) {
			auto Rig = std::make_shared<MeshPart>();
			Rig->SetMesh(Asset.Mesh);
			Rig->SetAnchored(true);
			Rig->SetCFrame(
				CFrame(RigIndex < VisibleRigCount ? 24.0f : 240.0f, static_cast<float>(RigIndex % 20) * 0.01f, 0.0f)
			);
			Rig->SetParent(WorkspaceValue);
			auto AnimatorValue = std::make_shared<Animator>();
			AnimatorValue->SetParent(Rig);
			Runtime->RegisterAnimator(AnimatorValue);
			auto Track = AnimatorValue->CreateTrack(Asset.Animation);
			Track->SetLooped(true);
			Track->Play();
			if (RigIndex < VisibleRigCount) VisibleObjects.push_back(Rig->GetObjectId());
			if (RigIndex + 1 == AnimatedRigCount) {
				Anchor = std::make_shared<Attachment>();
				Anchor->SetJointPath("B0/B1");
				Anchor->SetParent(Rig);
				Spatial->RegisterAttachment(Anchor);
				Prompt = std::make_shared<ProximityPrompt>();
				Prompt->SetMaxActivationDistance(64.0f);
				Prompt->SetParent(Anchor);
				SoundValue = std::make_shared<Sound>();
				SoundValue->SetSoundId(AudioReference);
				SoundValue->SetLooped(true);
				SoundValue->SetParent(Anchor);
			}
			Tracks.push_back(std::move(Track));
			Animators.push_back(std::move(AnimatorValue));
			Rigs.push_back(std::move(Rig));
		}
		std::ranges::sort(VisibleObjects);
		auto Audio = std::make_unique<AudioRuntime>(
			Assets, std::make_unique<BenchmarkAudioBackend>(), AudioRuntime::DiagnosticCallback{}, Spatial);
		Audio->RegisterSound(SoundValue);
		SoundValue->Play();
		std::uint64_t VisibilityPublication = 1;
		auto StepRuntime = [&](float DeltaTime) {
			Spatial->PrepareAnimationRequirements();
			AnimationUpdateContext Context{
				.Environment = AnimationRuntimeEnvironment::Graphical,
				.ViewOrigin = glm::vec3(0.0f),
				.HasViewOrigin = true,
				.VisibilityGeneration = 1,
				.VisibilityPublication = VisibilityPublication++,
				.VisibilityComplete = true,
				.VisibleObjects = VisibleObjects,
				.SemanticRequirementsComplete = Spatial->AreAnimationRequirementsComplete(),
				.SemanticRequiredObjects = Spatial->GetAnimationRequiredRigs(),
			};
			Runtime->Step(DeltaTime, Context);
		};
		RenderPublisher Publisher;
		Publisher.SetAssetMeshChanges(std::vector<RenderMeshCreate>(SourceMeshes.begin(), SourceMeshes.end()), {});
		RenderProjection Projection;
		StepRuntime(0.0f);
		Spatial->Step();
		InteractionServiceTestAccess::ProcessDirty(*Interaction);
		Audio->Step(CFrame());
		Publisher.SetAnimationPoseChanges(Runtime->GetPoseUpdates(), Runtime->GetPoseRemoves());
		Runtime->ClearChanges();
		auto Initial = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
		Require(
			Initial->AnimationPoseUpdates.size() == AnimatedRigCount,
			"50K world initial publication omitted an animated rig"
		);
		(void)Projection.Apply(*Initial);
		for (std::size_t Warmup = 0; Warmup < 90; ++Warmup) {
			StepRuntime(1.0f / 60.0f);
			Spatial->Step();
			InteractionServiceTestAccess::ProcessDirty(*Interaction);
			Audio->Step(CFrame());
			Publisher.SetAnimationPoseChanges(Runtime->GetPoseUpdates(), Runtime->GetPoseRemoves());
			Runtime->ClearChanges();
			(void)Projection.Apply(*Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360));
		}
		ChangeJournal::Get().Clear();
		const auto RuntimeBefore = Runtime->GetMetrics();
		const auto SpatialBefore = Spatial->GetMetrics();
		const auto AllocationsBeforeRuntime = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		StepRuntime(1.0f / 60.0f);
		const auto AllocationsAfterRuntime = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto PoseUpdateCount = Runtime->GetPoseUpdates().size();
		Spatial->Step();
		const auto AllocationsAfterSpatial = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		InteractionServiceTestAccess::ProcessDirty(*Interaction);
		const auto AllocationsAfterInteraction = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		Audio->Step(CFrame());
		const auto AllocationsAfterAudio = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto SemanticAllocationDelta = AllocationsAfterAudio - AllocationsAfterRuntime;
		std::vector<ObjectId> EvaluatedObjects;
		EvaluatedObjects.reserve(PoseUpdateCount);
		for (const auto &Update : Runtime->GetPoseUpdates())
			EvaluatedObjects.push_back(Update.Pose.Object);
		std::ranges::sort(EvaluatedObjects);
		Publisher.SetAnimationPoseChanges(Runtime->GetPoseUpdates(), Runtime->GetPoseRemoves());
		Runtime->ClearChanges();
		const auto Started = Clock::now();
		auto Incremental = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
		const auto PublishNanoseconds = Nanoseconds(Started);
		const auto RuntimeAfter = Runtime->GetMetrics();
		const auto SpatialAfter = Spatial->GetMetrics();
		const auto ExpectedPoseUpdates = VisibleRigCount + 1;
		const auto StaticUpdateCount = std::ranges::count_if(Incremental->Updates, [&](const auto &Update) {
			return !std::ranges::binary_search(EvaluatedObjects, Update.Object);
		});
		if (PoseUpdateCount != ExpectedPoseUpdates || Incremental->AnimationPoseUpdates.size() != ExpectedPoseUpdates ||
			!Incremental->MeshVertexUpdates.empty() || !Incremental->MeshCreates.empty() ||
			Incremental->Updates.size() != ExpectedPoseUpdates || StaticUpdateCount != 0 ||
			!Incremental->Creates.empty() || !Incremental->Removes.empty() || Incremental->FullResync)
			std::cerr << "[Animation:Benchmark] staticWorld publication mismatch runtimePoseUpdates=" << PoseUpdateCount
					  << " publishedPoseUpdates=" << Incremental->AnimationPoseUpdates.size()
					  << " meshVertexUpdates=" << Incremental->MeshVertexUpdates.size()
					  << " meshCreates=" << Incremental->MeshCreates.size()
					  << " objectUpdates=" << Incremental->Updates.size() << " staticUpdates=" << StaticUpdateCount
					  << " creates=" << Incremental->Creates.size() << " removes=" << Incremental->Removes.size()
					  << " fullResync=" << Incremental->FullResync << '\n';
		Require(
			PoseUpdateCount == ExpectedPoseUpdates && Incremental->AnimationPoseUpdates.size() == ExpectedPoseUpdates &&
				Incremental->MeshVertexUpdates.empty() && Incremental->MeshCreates.empty() &&
				Incremental->Updates.size() == ExpectedPoseUpdates && StaticUpdateCount == 0 &&
				Incremental->Creates.empty() && Incremental->Removes.empty() && !Incremental->FullResync,
			"50K static world policy publication touched static objects or evaluated a frozen rig"
		);
		Require(
			RuntimeAfter.PoseEvaluations - RuntimeBefore.PoseEvaluations == ExpectedPoseUpdates &&
				RuntimeAfter.FullRateAnimators == VisibleRigCount &&
				RuntimeAfter.FrozenVisualAnimators == AnimatedRigCount - VisibleRigCount - 1 &&
				RuntimeAfter.SemanticRequiredAnimators == 1,
			"50K static world policy classes did not produce exactly 50 visible plus one semantic evaluation"
		);
		Require(SpatialAfter.RegisteredAttachments == 1 &&
			SpatialAfter.IndexedSemanticAnchors == 1 && SpatialAfter.IndexedRigs == 1 &&
			SpatialAfter.AnchorResolutions - SpatialBefore.AnchorResolutions == 1 &&
			SpatialAfter.RigsVisited - SpatialBefore.RigsVisited == 1,
			"the one semantic anchor was not updated exactly once in the 50K Part world");
		Require(ChangeJournal::Get().ReadSince(0).empty(),
			"semantic pose movement emitted ChangeJournal document work in the 50K world");
		if (SemanticAllocationDelta != 0 || AllocationsAfterRuntime != AllocationsBeforeRuntime)
			std::cerr << "[Animation:Benchmark] staticWorld allocations runtime="
					  << AllocationsAfterRuntime - AllocationsBeforeRuntime
					  << " spatial=" << AllocationsAfterSpatial - AllocationsAfterRuntime
					  << " interaction=" << AllocationsAfterInteraction - AllocationsAfterSpatial
					  << " audio=" << AllocationsAfterAudio - AllocationsAfterInteraction << '\n';
		Require(
			SemanticAllocationDelta == 0 && AllocationsAfterRuntime == AllocationsBeforeRuntime &&
				RuntimeAfter.BufferAllocations == RuntimeBefore.BufferAllocations,
			"50K static world animation or semantic steady state allocated after warmup"
		);
		std::cout << "[Animation:Benchmark] staticWorld=50000 animatedRigs=500 visibleRigs=50 "
					 "semanticRigs=1 frozenVisualRigs=449 poseUpdates=51 anchorResolutions=1 staticUpdates=0 "
					 "changeJournalRecords=0 documentReconciliation=0 runtimeAllocations=0 semanticAllocations=0 "
					 "cpuDynamicVertexUpdates=0 fullResyncs=0 publisherMs="
				  << static_cast<double>(PublishNanoseconds) / 1'000'000.0 << '\n';
		Audio->Shutdown();
		InteractionServiceTestAccess::Shutdown(*Interaction);
		Spatial->Shutdown();
		Runtime->Shutdown();
		for (auto &AnimatorValue : Animators)
			AnimatorValue->Destroy();
		for (auto &Rig : Rigs)
			Rig->Destroy();
		for (auto &Object : StaticObjects) Object->Destroy();
		Tracks.clear();
		Animators.clear();
		Rigs.clear();
		StaticObjects.clear();
		ChangeJournal::Get().Clear();
	}

	void RunStaticAttachmentRegression(
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset
	) {
		constexpr std::size_t StaticAttachmentCount = 50'000;
		auto StaticOwner = std::make_shared<Part>();
		StaticOwner->SetAnchored(true);
		StaticOwner->SetParent(WorkspaceValue);
		std::vector<std::shared_ptr<Attachment>> StaticAttachments;
		StaticAttachments.reserve(StaticAttachmentCount);
		auto Runtime = std::make_unique<AnimationRuntime>(Assets);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets, Runtime.get());
		for (std::size_t Index = 0; Index < StaticAttachmentCount; ++Index) {
			auto AttachmentValue = std::make_shared<Attachment>();
			AttachmentValue->SetCFrame(CFrame(0.0f, static_cast<float>(Index % 8) * 0.01f, 0.0f));
			AttachmentValue->SetParent(StaticOwner);
			Spatial->RegisterAttachment(AttachmentValue);
			StaticAttachments.push_back(std::move(AttachmentValue));
		}
		auto Rig = std::make_shared<MeshPart>();
		Rig->SetMesh(Asset.Mesh);
		Rig->SetAnchored(true);
		Rig->SetParent(WorkspaceValue);
		auto AnimatorValue = std::make_shared<Animator>();
		AnimatorValue->SetParent(Rig);
		Runtime->RegisterAnimator(AnimatorValue);
		auto Anchor = std::make_shared<Attachment>();
		Anchor->SetJointPath("B0/B1");
		Anchor->SetParent(Rig);
		Spatial->RegisterAttachment(Anchor);
		auto Track = AnimatorValue->CreateTrack(Asset.Animation);
		Track->SetLooped(true);
		Track->Play();
		Runtime->Step(0.0f);
		Spatial->Step();
		for (std::size_t Warmup = 0; Warmup < 5; ++Warmup) {
			Runtime->Step(1.0f / 60.0f);
			Spatial->Step();
			Runtime->ClearChanges();
		}
		ChangeJournal::Get().Clear();
		const auto Before = Spatial->GetMetrics();
		Runtime->Step(1.0f / 60.0f);
		const auto AllocationsAfterRuntime = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		Spatial->Step();
		const auto AllocationsAfterSpatial = SemanticBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto AllocationDelta = AllocationsAfterSpatial - AllocationsAfterRuntime;
		const auto After = Spatial->GetMetrics();
		Require(After.RegisteredAttachments == StaticAttachmentCount + 1 &&
			After.IndexedSemanticAnchors == 1 && After.IndexedRigs == 1 &&
			After.AnchorResolutions - Before.AnchorResolutions == 1 &&
			After.RigsVisited - Before.RigsVisited == 1,
			"one animated anchor scanned or updated any of the 50K ordinary Attachments");
		if (AllocationDelta != 0)
			std::cerr << "[Animation:Benchmark] staticAttachments semantic allocations spatial="
				<< AllocationsAfterSpatial - AllocationsAfterRuntime << '\n';
		Require(ChangeJournal::Get().ReadSince(0).empty() && AllocationDelta == 0,
			"50K ordinary Attachment steady state journaled or allocated");
		std::cout << "[Animation:Benchmark] staticAttachments=50000 animatedRigs=1 semanticAnchors=1 "
			"anchorResolutions=1 staticAttachmentScans=0 changeJournalRecords=0 semanticAllocations=0\n";
		Spatial->Shutdown();
		Runtime->Shutdown();
		AnimatorValue->Destroy();
		Rig->Destroy();
		StaticOwner->Destroy();
		StaticAttachments.clear();
		ChangeJournal::Get().Clear();
	}

	void ReportMemory(std::size_t Bones, std::size_t ArtifactBytes) {
		const auto SkeletonBytes = Bones * (sizeof(ImportedSkeletonJoint) + 16);
		const auto AnimatorPoseBytes = Bones * (sizeof(glm::mat4) * 2 + sizeof(glm::vec3) * 2 + sizeof(glm::quat));
		const auto PaletteBytes = Bones * sizeof(RenderSkinPaletteEntry);
		const auto TrackBytes = sizeof(AnimationTrack) + Bones * sizeof(std::int32_t);
		const auto RendererBytes = sizeof(RenderAnimationPoseUpdate) + PaletteBytes;
		std::cout << "[Animation:Memory] bones=" << Bones << " canonicalArtifactsBytes=" << ArtifactBytes
			<< " skeletonEstimateBytes=" << SkeletonBytes << " animatorPoseEstimateBytes=" << AnimatorPoseBytes
			<< " activeTrackEstimateBytes=" << TrackBytes << " positionNormalPaletteBytes=" << PaletteBytes
			<< " rendererPoseEstimateBytes=" << RendererBytes << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	struct SdlProcessCleanup final { ~SdlProcessCleanup() { SDL_Quit(); } } SdlCleanup;
	try {
		bool Quick = false;
		bool ProfileMatrix = false;
		bool PolicyScenarios = false;
		bool JobScaling = false;
		bool StaticWorld = false;
		bool DetailedProfiling = false;
		for (int ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex) {
			const std::string_view Argument(Arguments[ArgumentIndex]);
			if (Argument == "--quick")
				Quick = true;
			else if (Argument == "--profile-matrix")
				ProfileMatrix = true;
			else if (Argument == "--policy-scenarios")
				PolicyScenarios = true;
			else if (Argument == "--job-scaling")
				JobScaling = true;
			else if (Argument == "--static-world")
				StaticWorld = true;
			else if (Argument == "--detailed-profile")
				DetailedProfiling = true;
			else
				throw std::invalid_argument("[Animation:Benchmark] unsupported argument");
		}
		BootstrapNativeRuntimeSchema();
		const auto Unique = std::to_string(Clock::now().time_since_epoch().count());
		const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-animation-benchmark-" + Unique);
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
		} CleanupValue{Root};
		std::filesystem::create_directories(Root / "assets");
		for (const auto Bones : {16u, 64u, 128u, 256u}) WriteRigFixture(Root, Bones, 999);
		WriteBytes(Root / "assets" / "semantic-tone.wav", MakeWave());
		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		auto World = std::make_shared<DataModel>();
		World->Root = Root;
		World->Filesystem = &Filesystem;
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		std::unordered_map<std::size_t, ImportedRig> Imported;
		for (const auto Bones : {16u, 64u, 128u, 256u}) Imported.emplace(Bones, ImportRig(*Assets, Mount, Bones));
		const auto AudioReference = ImportAudio(*Assets, Mount);
		auto MeshChanges = Assets->DrainMeshChanges();
		const auto SourceMeshes = MeshChanges.Creates;
		const auto Frames = Quick ? 2u : 10u;
		std::cout << std::fixed << std::setprecision(4);
		if (ProfileMatrix) {
			std::cout << "[Animation:Benchmark] matrix=prePolicy frames=" << Frames
					  << " detailedProfiling=" << DetailedProfiling << '\n';
			for (const auto Rigs : {1u, 10u, 100u, 500u, 1000u})
				for (const auto Bones : {16u, 64u, 128u, 256u})
					for (const auto Tracks : {1u, 2u, 4u}) {
						auto Result = RunScenario(
							World,
							WorkspaceValue,
							Assets,
							Imported.at(Bones),
							SourceMeshes,
							AudioReference,
							Rigs,
							Bones,
							Tracks,
							0,
							Frames,
							0,
							DetailedProfiling
						);
						Require(
							Result.RuntimeBufferAllocationDelta == 0 && Result.SemanticAllocationDelta == 0,
							"pre-policy profile matrix allocated after warmup"
						);
					}
		} else if (StaticWorld) {
			RunStaticWorldRegression(World, WorkspaceValue, Assets, Imported.at(64), SourceMeshes, AudioReference);
		} else if (JobScaling) {
			RunJobScaling(World, WorkspaceValue, Assets, Imported.at(64), SourceMeshes, AudioReference, Quick);
		} else if (PolicyScenarios) {
			for (const auto Scenario : {
					 PolicyBenchmarkScenario::AllNearVisible,
					 PolicyBenchmarkScenario::NearAndFarVisible,
					 PolicyBenchmarkScenario::VisibleAndOffscreen,
					 PolicyBenchmarkScenario::AllOffscreenVisual,
					 PolicyBenchmarkScenario::AllOffscreenSemantic,
					 PolicyBenchmarkScenario::SemanticAndFarVisual,
				 })
				RunPolicyScenario(World, WorkspaceValue, Assets, Imported.at(64), SourceMeshes, Scenario, Quick);
		} else {
			for (const auto Rigs : {1u, 10u, 100u, 500u})
				for (const auto AnchorsPerRig : {0u, 1u, 4u, 16u, 64u}) {
					auto Result = RunScenario(
						World,
						WorkspaceValue,
						Assets,
						Imported.at(64),
						SourceMeshes,
						AudioReference,
						Rigs,
						64,
						1,
						AnchorsPerRig,
						Frames
					);
					Require(
						Result.RuntimeBufferAllocationDelta == 0 && Result.SemanticAllocationDelta == 0,
						"rig/anchor-scaling scenario allocated after warmup"
					);
				}
			for (const auto Bones : {16u, 64u, 128u, 256u}) {
				auto Result = RunScenario(
					World,
					WorkspaceValue,
					Assets,
					Imported.at(Bones),
					SourceMeshes,
					AudioReference,
					10,
					Bones,
					1,
					0,
					Frames
				);
				Require(
					Result.RuntimeBufferAllocationDelta == 0 && Result.SemanticAllocationDelta == 0,
					"bone-scaling scenario allocated after warmup"
				);
			}
			auto SparseBinding = RunScenario(
				World, WorkspaceValue, Assets, Imported.at(256), SourceMeshes, AudioReference, 1, 256, 1, 1, Frames, 200
			);
			Require(
				SparseBinding.AnchorResolutionDelta == Frames && SparseBinding.RigVisitDelta == Frames &&
					SparseBinding.RuntimeBufferAllocationDelta == 0 && SparseBinding.SemanticAllocationDelta == 0,
				"one sparse joint-200 binding scaled with skeleton/world size instead of one registered anchor"
			);
			for (const auto Tracks : {1u, 2u, 4u}) {
				auto Result = RunScenario(
					World,
					WorkspaceValue,
					Assets,
					Imported.at(64),
					SourceMeshes,
					AudioReference,
					10,
					64,
					Tracks,
					0,
					Frames
				);
				Require(
					Result.RuntimeBufferAllocationDelta == 0 && Result.SemanticAllocationDelta == 0,
					"track-scaling scenario allocated after warmup"
				);
			}
			RunCpuSkinningScaling(Quick);
			RunStaticWorldRegression(World, WorkspaceValue, Assets, Imported.at(64), SourceMeshes, AudioReference);
			RunStaticAttachmentRegression(WorkspaceValue, Assets, Imported.at(64));
		}
		for (const auto Bones : {64u, 128u, 256u}) ReportMemory(Bones, Imported.at(Bones).ArtifactBytes);
		Assets.reset();
		WorkspaceValue.reset();
		World->Destroy();
		std::cout << "[Animation:Benchmark] PASS\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Animation:Benchmark] FAIL: " << Error.what() << '\n';
		return 1;
	}
}
