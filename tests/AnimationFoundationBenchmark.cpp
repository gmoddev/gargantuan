#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

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

	void AppendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
		Bytes.push_back(static_cast<std::uint8_t>(Value));
		Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
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
		double SamplingBlendMillisecondsPerFrame = 0.0;
		double HierarchyMillisecondsPerFrame = 0.0;
		double SkinMatrixMillisecondsPerFrame = 0.0;
		double SkinningMillisecondsPerFrame = 0.0;
		double PoseBuildMillisecondsPerFrame = 0.0;
		double PublisherMillisecondsPerFrame = 0.0;
		double ProjectionMillisecondsPerFrame = 0.0;
		std::uint64_t RuntimeBufferAllocationDelta = 0;
	};

	ScenarioResult RunScenario(
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset,
		std::span<const RenderMeshCreate> SourceMeshes,
		std::size_t RigCount,
		std::size_t BoneCount,
		std::size_t TrackCount,
		std::size_t Frames
	) {
		std::vector<std::shared_ptr<MeshPart>> Rigs;
		std::vector<std::shared_ptr<Animator>> Animators;
		std::vector<std::shared_ptr<AnimationTrack>> Tracks;
		Rigs.reserve(RigCount);
		Animators.reserve(RigCount);
		Tracks.reserve(RigCount * TrackCount);
		AnimationRuntime Runtime(Assets);
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
			Rigs.push_back(std::move(Rig));
			Animators.push_back(std::move(AnimatorValue));
		}

		RenderPublisher Publisher;
		Publisher.SetAssetMeshChanges(std::vector<RenderMeshCreate>(SourceMeshes.begin(), SourceMeshes.end()), {});
		RenderProjection Projection;
		Runtime.Step(0.0f);
		Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
		Runtime.ClearChanges();
		auto Initial = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
		Require(Initial->AnimationPoseUpdates.size() == RigCount, "scenario full publication omitted an animated rig");
		(void)Projection.Apply(*Initial);
		for (std::size_t Warmup = 0; Warmup < 5; ++Warmup) {
			Runtime.Step(1.0f / 60.0f);
			Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
			Runtime.ClearChanges();
			(void)Projection.Apply(*Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360));
		}
		const auto MetricsBefore = Runtime.GetMetrics();
		std::uint64_t TotalNanoseconds = 0;
		std::uint64_t PublisherNanoseconds = 0;
		std::uint64_t ProjectionNanoseconds = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto FrameStarted = Clock::now();
			Runtime.Step(1.0f / 60.0f);
			const auto PublisherStarted = Clock::now();
			Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
			Runtime.ClearChanges();
			auto Publication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
			PublisherNanoseconds += Nanoseconds(PublisherStarted);
			Require(Publication->AnimationPoseUpdates.size() == RigCount,
				"scenario incremental publication count differs from animated rig count");
			const auto ProjectionStarted = Clock::now();
			(void)Projection.Apply(*Publication);
			ProjectionNanoseconds += Nanoseconds(ProjectionStarted);
			TotalNanoseconds += Nanoseconds(FrameStarted);
		}
		const auto MetricsAfter = Runtime.GetMetrics();
		const auto PerFrameMilliseconds = [Frames](std::uint64_t NanosecondTotal) {
			return static_cast<double>(NanosecondTotal) / 1'000'000.0 / static_cast<double>(Frames);
		};
		ScenarioResult Result{
			.TotalMillisecondsPerFrame = PerFrameMilliseconds(TotalNanoseconds),
			.SamplingBlendMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.SamplingAndBlendingCpuNanoseconds - MetricsBefore.SamplingAndBlendingCpuNanoseconds),
			.HierarchyMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.HierarchyCpuNanoseconds - MetricsBefore.HierarchyCpuNanoseconds),
			.SkinMatrixMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.SkinMatrixCpuNanoseconds - MetricsBefore.SkinMatrixCpuNanoseconds),
			.SkinningMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.SkinningCpuNanoseconds - MetricsBefore.SkinningCpuNanoseconds),
			.PoseBuildMillisecondsPerFrame = PerFrameMilliseconds(
				MetricsAfter.PosePublicationCpuNanoseconds - MetricsBefore.PosePublicationCpuNanoseconds),
			.PublisherMillisecondsPerFrame = PerFrameMilliseconds(PublisherNanoseconds),
			.ProjectionMillisecondsPerFrame = PerFrameMilliseconds(ProjectionNanoseconds),
			.RuntimeBufferAllocationDelta = MetricsAfter.BufferAllocations - MetricsBefore.BufferAllocations,
		};
		Runtime.Shutdown();
		for (auto &AnimatorValue : Animators) AnimatorValue->Destroy();
		for (auto &Rig : Rigs) Rig->Destroy();
		Tracks.clear();
		Animators.clear();
		Rigs.clear();
		std::cout << "[Animation:Benchmark] scenario rigs=" << RigCount << " bones=" << BoneCount
			<< " tracks=" << TrackCount << " totalMs=" << Result.TotalMillisecondsPerFrame
			<< " sampleBlendMs=" << Result.SamplingBlendMillisecondsPerFrame
			<< " hierarchyMs=" << Result.HierarchyMillisecondsPerFrame
			<< " skinMatrixMs=" << Result.SkinMatrixMillisecondsPerFrame
			<< " cpuSkinMs=" << Result.SkinningMillisecondsPerFrame
			<< " poseBuildMs=" << Result.PoseBuildMillisecondsPerFrame
			<< " publisherMs=" << Result.PublisherMillisecondsPerFrame
			<< " projectionMs=" << Result.ProjectionMillisecondsPerFrame
			<< " runtimeBufferAllocations=" << Result.RuntimeBufferAllocationDelta << '\n';
		return Result;
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
			std::vector<glm::mat4> Palette(64, glm::mat4(1.0f));
			for (std::size_t Bone = 0; Bone < Palette.size(); ++Bone)
				Palette[Bone] = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(Bone) * 0.001f, 0.0f, 0.0f));
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
		const std::shared_ptr<Workspace> &WorkspaceValue,
		const std::shared_ptr<AssetService> &Assets,
		const ImportedRig &Asset,
		std::span<const RenderMeshCreate> SourceMeshes
	) {
		constexpr std::size_t StaticObjectCount = 50'000;
		std::vector<std::shared_ptr<Part>> StaticObjects;
		StaticObjects.reserve(StaticObjectCount);
		for (std::size_t Index = 0; Index < StaticObjectCount; ++Index) {
			auto Object = std::make_shared<Part>();
			Object->SetAnchored(true);
			Object->SetCFrame(CFrame(glm::vec3(static_cast<float>(Index % 250), -10.0f,
				static_cast<float>(Index / 250))));
			Object->SetParent(WorkspaceValue);
			StaticObjects.push_back(std::move(Object));
		}
		auto Rig = std::make_shared<MeshPart>();
		Rig->SetMesh(Asset.Mesh);
		Rig->SetAnchored(true);
		Rig->SetParent(WorkspaceValue);
		auto AnimatorValue = std::make_shared<Animator>();
		AnimatorValue->SetParent(Rig);
		auto Runtime = std::make_unique<AnimationRuntime>(Assets);
		Runtime->RegisterAnimator(AnimatorValue);
		auto Track = AnimatorValue->CreateTrack(Asset.Animation);
		Track->SetLooped(true);
		Track->Play();
		RenderPublisher Publisher;
		Publisher.SetAssetMeshChanges(std::vector<RenderMeshCreate>(SourceMeshes.begin(), SourceMeshes.end()), {});
		RenderProjection Projection;
		Runtime->Step(0.0f);
		Publisher.SetAnimationPoseChanges(Runtime->GetPoseUpdates(), Runtime->GetPoseRemoves());
		Runtime->ClearChanges();
		(void)Projection.Apply(*Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360));
		Runtime->Step(1.0f / 60.0f);
		Publisher.SetAnimationPoseChanges(Runtime->GetPoseUpdates(), Runtime->GetPoseRemoves());
		Runtime->ClearChanges();
		const auto Started = Clock::now();
		auto Incremental = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
		const auto PublishNanoseconds = Nanoseconds(Started);
		const auto StaticUpdateCount = std::ranges::count_if(Incremental->Updates, [&](const auto &Update) {
			return Update.Object != Rig->GetObjectId();
		});
		Require(Incremental->AnimationPoseUpdates.size() == 1 && Incremental->MeshVertexUpdates.size() == 1 &&
			StaticUpdateCount == 0 && Incremental->Creates.empty() && Incremental->Removes.empty(),
			"50K static world animation publication touched static objects");
		std::cout << "[Animation:Benchmark] staticWorld=50000 animatedRigs=1 poseUpdates=1 staticUpdates=0 publisherMs="
			<< static_cast<double>(PublishNanoseconds) / 1'000'000.0 << '\n';
		Runtime->Shutdown();
		AnimatorValue->Destroy();
		Rig->Destroy();
		for (auto &Object : StaticObjects) Object->Destroy();
		StaticObjects.clear();
	}

	void ReportMemory(std::size_t Bones, std::size_t ArtifactBytes) {
		const auto SkeletonBytes = Bones * (sizeof(ImportedSkeletonJoint) + 16);
		const auto AnimatorPoseBytes = Bones * (sizeof(glm::mat4) * 2 + sizeof(glm::vec3) * 2 + sizeof(glm::quat));
		const auto PaletteBytes = Bones * sizeof(glm::mat4);
		const auto TrackBytes = sizeof(AnimationTrack) + Bones * sizeof(std::int32_t);
		const auto RendererBytes = sizeof(RenderAnimationPoseUpdate) + PaletteBytes;
		std::cout << "[Animation:Memory] bones=" << Bones << " canonicalArtifactsBytes=" << ArtifactBytes
			<< " skeletonEstimateBytes=" << SkeletonBytes << " animatorPoseEstimateBytes=" << AnimatorPoseBytes
			<< " activeTrackEstimateBytes=" << TrackBytes << " paletteBytes=" << PaletteBytes
			<< " rendererPoseEstimateBytes=" << RendererBytes << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	struct SdlProcessCleanup final { ~SdlProcessCleanup() { SDL_Quit(); } } SdlCleanup;
	try {
		const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
		BootstrapNativeRuntimeSchema();
		const auto Unique = std::to_string(Clock::now().time_since_epoch().count());
		const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-animation-benchmark-" + Unique);
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
		} CleanupValue{Root};
		std::filesystem::create_directories(Root / "assets");
		for (const auto Bones : {16u, 64u, 128u, 256u}) WriteRigFixture(Root, Bones, 999);
		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		auto World = std::make_shared<DataModel>();
		World->Root = Root;
		World->Filesystem = &Filesystem;
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		std::unordered_map<std::size_t, ImportedRig> Imported;
		for (const auto Bones : {16u, 64u, 128u, 256u}) Imported.emplace(Bones, ImportRig(*Assets, Mount, Bones));
		auto MeshChanges = Assets->DrainMeshChanges();
		const auto SourceMeshes = MeshChanges.Creates;
		const auto Frames = Quick ? 2u : 10u;
		std::cout << std::fixed << std::setprecision(4);
		for (const auto Rigs : {1u, 10u, 100u, 500u})
			Require(RunScenario(WorkspaceValue, Assets, Imported.at(64), SourceMeshes,
				Rigs, 64, 1, Frames).RuntimeBufferAllocationDelta == 0,
				"rig-scaling scenario allocated after warmup");
		for (const auto Bones : {16u, 64u, 128u, 256u})
			Require(RunScenario(WorkspaceValue, Assets, Imported.at(Bones), SourceMeshes,
				10, Bones, 1, Frames).RuntimeBufferAllocationDelta == 0,
				"bone-scaling scenario allocated after warmup");
		for (const auto Tracks : {1u, 2u, 4u})
			Require(RunScenario(WorkspaceValue, Assets, Imported.at(64), SourceMeshes,
				10, 64, Tracks, Frames).RuntimeBufferAllocationDelta == 0,
				"track-scaling scenario allocated after warmup");
		RunCpuSkinningScaling(Quick);
		RunStaticWorldRegression(WorkspaceValue, Assets, Imported.at(64), SourceMeshes);
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
