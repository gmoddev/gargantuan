#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ProximityPrompt.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/InteractionService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/assets/AssetImporter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>
#include <numbers>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace gargantuan {
	struct AnimationRuntimeTestAccess {
		static void BeforePoseMerge(AnimationRuntime &Runtime, std::function<void()> Callback) {
			Runtime.SetBeforePoseMergeForTesting(std::move(Callback));
		}
	};
	struct InteractionServiceTestAccess {
		static void Step(InteractionService &Service, InteractionService::Clock::time_point Now) {
			Service.Step(Now);
		}
	};
}

namespace {
	using Json = nlohmann::ordered_json;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	template <typename Exception, typename Callback> void CheckThrows(Callback CallbackValue, const char *Message) {
		try { CallbackValue(); }
		catch (const Exception &) { return; }
		catch (...) {}
		Check(false, Message);
	}

	bool Near(float Left, float Right, float Epsilon = 1.0e-4f) {
		return std::abs(Left - Right) <= Epsilon;
	}

	bool Near(const glm::vec3 &Left, const glm::vec3 &Right, float Epsilon = 1.0e-4f) {
		return glm::length(Left - Right) <= Epsilon;
	}

	bool Near(const glm::mat4 &Left, const glm::mat4 &Right, float Epsilon = 1.0e-4f) {
		for (glm::length_t Column = 0; Column < 4; ++Column)
			for (glm::length_t Row = 0; Row < 4; ++Row)
				if (!Near(Left[Column][Row], Right[Column][Row], Epsilon)) return false;
		return true;
	}

	void AppendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
		Bytes.push_back(static_cast<std::uint8_t>(Value));
		Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
	}

	void AppendU32(std::vector<std::uint8_t> &Bytes, std::uint32_t Value) {
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.push_back(static_cast<std::uint8_t>(Value >> (Shift * 8)));
	}

	void AppendFloat(std::vector<std::uint8_t> &Bytes, float Value) {
		AppendU32(Bytes, std::bit_cast<std::uint32_t>(Value));
	}

	void StoreFloat(std::vector<std::uint8_t> &Bytes, std::size_t Offset, float Value) {
		const auto Bits = std::bit_cast<std::uint32_t>(Value);
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.at(Offset + Shift) = static_cast<std::uint8_t>(Bits >> (Shift * 8));
	}

	void AlignFour(std::vector<std::uint8_t> &Bytes, std::uint8_t Padding = 0) {
		while (Bytes.size() % 4 != 0) Bytes.push_back(Padding);
	}

	struct AnimationGltfFixture {
		std::vector<std::uint8_t> Binary;
		Json Document;
		std::size_t WeightOffset = 0;
		std::size_t InverseBindOffset = 0;
		std::size_t TimeOffset = 0;
		std::size_t ScaleOffset = 0;
	};

	std::size_t AddFloatView(
		AnimationGltfFixture &Fixture,
		Json &Views,
		std::span<const float> Values
	) {
		AlignFour(Fixture.Binary);
		const auto Offset = Fixture.Binary.size();
		for (const auto Value : Values) AppendFloat(Fixture.Binary, Value);
		Views.push_back({{"buffer", 0}, {"byteOffset", Offset}, {"byteLength", Values.size_bytes()}});
		return Views.size() - 1;
	}

	std::size_t AddByteView(
		AnimationGltfFixture &Fixture,
		Json &Views,
		std::span<const std::uint8_t> Values
	) {
		AlignFour(Fixture.Binary);
		const auto Offset = Fixture.Binary.size();
		Fixture.Binary.insert(Fixture.Binary.end(), Values.begin(), Values.end());
		Views.push_back({{"buffer", 0}, {"byteOffset", Offset}, {"byteLength", Values.size()}});
		return Views.size() - 1;
	}

	std::size_t AddU16View(
		AnimationGltfFixture &Fixture,
		Json &Views,
		std::span<const std::uint16_t> Values
	) {
		AlignFour(Fixture.Binary);
		const auto Offset = Fixture.Binary.size();
		for (const auto Value : Values) AppendU16(Fixture.Binary, Value);
		Views.push_back({{"buffer", 0}, {"byteOffset", Offset}, {"byteLength", Values.size_bytes()}});
		return Views.size() - 1;
	}

	std::size_t AddAccessor(
		Json &Accessors,
		std::size_t View,
		std::uint32_t ComponentType,
		std::size_t Count,
		std::string Type,
		bool Normalized = false
	) {
		Json Accessor{{"bufferView", View}, {"componentType", ComponentType}, {"count", Count},
			{"type", std::move(Type)}};
		if (Normalized) Accessor["normalized"] = true;
		Accessors.push_back(std::move(Accessor));
		return Accessors.size() - 1;
	}

	AnimationGltfFixture MakeAnimationGltfFixture(float WaveDistance = 1.0f, float WaveDegrees = 90.0f) {
		AnimationGltfFixture Result;
		Json Views = Json::array();
		Json Accessors = Json::array();

		const std::array Positions{
			0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f,
		};
		const std::array Normals{
			0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 1.0f,
		};
		const std::array<std::uint8_t, 12> Joints{
			0, 0, 0, 0,
			0, 1, 0, 0,
			0, 1, 2, 0,
		};
		const std::array Weights{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.25f, 0.75f, 0.0f,
		};
		const std::array<std::uint16_t, 3> Indices{0, 1, 2};

		const auto PositionView = AddFloatView(Result, Views, Positions);
		const auto NormalView = AddFloatView(Result, Views, Normals);
		const auto JointView = AddByteView(Result, Views, Joints);
		Result.WeightOffset = Result.Binary.size();
		const auto WeightView = AddFloatView(Result, Views, Weights);
		const auto IndexView = AddU16View(Result, Views, Indices);

		std::vector<float> InverseBinds;
		InverseBinds.reserve(48);
		for (const auto TranslationY : {0.0f, -1.0f, -2.0f}) {
			for (std::size_t Column = 0; Column < 4; ++Column) for (std::size_t Row = 0; Row < 4; ++Row) {
				float Value = Column == Row ? 1.0f : 0.0f;
				if (Column == 3 && Row == 1) Value = TranslationY;
				InverseBinds.push_back(Value);
			}
		}
		AlignFour(Result.Binary);
		Result.InverseBindOffset = Result.Binary.size();
		const auto InverseBindView = AddFloatView(Result, Views, InverseBinds);

		const std::array Times{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
		AlignFour(Result.Binary);
		Result.TimeOffset = Result.Binary.size();
		const auto TimeView = AddFloatView(Result, Views, Times);
		std::vector<float> WaveRotations;
		std::vector<float> CounterRotations;
		std::vector<float> LowerTranslations;
		std::vector<float> LowerScales;
		for (const auto Time : Times) {
			const auto Angle = WaveDegrees * Time * std::numbers::pi_v<float> / 180.0f;
			WaveRotations.insert(WaveRotations.end(), {0.0f, 0.0f, std::sin(Angle * 0.5f), std::cos(Angle * 0.5f)});
			CounterRotations.insert(CounterRotations.end(), {0.0f, 0.0f, -std::sin(Angle * 0.5f), std::cos(Angle * 0.5f)});
			LowerTranslations.insert(LowerTranslations.end(), {WaveDistance * Time, 1.0f, 0.0f});
			LowerScales.insert(LowerScales.end(), {1.0f + Time, 1.0f, 1.0f - Time * 0.5f});
		}
		const auto WaveRotationView = AddFloatView(Result, Views, WaveRotations);
		const auto CounterRotationView = AddFloatView(Result, Views, CounterRotations);
		const auto TranslationView = AddFloatView(Result, Views, LowerTranslations);
		AlignFour(Result.Binary);
		Result.ScaleOffset = Result.Binary.size();
		const auto ScaleView = AddFloatView(Result, Views, LowerScales);
		AlignFour(Result.Binary);

		const auto PositionAccessor = AddAccessor(Accessors, PositionView, 5126, 3, "VEC3");
		const auto NormalAccessor = AddAccessor(Accessors, NormalView, 5126, 3, "VEC3");
		const auto JointAccessor = AddAccessor(Accessors, JointView, 5121, 3, "VEC4");
		const auto WeightAccessor = AddAccessor(Accessors, WeightView, 5126, 3, "VEC4");
		const auto IndexAccessor = AddAccessor(Accessors, IndexView, 5123, 3, "SCALAR");
		const auto InverseBindAccessor = AddAccessor(Accessors, InverseBindView, 5126, 3, "MAT4");
		const auto TimeAccessor = AddAccessor(Accessors, TimeView, 5126, Times.size(), "SCALAR");
		const auto WaveRotationAccessor = AddAccessor(Accessors, WaveRotationView, 5126, Times.size(), "VEC4");
		const auto CounterRotationAccessor = AddAccessor(Accessors, CounterRotationView, 5126, Times.size(), "VEC4");
		const auto TranslationAccessor = AddAccessor(Accessors, TranslationView, 5126, Times.size(), "VEC3");
		const auto ScaleAccessor = AddAccessor(Accessors, ScaleView, 5126, Times.size(), "VEC3");

		auto &Root = Result.Document;
		Root["asset"] = {{"version", "2.0"}, {"generator", "Gargantuan Animation Foundation 1 test"}};
		Root["buffers"] = Json::array({{{"byteLength", Result.Binary.size()}}});
		Root["bufferViews"] = std::move(Views);
		Root["accessors"] = std::move(Accessors);
		Root["nodes"] = Json::array({
			{{"name", "Root"}, {"children", {1}}},
			{{"name", "Upper"}, {"translation", {0.0, 1.0, 0.0}}, {"children", {2}}},
			{{"name", "Lower"}, {"translation", {0.0, 1.0, 0.0}}},
			{{"name", "RigMesh"}, {"mesh", 0}, {"skin", 0}},
		});
		Root["skins"] = Json::array({{
			{"name", "TestRig"}, {"joints", {0, 1, 2}}, {"skeleton", 0},
			{"inverseBindMatrices", InverseBindAccessor},
		}});
		Root["meshes"] = Json::array({{
			{"name", "Animated Triangle"},
			{"primitives", Json::array({{
				{"attributes", {{"POSITION", PositionAccessor}, {"NORMAL", NormalAccessor},
					{"JOINTS_0", JointAccessor}, {"WEIGHTS_0", WeightAccessor}}},
				{"indices", IndexAccessor},
			}})},
		}});
		Root["animations"] = Json::array({
			{
				{"name", "Wave"},
				{"samplers", Json::array({
					{{"input", TimeAccessor}, {"output", WaveRotationAccessor}, {"interpolation", "LINEAR"}},
					{{"input", TimeAccessor}, {"output", TranslationAccessor}, {"interpolation", "LINEAR"}},
					{{"input", TimeAccessor}, {"output", ScaleAccessor}, {"interpolation", "STEP"}},
				})},
				{"channels", Json::array({
					{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}},
					{{"sampler", 1}, {"target", {{"node", 2}, {"path", "translation"}}}},
					{{"sampler", 2}, {"target", {{"node", 2}, {"path", "scale"}}}},
				})},
			},
			{
				{"name", "Counter"},
				{"samplers", Json::array({
					{{"input", TimeAccessor}, {"output", CounterRotationAccessor}, {"interpolation", "LINEAR"}},
				})},
				{"channels", Json::array({
					{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}},
				})},
			},
		});
		Root["scenes"] = Json::array({{{"nodes", {0, 3}}}});
		Root["scene"] = 0;
		return Result;
	}

	std::vector<std::uint8_t> MakeGlb(const AnimationGltfFixture &Fixture) {
		auto JsonText = Fixture.Document.dump();
		while (JsonText.size() % 4 != 0) JsonText.push_back(' ');
		std::vector<std::uint8_t> Result;
		Result.reserve(12 + 8 + JsonText.size() + 8 + Fixture.Binary.size());
		AppendU32(Result, 0x46546c67u);
		AppendU32(Result, 2);
		AppendU32(Result, static_cast<std::uint32_t>(12 + 8 + JsonText.size() + 8 + Fixture.Binary.size()));
		AppendU32(Result, static_cast<std::uint32_t>(JsonText.size()));
		AppendU32(Result, 0x4e4f534au);
		Result.insert(Result.end(), JsonText.begin(), JsonText.end());
		AppendU32(Result, static_cast<std::uint32_t>(Fixture.Binary.size()));
		AppendU32(Result, 0x004e4942u);
		Result.insert(Result.end(), Fixture.Binary.begin(), Fixture.Binary.end());
		return Result;
	}

	void WriteBytes(const std::filesystem::path &Path, std::span<const std::uint8_t> Bytes) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		if (!Output) throw std::runtime_error("Could not write animation test fixture");
	}

	std::vector<std::uint8_t> MakeWave(std::uint32_t FrameCount = 4'800) {
		constexpr std::uint32_t SampleRate = 48'000;
		constexpr std::uint16_t Channels = 1;
		const auto DataBytes = FrameCount * sizeof(std::int16_t);
		std::vector<std::uint8_t> Bytes;
		Bytes.reserve(44 + DataBytes);
		Bytes.insert(Bytes.end(), {'R', 'I', 'F', 'F'});
		AppendU32(Bytes, 36 + DataBytes);
		Bytes.insert(Bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
		AppendU32(Bytes, 16);
		AppendU16(Bytes, 1);
		AppendU16(Bytes, Channels);
		AppendU32(Bytes, SampleRate);
		AppendU32(Bytes, SampleRate * Channels * sizeof(std::int16_t));
		AppendU16(Bytes, Channels * sizeof(std::int16_t));
		AppendU16(Bytes, 16);
		Bytes.insert(Bytes.end(), {'d', 'a', 't', 'a'});
		AppendU32(Bytes, DataBytes);
		for (std::uint32_t Frame = 0; Frame < FrameCount; ++Frame) AppendU16(Bytes, 16'384);
		return Bytes;
	}

	class CapturingSemanticAudioBackend final : public gargantuan::IAudioBackend {
	  public:
		std::vector<float> LastSubmission;
		gargantuan::AudioBackendMetrics Metrics;
		bool Available = true;

		[[nodiscard]] bool IsAvailable() const override { return Available; }
		[[nodiscard]] std::uint32_t GetSampleRate() const override { return 48'000; }
		std::size_t GetQueuedFrames() override { return 0; }
		bool Submit(std::span<const float> Samples) override {
			LastSubmission.assign(Samples.begin(), Samples.end());
			Metrics.SubmittedFrames += Samples.size() / 2;
			return true;
		}
		void Clear() override { LastSubmission.clear(); }
		void Shutdown() override { Available = false; }
		[[nodiscard]] std::string GetDiagnostic() const override { return {}; }
		[[nodiscard]] gargantuan::AudioBackendMetrics GetMetrics() const override { return Metrics; }
	};

	std::shared_ptr<gargantuan::AssetService> GetAssets(const std::shared_ptr<gargantuan::DataModel> &World) {
		return std::dynamic_pointer_cast<gargantuan::AssetService>(World->GetService("AssetService"));
	}

	std::optional<gargantuan::AssetRecord> FindRecord(
		const gargantuan::AssetOperationResult &Import,
		gargantuan::AssetKind Kind,
		std::string_view Name = {}
	) {
		for (const auto &Record : Import.Records)
			if (Record.Kind == Kind && (Name.empty() || Record.Name == Name)) return Record;
		return std::nullopt;
	}

	void CheckPoseSample(
		gargantuan::AnimationRuntime &Runtime,
		const std::shared_ptr<gargantuan::AnimationTrack> &Track,
		gargantuan::ObjectId RigObject,
		float Fraction
	) {
		Track->Play();
		Track->SetTimePosition(Fraction);
		Runtime.Step(0.0f);
		auto Pose = Runtime.GetPose(RigObject);
		const auto Angle = Fraction * std::numbers::pi_v<float> * 0.5f;
		const auto Cosine = std::cos(Angle);
		const auto Sine = std::sin(Angle);
		const glm::vec3 ExpectedUpperAxis(Cosine, Sine, 0.0f);
		const glm::vec3 ExpectedLowerOrigin(Cosine * Fraction - Sine, 1.0f + Sine * Fraction + Cosine, 0.0f);
		const bool Valid = Pose && Pose->JointModelTransforms && Pose->SkinPalette &&
			Pose->JointModelTransforms->size() == 3 && Pose->SkinPalette->size() == 3;
		Check(Valid, "headless runtime publishes a complete three-joint pose");
		if (!Valid) return;
		const auto UpperAxis = glm::vec3((*Pose->JointModelTransforms)[1] * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
		const auto LowerOrigin = glm::vec3((*Pose->JointModelTransforms)[2][3]);
		Check(Near(UpperAxis, ExpectedUpperAxis) && Near(LowerOrigin, ExpectedLowerOrigin),
			"animation sampling preserves expected hierarchy and matrix convention");
	}

	void TestCpuSkinningGolden() {
		using namespace gargantuan;
		auto Vertices = std::make_shared<std::vector<RenderVertex>>(2);
		(*Vertices)[0].Position = {1.0f, 0.0f, 0.0f};
		(*Vertices)[0].Normal = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
		(*Vertices)[0].Tangent = {0.0f, 0.0f, 1.0f, 1.0f};
		(*Vertices)[1] = (*Vertices)[0];
		(*Vertices)[1].Position = {1.0f, 1.0f, 0.0f};
		auto Influences = std::make_shared<std::vector<ImportedSkinInfluence>>(2);
		(*Influences)[0].Joints = {0, 0, 0, 0};
		(*Influences)[0].Weights = {1.0f, 0.0f, 0.0f, 0.0f};
		(*Influences)[1].Joints = {0, 1, 0, 0};
		(*Influences)[1].Weights = {0.25f, 0.75f, 0.0f, 0.0f};
		ImportedMesh Mesh;
		Mesh.Vertices = Vertices;
		Mesh.SkinInfluences = Influences;
		const std::array<glm::mat4, 2> PositionMatrices{
			glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)) *
				glm::rotate(glm::mat4(1.0f), std::numbers::pi_v<float> * 0.5f, glm::vec3(0.0f, 0.0f, 1.0f)),
			glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f)) *
				glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f)),
		};
		std::array<RenderSkinPaletteEntry, 2> Palette;
		for (std::size_t Index = 0; Index < Palette.size(); ++Index) {
			Palette[Index].PositionMatrix = PositionMatrices[Index];
			Palette[Index].NormalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(PositionMatrices[Index]))));
		}
		std::vector<RenderVertex> Output;
		RenderBounds Bounds;
		Check(AnimationRuntime::SkinMeshCpu(Mesh, Palette, Output, Bounds),
			"CPU skinning accepts normalized one- and two-bone influences");
		if (Output.size() != 2) return;
		Check(Near(Output[0].Position, glm::vec3(2.0f, 1.0f, 0.0f)),
			"one-bone translation and rotation produce the golden position");
		const glm::vec3 JointZeroPosition(1.0f, 1.0f, 0.0f);
		const glm::vec3 JointOnePosition(2.0f, 3.0f, 0.0f);
		Check(Near(Output[1].Position, JointZeroPosition * 0.25f + JointOnePosition * 0.75f),
			"two-bone weighted skinning produces the golden position");
		const auto SourceNormal = (*Vertices)[1].Normal;
		const auto ExpectedNormal = glm::normalize(
			glm::mat3(Palette[0].NormalMatrix) * SourceNormal * 0.25f +
			glm::mat3(Palette[1].NormalMatrix) * SourceNormal * 0.75f
		);
		Check(Near(Output[1].Normal, ExpectedNormal),
			"skinned normals use the inverse-transpose under rotation and nonuniform scale");
		auto Singular = Palette;
		Singular[0].NormalMatrix = glm::mat4(0.0f);
		Singular[1].NormalMatrix = glm::mat4(0.0f);
		Check(!AnimationRuntime::SkinMeshCpu(Mesh, Singular, Output, Bounds),
			"CPU skinning fails closed on an invalid normal transform");
	}

	void TestSemanticAnimatedAnchors(
		const gargantuan::AssetProjectSnapshot &ProjectAssets,
		const std::string &MeshReference,
		const std::string &AnimationReference,
		const std::string &CounterAnimationReference,
		const std::string &AudioReference
	) {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto Assets = GetAssets(World);
		Assets->LoadProjectAssetSnapshot(ProjectAssets);
		auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		HeadlessRenderer Renderer(Vector2(640.0f, 360.0f));
		Engine Runtime(World, &Renderer, {}, EngineProviderConfiguration{.AudioEnabled = false});

		auto Rig = std::make_shared<MeshPart>();
		Rig->SetName("SemanticRig");
		Rig->SetMesh(MeshReference);
		Rig->SetAnchored(true);
		Rig->SetCFrame(CFrame(glm::vec3(3.0f, 1.0f, -2.0f),
			glm::mat3(glm::rotate(glm::mat4(1.0f), 0.37f, glm::vec3(0.0f, 0.0f, 1.0f)))));
		Rig->SetSize({2.0f, 3.0f, 0.5f});
		Rig->SetParent(Runtime.Workspace);
		auto AnimatorValue = std::make_shared<Animator>();
		AnimatorValue->SetParent(Rig);
		auto Anchor = std::make_shared<Attachment>();
		Anchor->SetName("LowerSocket");
		Anchor->SetJointPath("Root/Upper/Lower");
		Anchor->SetCFrame(CFrame(0.25f, 0.5f, -0.25f));
		Anchor->SetParent(Rig);
		auto ChildAnchor = std::make_shared<Attachment>();
		ChildAnchor->SetCFrame(CFrame(-0.1f, 0.2f, 0.3f));
		ChildAnchor->SetParent(Anchor);
		auto SpatialSound = std::make_shared<Sound>();
		SpatialSound->SetSoundId(AudioReference);
		SpatialSound->SetRollOffMinDistance(0.1f);
		SpatialSound->SetRollOffMaxDistance(100.0f);
		SpatialSound->SetParent(Anchor);
		auto Prompt = std::make_shared<ProximityPrompt>();
		Prompt->SetActionText("Inspect");
		Prompt->SetObjectText("Animated socket");
		Prompt->SetParent(Anchor);

		auto Track = AnimatorValue->CreateTrack(AnimationReference);
		Track->SetLooped(true);
		Track->Play();
		Track->SetTimePosition(0.5f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		auto Pose = Runtime.Animation->GetPose(Rig->GetObjectId());
		auto Transform = Runtime.Spatial->ResolveAttachment(Anchor);
		auto Mesh = Assets->ResolveMeshResource(MeshReference);
		const auto OwnerMatrix = glm::translate(glm::mat4(1.0f), Rig->GetCFrame().Position) *
			glm::mat4(Rig->GetCFrame().Rotation) * glm::scale(glm::mat4(1.0f), Rig->GetSize());
		const auto LocalMatrix = glm::translate(glm::mat4(1.0f), Anchor->GetCFrame().Position) *
			glm::mat4(Anchor->GetCFrame().Rotation);
		const bool PoseReady = Pose && Pose->JointModelTransforms && Pose->JointModelTransforms->size() == 3 &&
			Pose->SkinPalette && Pose->SkinPalette->size() == 3;
		Check(PoseReady && Transform && Transform->Animated,
			"semantic resolver consumes a current renderer-independent joint model pose");
		if (PoseReady && Transform) {
			const auto Expected = OwnerMatrix * (*Pose->JointModelTransforms)[2] * LocalMatrix;
			Check(
				Near(Transform->Matrix, Expected) && Near(Transform->WorldCFrame.Position, glm::vec3(Expected[3])),
				"bound Attachment composes owner CFrame, nonuniform Size, current joint model transform, and local "
				"CFrame"
			);
			Check(Near(Anchor->GetWorldCFrame().Position, Transform->WorldCFrame.Position),
				"Attachment.WorldCFrame exposes the transient semantic pose to ordinary reads");
			const auto BindModel = glm::inverse(Mesh->Value.Skeleton->Joints->at(2).InverseBindMatrix);
			const auto BindSocket = BindModel * glm::vec4(Anchor->GetCFrame().Position, 1.0f);
			const auto CpuSkinnedSocket = OwnerMatrix *
				((*Pose->SkinPalette)[2].PositionMatrix * BindSocket);
			Check(Near(glm::vec3(CpuSkinnedSocket), Transform->WorldCFrame.Position),
				"semantic socket matches the CPU reference-skinned location under asymmetric owner scale");
			Check(!Runtime.Animation->GetPoseUpdates().empty() &&
				Runtime.Animation->GetPoseUpdates().front().Pose.Mode == RenderAnimationSkinningMode::GpuPalette &&
				Runtime.Animation->GetPoseUpdates().front().Pose.Palette.Entries == Pose->SkinPalette,
				"the GPU publication and semantic resolver share one evaluated pose without GPU readback");
			auto ChildTransform = Runtime.Spatial->ResolveAttachment(ChildAnchor);
			const auto ChildLocal = glm::translate(glm::mat4(1.0f), ChildAnchor->GetCFrame().Position) *
				glm::mat4(ChildAnchor->GetCFrame().Rotation);
			Check(ChildTransform && ChildTransform->Animated && Near(ChildTransform->Matrix, Expected * ChildLocal),
				"an unbound child Attachment inherits its animated parent semantic transform");
		}
		auto BlendTrack = AnimatorValue->CreateTrack(CounterAnimationReference);
		Track->SetWeight(0.5f);
		BlendTrack->SetWeight(0.5f);
		BlendTrack->Play();
		BlendTrack->SetTimePosition(0.5f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		auto BlendedSemanticPose = Runtime.Animation->GetPose(Rig->GetObjectId());
		auto BlendedSemantic = Runtime.Spatial->ResolveAttachment(Anchor);
		Check(BlendedSemanticPose && BlendedSemanticPose->JointModelTransforms && BlendedSemantic &&
			Near(BlendedSemantic->Matrix,
				OwnerMatrix * (*BlendedSemanticPose->JointModelTransforms)[2] * LocalMatrix),
			"semantic Attachment consumes the final two-track blended pose without duplicating blending");
		BlendTrack->Stop();
		Track->SetWeight(1.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		auto PeerAnchor = std::make_shared<Attachment>();
		PeerAnchor->SetJointPath("Root/Upper/Lower");
		PeerAnchor->SetParent(Rig);
		Runtime.Spatial->Step();
		const auto BeforePeerDestroy = Runtime.Spatial->GetMetrics();
		Check(Runtime.Spatial->ResolveAttachment(PeerAnchor)->Animated,
			"multiple Attachments may bind deterministically to the same canonical joint path");
		PeerAnchor->Destroy();
		Runtime.Spatial->Step();
		Check(Runtime.Spatial->GetMetrics().IndexedSemanticAnchors + 1 ==
			BeforePeerDestroy.IndexedSemanticAnchors,
			"destroying a bound Attachment removes exactly its rig registration");
		ChildAnchor->SetParent(Rig);
		Runtime.Spatial->Step();
		Check(!Runtime.Spatial->ResolveAttachment(ChildAnchor)->Animated,
			"reparenting an unbound nested Attachment away from its bound parent restores static semantics");
		ChildAnchor->SetParent(Anchor);
		Runtime.Spatial->Step();
		Check(Runtime.Spatial->ResolveAttachment(ChildAnchor)->Animated,
			"reparenting beneath the bound Attachment rebuilds the semantic chain without stale identity");

		auto JointPathProperty = Anchor->FindProperty("JointPath");
		auto WorldCFrameProperty = Anchor->FindProperty("WorldCFrame");
		Check(JointPathProperty && JointPathProperty->PersistencePolicy == InstanceProperty::Persistence::Saved &&
			JointPathProperty->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated &&
			WorldCFrameProperty && WorldCFrameProperty->PersistencePolicy == InstanceProperty::Persistence::Transient &&
			!WorldCFrameProperty->Editable && !WorldCFrameProperty->Write,
			"schema persists only the authored JointPath and keeps WorldCFrame transient and read-only");
		for (const auto Malformed : {"/Root", "Root/", "Root//Hand", "Root\\Hand", "Root/../Hand", "Root/./Hand"}) {
			bool Rejected = false;
			try { Anchor->SetJointPath(Malformed); }
			catch (const std::invalid_argument &) { Rejected = true; }
			Check(Rejected && Anchor->GetJointPath() == "Root/Upper/Lower",
				"JointPath rejects malformed separators and traversal-like segments without changing authored state");
		}
		bool OversizedRejected = false;
		try { Anchor->SetJointPath(std::string(AssetLimits::MaximumJointPathBytes + 1, 'a')); }
		catch (const std::invalid_argument &) { OversizedRejected = true; }
		Check(OversizedRejected && Anchor->GetJointPath() == "Root/Upper/Lower",
			"JointPath enforces the canonical Mesh artifact byte bound");
		auto Persisted = std::make_shared<Attachment>();
		Persisted->SetArchivable(true);
		Persisted->SetCFrame(CFrame(1.0f, 2.0f, 3.0f));
		Persisted->SetJointPath("Root/Upper/Lower");
		std::shared_ptr<Instance> PersistedRoot = Persisted;
		const auto Serialized = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, PersistedRoot);
		std::istringstream Input(Serialized);
		auto RestoredState = InstanceSerialization::Deserialize(
			InstanceSerialization::InstanceFormat::Json, Input);
		auto Restored = std::dynamic_pointer_cast<Attachment>(RestoredState.Instance);
		Check(RestoredState.Ok && Restored && Restored->GetJointPath() == "Root/Upper/Lower" &&
			Near(Restored->GetCFrame().Position, glm::vec3(1.0f, 2.0f, 3.0f)) &&
			Serialized.find("WorldCFrame") == std::string::npos &&
			Serialized.find("PoseRevision") == std::string::npos,
			"save/reopen preserves Attachment CFrame and canonical path without transient pose state");

		std::size_t TransientSignals = 0;
		auto WorldChanged = Anchor->GetPropertyChangedSignal("WorldCFrame")->Connect(
			[&](std::monostate) { ++TransientSignals; });
		ChangeJournal::Get().Clear();
		Track->SetTimePosition(0.75f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		Check(TransientSignals > 0 && ChangeJournal::Get().ReadSince(0).empty(),
			"pose changes use a transient WorldCFrame dirty signal and emit zero ChangeJournal records");
		const auto BeforeIdle = Runtime.Spatial->GetMetrics();
		Runtime.Spatial->Step();
		const auto AfterIdle = Runtime.Spatial->GetMetrics();
		Check(AfterIdle.AnchorResolutions == BeforeIdle.AnchorResolutions,
			"an unchanged pose performs no semantic Attachment resolution work");
		ChangeJournal::Get().Clear();
		const auto BindingCursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
		Anchor->SetJointPath("Root/Upper");
		auto BindingRecords = ChangeJournal::Get().Read(BindingCursor).Records;
		Runtime.Spatial->Step();
		auto Rebound = Runtime.Spatial->ResolveAttachment(Anchor);
		auto ReboundPose = Runtime.Animation->GetPose(Rig->GetObjectId());
		Check(BindingRecords.size() == 1 &&
			std::holds_alternative<PropertyUpdatedChange>(BindingRecords.front().Payload) &&
			std::get<PropertyUpdatedChange>(BindingRecords.front().Payload).PropertyName == "JointPath",
			"authored JointPath mutation journals exactly one scoped property record");
		Check(Rebound && ReboundPose && ReboundPose->JointModelTransforms &&
			Near(Rebound->Matrix, OwnerMatrix * (*ReboundPose->JointModelTransforms)[1] * LocalMatrix),
			"authored JointPath mutation unregisters the old joint and resolves the new current joint");
		Anchor->SetJointPath("Root/Upper/Lower");
		Runtime.Spatial->Step();
		ChangeJournal::Get().Clear();
		Track->SetTimePosition(0.25f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		auto BeforePause = Runtime.Spatial->ResolveAttachment(Anchor);
		Track->Pause();
		Runtime.Animation->Step(0.25f);
		Runtime.Spatial->Step();
		auto WhilePaused = Runtime.Spatial->ResolveAttachment(Anchor);
		Track->Resume();
		Runtime.Animation->Step(0.25f);
		Runtime.Spatial->Step();
		auto AfterResume = Runtime.Spatial->ResolveAttachment(Anchor);
		Check(BeforePause && WhilePaused && AfterResume && Near(BeforePause->Matrix, WhilePaused->Matrix) &&
			!Near(WhilePaused->Matrix, AfterResume->Matrix),
			"paused animation freezes the semantic Attachment/Sound source pose and Resume continues from that pose");

		Anchor->SetJointPath("Missing/Hand");
		Runtime.Spatial->Step();
		Transform = Runtime.Spatial->ResolveAttachment(Anchor);
		const auto StaticExpected = glm::translate(glm::mat4(1.0f), Rig->GetCFrame().Position) *
			glm::mat4(Rig->GetCFrame().Rotation) * LocalMatrix;
		Check(Transform && !Transform->Animated && Near(Transform->Matrix, StaticExpected),
			"an invalid canonical joint path fails closed to ordinary static Attachment semantics");
		Anchor->SetJointPath("Root/Upper/Lower");
		Track->Stop();
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		Transform = Runtime.Spatial->ResolveAttachment(Anchor);
		const auto BindModel = glm::inverse(Mesh->Value.Skeleton->Joints->at(2).InverseBindMatrix);
		Check(Transform && Transform->Animated && Near(Transform->Matrix, OwnerMatrix * BindModel * LocalMatrix),
			"a valid skeleton with no playing track resolves the joint bind pose");

		Track->Play();
		Track->SetTimePosition(0.5f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		const auto GpuSemantic = Runtime.Spatial->ResolveAttachment(Anchor);
		auto CpuRig = std::make_shared<MeshPart>();
		CpuRig->SetMesh(MeshReference);
		CpuRig->SetAnchored(true);
		CpuRig->SetCFrame(Rig->GetCFrame());
		CpuRig->SetSize(Rig->GetSize());
		CpuRig->SetParent(Runtime.Workspace);
		auto CpuAnimator = std::make_shared<Animator>();
		CpuAnimator->SetParent(CpuRig);
		auto CpuAnchor = std::make_shared<Attachment>();
		CpuAnchor->SetJointPath("Root/Upper/Lower");
		CpuAnchor->SetCFrame(Anchor->GetCFrame());
		CpuAnchor->SetParent(CpuRig);
		auto CpuTrack = CpuAnimator->CreateTrack(AnimationReference);
		CpuTrack->SetLooped(true);
		CpuTrack->Play();
		CpuTrack->SetTimePosition(0.5f);
		AnimationRuntime CpuRuntime(Assets, {}, AnimationRuntimeOptions{.CpuSkinningFallback = true});
		CpuRuntime.RegisterAnimator(CpuAnimator);
		auto CpuSpatial = std::make_shared<SemanticSpatialResolver>(Assets, &CpuRuntime);
		CpuSpatial->RegisterAttachment(CpuAnchor);
		CpuRuntime.Step(0.0f);
		CpuSpatial->Step();
		auto CpuSemantic = CpuSpatial->ResolveAttachment(CpuAnchor);
		Check(GpuSemantic && CpuSemantic && Near(GpuSemantic->Matrix, CpuSemantic->Matrix) &&
			CpuRuntime.GetPoseUpdates().front().Pose.Mode == RenderAnimationSkinningMode::CpuFallback,
			"GPU-palette, CPU-fallback, and headless semantic anchors resolve identically");

		Rig->SetCFrame(CFrame());
		Rig->SetSize({4.0f, 1.0f, 1.0f});
		Anchor->SetCFrame(CFrame());
		Track->SetTimePosition(0.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		auto PoseZero = Runtime.Spatial->ResolveAttachment(Anchor);
		Track->SetTimePosition(1.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		auto PoseOne = Runtime.Spatial->ResolveAttachment(Anchor);
		Check(PoseZero && PoseOne && glm::distance(PoseZero->WorldCFrame.Position, PoseOne->WorldCFrame.Position) > 2.0f &&
			Near(Rig->GetCFrame().Position, glm::vec3(0.0f)),
			"joint animation moves the semantic socket locally without applying physics root motion to MeshPart");

		auto LocalPlayer = *Runtime.Players->GetLocalPlayer();
		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetParent(Runtime.Workspace);
		Character->SetPosition(PoseZero->WorldCFrame.Position);
		LocalPlayer->SetCharacter(Character);
		Prompt->SetMaxActivationDistance(1.0f);
		Prompt->SetHoldDuration(0.5f);
		Prompt->SetRequiresLineOfSight(false);
		auto Now = InteractionService::Clock::time_point(std::chrono::seconds(10));
		Track->SetTimePosition(0.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
		Check(Runtime.Interaction->GetAvailable(), "prompt enters range at the current animated socket");
		std::size_t Triggered = 0;
		auto TriggerConnection = Prompt->Triggered->Connect(
			[&](std::shared_ptr<Player>) { ++Triggered; });
		Runtime.Interaction->BeginActivation();
		Now += std::chrono::milliseconds(1);
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
		Track->SetTimePosition(1.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		Now += std::chrono::milliseconds(100);
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
		Check(!Runtime.Interaction->GetAvailable() && Runtime.Interaction->GetHoldProgress() == 0.0f && Triggered == 0,
			"animated range leave cancels an in-progress hold before final validation");
		Runtime.Interaction->EndActivation();
		Now += std::chrono::milliseconds(40);
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);

		Prompt->SetMaxActivationDistance(64.0f);
		Prompt->SetRequiresLineOfSight(true);
		auto Wall = std::make_shared<Part>();
		Wall->SetAnchored(true);
		Wall->SetCFrame(CFrame((PoseZero->WorldCFrame.Position + PoseOne->WorldCFrame.Position) * 0.5f));
		Wall->SetSize({0.25f, 8.0f, 8.0f});
		Wall->SetParent(Runtime.Workspace);
		Now += std::chrono::milliseconds(40);
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
		Check(!Runtime.Interaction->GetAvailable(),
			"line of sight raycasts toward the current animated endpoint rather than the rigid MeshPart origin");
		Track->SetTimePosition(0.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		Now += std::chrono::milliseconds(40);
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
		Check(Runtime.Interaction->GetAvailable(),
			"moving the semantic endpoint away from an occluder immediately restores prompt visibility");
		Wall->Destroy();

		auto Backend = std::make_unique<CapturingSemanticAudioBackend>();
		auto *BackendView = Backend.get();
		AudioRuntime Audio(Assets, std::move(Backend), {}, Runtime.Spatial);
		Audio.RegisterSound(SpatialSound);
		const auto ListenerPosition = (PoseZero->WorldCFrame.Position + PoseOne->WorldCFrame.Position) * 0.5f;
		auto ChannelEnergy = [](std::span<const float> Samples, std::size_t Channel) {
			double Result = 0.0;
			for (std::size_t Index = Channel; Index < Samples.size(); Index += 2) Result += std::abs(Samples[Index]);
			return Result;
		};
		SpatialSound->Play();
		Audio.Step(CFrame(ListenerPosition));
		const auto ZeroLeft = ChannelEnergy(BackendView->LastSubmission, 0);
		const auto ZeroRight = ChannelEnergy(BackendView->LastSubmission, 1);
		SpatialSound->Stop();
		Track->SetTimePosition(1.0f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		SpatialSound->Play();
		Audio.Step(CFrame(ListenerPosition));
		const auto OneLeft = ChannelEnergy(BackendView->LastSubmission, 0);
		const auto OneRight = ChannelEnergy(BackendView->LastSubmission, 1);
		Check(ZeroRight > ZeroLeft && OneLeft > OneRight && Audio.GetMetrics().SemanticSourceResolutions > 0,
			"positional Sound panning follows copied semantic source state across animated joint movement");

		Track->Stop();
		auto EndedAnchor = std::make_shared<Attachment>();
		EndedAnchor->SetJointPath("Root/Upper");
		EndedAnchor->SetParent(Rig);
		auto EndedTrack = AnimatorValue->CreateTrack(AnimationReference);
		auto EndedDestroysAnchor = EndedTrack->Ended->Once(
			[EndedAnchor](std::monostate) { EndedAnchor->Destroy(); });
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		const auto BeforeEndedDestroy = Runtime.Spatial->GetMetrics().IndexedSemanticAnchors;
		EndedTrack->Play();
		Runtime.Animation->Step(1.0f);
		Runtime.Spatial->Step();
		Check(EndedAnchor->GetDestroyed() &&
			Runtime.Spatial->GetMetrics().IndexedSemanticAnchors + 1 == BeforeEndedDestroy,
			"Ended callback may destroy a bound Attachment reentrantly without leaving a stale rig registration");

		AnimatorValue->Destroy();
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		Transform = Runtime.Spatial->ResolveAttachment(Anchor);
		const auto BindOwner = glm::scale(glm::mat4(1.0f), Rig->GetSize());
		Check(Transform && Transform->Animated && Near(Transform->Matrix, BindOwner * BindModel),
			"destroying Animator returns a still-valid joint binding to skeleton bind pose");
		auto ReplacementAnimator = std::make_shared<Animator>();
		ReplacementAnimator->SetParent(Rig);
		auto ReplacementTrack = ReplacementAnimator->CreateTrack(AnimationReference);
		ReplacementTrack->Play();
		ReplacementTrack->SetTimePosition(0.5f);
		Runtime.Animation->Step(0.0f);
		Runtime.Spatial->Step();
		Check(Runtime.Spatial->ResolveAttachment(Anchor)->Animated,
			"Animator replacement reuses the authored skeleton binding without stale runtime identity");

		Audio.Shutdown();
		CpuSpatial->Shutdown();
		CpuRuntime.Shutdown();
		Runtime.Destroy();
		if (!World->GetDestroyed()) World->Destroy();
		ChangeJournal::Get().Clear();
	}

	struct AnimationPolicyFixture {
		std::shared_ptr<gargantuan::MeshPart> Rig;
		std::shared_ptr<gargantuan::Animator> AnimatorValue;
		std::shared_ptr<gargantuan::AnimationTrack> Track;
		std::unique_ptr<gargantuan::AnimationRuntime> Runtime;
		std::vector<gargantuan::ObjectId> VisibleObjects;
		std::vector<gargantuan::ObjectId> SemanticObjects;
		std::uint64_t Publication = 1;

		AnimationPolicyFixture(
			const std::shared_ptr<gargantuan::AssetService> &Assets,
			const std::shared_ptr<gargantuan::Workspace> &WorkspaceValue,
			const std::string &MeshReference,
			const std::string &AnimationReference,
			float Distance
		) {
			Rig = std::make_shared<gargantuan::MeshPart>();
			Rig->SetMesh(MeshReference);
			Rig->SetCFrame(gargantuan::CFrame(Distance, 0.0f, 0.0f));
			Rig->SetParent(WorkspaceValue);
			AnimatorValue = std::make_shared<gargantuan::Animator>();
			AnimatorValue->SetParent(Rig);
			Runtime = std::make_unique<gargantuan::AnimationRuntime>(Assets);
			Runtime->RegisterAnimator(AnimatorValue);
			Track = AnimatorValue->CreateTrack(AnimationReference);
			Track->SetLooped(true);
			Track->Play();
			VisibleObjects.reserve(1);
			SemanticObjects.reserve(1);
		}

		~AnimationPolicyFixture() {
			if (Runtime) Runtime->Shutdown();
			if (Rig && !Rig->GetDestroyed()) Rig->Destroy();
		}

		void SetDistance(float Distance) {
			Rig->SetCFrame(gargantuan::CFrame(Distance, 0.0f, 0.0f));
		}

		void Step(
			float DeltaTime,
			bool Visible,
			bool Semantic,
			gargantuan::AnimationRuntimeEnvironment Environment = gargantuan::AnimationRuntimeEnvironment::Graphical
		) {
			VisibleObjects.clear();
			SemanticObjects.clear();
			if (Visible) VisibleObjects.push_back(Rig->GetObjectId());
			if (Semantic) SemanticObjects.push_back(Rig->GetObjectId());
			gargantuan::AnimationUpdateContext Context;
			Context.Environment = Environment;
			Context.ViewOrigin = glm::vec3(0.0f);
			Context.HasViewOrigin = Environment == gargantuan::AnimationRuntimeEnvironment::Graphical;
			if (Environment == gargantuan::AnimationRuntimeEnvironment::Graphical) {
				Context.VisibilityGeneration = 1;
				Context.VisibilityPublication = Publication++;
				Context.VisibilityComplete = true;
				Context.VisibleObjects = VisibleObjects;
			}
			Context.SemanticRequiredObjects = SemanticObjects;
			Runtime->Step(DeltaTime, Context);
		}
	};

	void TestAnimationUpdatePolicy(
		const std::shared_ptr<gargantuan::AssetService> &Assets,
		const std::shared_ptr<gargantuan::Workspace> &WorkspaceValue,
		const std::string &MeshReference,
		const std::string &AnimationReference
	) {
		using namespace gargantuan;
		AnimationPolicyFixture NearFixture(Assets, WorkspaceValue, MeshReference, AnimationReference, 10.0f);
		NearFixture.Step(0.0f, true, false);
		const auto NearInitialEvaluations = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		for (std::size_t Frame = 0; Frame < 12; ++Frame)
			NearFixture.Step(1.0f / 60.0f, true, false);
		Check(
			NearFixture.Runtime->GetMetrics().PoseEvaluations == NearInitialEvaluations + 12 &&
				NearFixture.Runtime->GetMetrics().FullRateAnimators == 1,
			"near visible rigs evaluate at full rate"
		);

		const auto BeforeGrace = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		for (std::size_t Frame = 0; Frame < 12; ++Frame)
			NearFixture.Step(1.0f / 60.0f, false, false);
		Check(
			NearFixture.Runtime->GetMetrics().PoseEvaluations == BeforeGrace + 12,
			"recently visible rigs remain current throughout the transition grace window"
		);
		for (std::size_t Frame = 0; Frame < 6; ++Frame)
			NearFixture.Step(1.0f / 60.0f, false, false);
		const auto FrozenEvaluations = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		const auto FrozenTime = NearFixture.Track->GetTimePosition();
		const auto FrozenAllocations = NearFixture.Runtime->GetMetrics().BufferAllocations;
		for (std::size_t Frame = 0; Frame < 30; ++Frame)
			NearFixture.Step(1.0f / 60.0f, false, false);
		Check(
			NearFixture.Runtime->GetMetrics().PoseEvaluations == FrozenEvaluations &&
				NearFixture.Runtime->GetMetrics().FrozenVisualAnimators == 1 &&
				!Near(NearFixture.Track->GetTimePosition(), FrozenTime) &&
				NearFixture.Runtime->GetMetrics().BufferAllocations == FrozenAllocations,
			"offscreen visual-only rigs freeze pose work without freezing logical time or allocating"
		);
		const auto RevisionBeforeReturn = NearFixture.Runtime->GetPose(NearFixture.Rig->GetObjectId())->PoseRevision;
		NearFixture.Step(0.0f, true, false);
		auto ReturnedPose = NearFixture.Runtime->GetPose(NearFixture.Rig->GetObjectId());
		Check(
			ReturnedPose && ReturnedPose->PoseRevision > RevisionBeforeReturn &&
				NearFixture.Runtime->GetMetrics().PoseEvaluations == FrozenEvaluations + 1,
			"visibility re-entry immediately publishes the current logical pose without replaying skipped frames"
		);

		for (std::size_t Frame = 0; Frame < 20; ++Frame)
			NearFixture.Step(1.0f / 60.0f, false, false);
		const auto BeforeSeek = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		NearFixture.Track->SetTimePosition(0.75f);
		NearFixture.Step(0.0f, false, false);
		Check(
			NearFixture.Runtime->GetMetrics().PoseEvaluations == BeforeSeek + 1 &&
				NearFixture.Runtime->GetMetrics().ImmediatePoseRefreshes > 0,
			"explicit track control changes refresh a graphically frozen pose immediately"
		);
		const auto BeforeSemantic = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		NearFixture.Step(1.0f / 60.0f, false, true);
		NearFixture.Step(1.0f / 60.0f, false, true);
		Check(
			NearFixture.Runtime->GetMetrics().PoseEvaluations == BeforeSemantic + 2 &&
				NearFixture.Runtime->GetMetrics().SemanticRequiredAnimators == 1,
			"semantic-required rigs override visibility and update every logical step"
		);
		const auto BeforeSemanticRemoval = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		NearFixture.Step(1.0f / 60.0f, false, false);
		Check(
			NearFixture.Runtime->GetMetrics().PoseEvaluations == BeforeSemanticRemoval &&
				NearFixture.Runtime->GetMetrics().FrozenVisualAnimators == 1,
			"removing the final semantic requirement returns an offscreen rig to visual freeze"
		);

		std::size_t EndedCount = 0;
		auto EndedConnection = NearFixture.Track->Ended->Connect([&](std::monostate) { ++EndedCount; });
		NearFixture.Track->SetLooped(false);
		NearFixture.Track->Play();
		NearFixture.Track->SetTimePosition(0.0f);
		NearFixture.Step(0.0f, false, false);
		const auto BeforeFrozenEnd = NearFixture.Runtime->GetMetrics().PoseEvaluations;
		NearFixture.Step(1.25f, false, false);
		Check(
			EndedCount == 1 && NearFixture.Track->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped &&
				NearFixture.Track->HoldsNaturalEndPose() &&
				NearFixture.Runtime->GetMetrics().PoseEvaluations == BeforeFrozenEnd,
			"a non-looping offscreen track completes and fires Ended exactly once without forcing pose work"
		);
		NearFixture.Step(0.0f, true, false);
		const auto FinalPose = NearFixture.Runtime->GetPose(NearFixture.Rig->GetObjectId());
		Check(
			EndedCount == 1 && FinalPose && Near(NearFixture.Track->GetTimePosition(), 1.0f),
			"a completed offscreen track publishes its held final pose on visibility return"
		);

		AnimationPolicyFixture ControlFixture(Assets, WorkspaceValue, MeshReference, AnimationReference, 10.0f);
		ControlFixture.Step(0.0f, true, false);
		for (std::size_t Frame = 0; Frame < 20; ++Frame)
			ControlFixture.Step(1.0f / 60.0f, false, false);
		const auto BeforeSpeed = ControlFixture.Runtime->GetMetrics().PoseEvaluations;
		ControlFixture.Track->SetSpeed(2.0f);
		ControlFixture.Step(0.0f, false, false);
		const auto BeforeWeight = ControlFixture.Runtime->GetMetrics().PoseEvaluations;
		ControlFixture.Track->SetWeight(0.5f);
		ControlFixture.Step(0.0f, false, false);
		const auto BeforePause = ControlFixture.Runtime->GetMetrics().PoseEvaluations;
		ControlFixture.Track->Pause();
		ControlFixture.Step(0.0f, false, false);
		Check(
			BeforeWeight == BeforeSpeed + 1 && BeforePause == BeforeWeight + 1 &&
				ControlFixture.Runtime->GetMetrics().PoseEvaluations == BeforePause + 1,
			"speed, weight, and pause controls each force one current pose while graphically frozen"
		);
		const auto PausedEvaluations = ControlFixture.Runtime->GetMetrics().PoseEvaluations;
		const auto PausedTime = ControlFixture.Track->GetTimePosition();
		for (std::size_t Frame = 0; Frame < 30; ++Frame)
			ControlFixture.Step(1.0f / 60.0f, false, false);
		Check(
			ControlFixture.Runtime->GetMetrics().PoseEvaluations == PausedEvaluations &&
				Near(ControlFixture.Track->GetTimePosition(), PausedTime),
			"a paused frozen track performs no repeated pose solve or logical advancement"
		);
		ControlFixture.Track->Resume();
		ControlFixture.Step(0.0f, false, false);
		Check(
			ControlFixture.Runtime->GetMetrics().PoseEvaluations == PausedEvaluations + 1,
			"resume refreshes one current pose before returning to visual freeze"
		);
		ControlFixture.Track->Stop();
		ControlFixture.Step(0.0f, false, false);
		const auto StoppedEvaluations = ControlFixture.Runtime->GetMetrics().PoseEvaluations;
		for (std::size_t Frame = 0; Frame < 10; ++Frame)
			ControlFixture.Step(1.0f / 60.0f, false, false);
		Check(
			!ControlFixture.Runtime->GetPose(ControlFixture.Rig->GetObjectId()) &&
				ControlFixture.Runtime->GetMetrics().PoseEvaluations == StoppedEvaluations,
			"a stopped Animator retires its pose and no longer enters expensive scheduling"
		);
		ControlFixture.Track->Play();
		ControlFixture.Step(0.0f, false, false);
		Check(
			ControlFixture.Runtime->GetPose(ControlFixture.Rig->GetObjectId()).has_value() &&
				ControlFixture.Runtime->GetMetrics().PoseEvaluations == StoppedEvaluations + 1,
			"Play publishes one initial current pose even while the visual policy remains frozen"
		);

		AnimationPolicyFixture HeadlessFixture(Assets, WorkspaceValue, MeshReference, AnimationReference, 10.0f);
		HeadlessFixture.Step(0.25f, false, false, AnimationRuntimeEnvironment::Headless);
		Check(
			HeadlessFixture.Runtime->GetMetrics().PoseEvaluations == 0 &&
				HeadlessFixture.Runtime->GetMetrics().HeadlessVisualPoseSkips > 0 &&
				!HeadlessFixture.Runtime->GetPose(HeadlessFixture.Rig->GetObjectId()) &&
				Near(HeadlessFixture.Track->GetTimePosition(), 0.25f),
			"headless visual-only rigs advance logical state while producing no visual pose work"
		);
		HeadlessFixture.Runtime->RequestPoseRefresh(HeadlessFixture.Rig->GetObjectId());
		HeadlessFixture.Step(0.0f, false, false, AnimationRuntimeEnvironment::Headless);
		Check(
			HeadlessFixture.Runtime->GetMetrics().PoseEvaluations == 1,
			"an explicit headless pose request performs one bounded refresh"
		);
		HeadlessFixture.Step(1.0f / 60.0f, false, true, AnimationRuntimeEnvironment::Headless);
		Check(
			HeadlessFixture.Runtime->GetMetrics().PoseEvaluations == 2 &&
				HeadlessFixture.Runtime->GetMetrics().SemanticRequiredAnimators == 1,
			"headless semantic rigs continue evaluating without renderer feedback"
		);

		for (const auto FrameRate : {30.0f, 60.0f, 144.0f}) {
			AnimationPolicyFixture ReducedFixture(Assets, WorkspaceValue, MeshReference, AnimationReference, 100.0f);
			ReducedFixture.Step(0.0f, true, false);
			const auto Initial = ReducedFixture.Runtime->GetMetrics().PoseEvaluations;
			const auto FrameCount = static_cast<std::size_t>(std::lround(FrameRate * 2.0f));
			for (std::size_t Frame = 0; Frame < FrameCount; ++Frame)
				ReducedFixture.Step(1.0f / FrameRate, true, false);
			const auto Evaluations = ReducedFixture.Runtime->GetMetrics().PoseEvaluations - Initial;
			Check(
				Evaluations >= 58 && Evaluations <= 62 &&
					ReducedFixture.Runtime->GetMetrics().ReducedRateAnimators == 1,
				"mid-distance cadence remains approximately 30 Hz across render frame rates"
			);
		}

		AnimationPolicyFixture VariableFixture(Assets, WorkspaceValue, MeshReference, AnimationReference, 100.0f);
		VariableFixture.Step(0.0f, true, false);
		const auto VariableInitial = VariableFixture.Runtime->GetMetrics().PoseEvaluations;
		constexpr std::array VariableDeltas{1.0f / 144.0f, 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 120.0f};
		float Elapsed = 0.0f;
		std::size_t VariableFrame = 0;
		while (Elapsed < 2.0f) {
			const auto Delta = VariableDeltas[VariableFrame++ % VariableDeltas.size()];
			Elapsed += Delta;
			VariableFixture.Step(Delta, true, false);
		}
		const auto VariableEvaluations = VariableFixture.Runtime->GetMetrics().PoseEvaluations - VariableInitial;
		Check(
			VariableEvaluations >= 58 && VariableEvaluations <= 62,
			"mid-distance cadence follows elapsed time under variable frame delivery"
		);

		VariableFixture.SetDistance(161.0f);
		VariableFixture.Step(1.0f / 60.0f, true, false);
		Check(
			VariableFixture.Runtime->GetMetrics().ReducedRateAnimators == 1,
			"crossing the mid exit threshold enters the far reduced-rate band"
		);
		VariableFixture.SetDistance(150.0f);
		VariableFixture.Step(1.0f / 60.0f, true, false);
		Check(
			VariableFixture.Runtime->GetMetrics().ReducedRateAnimators == 1,
			"distance hysteresis retains the far band above its enter threshold"
		);
		VariableFixture.SetDistance(140.0f);
		VariableFixture.Step(1.0f / 60.0f, true, false);
		Check(
			VariableFixture.Runtime->GetMetrics().ReducedRateAnimators == 1,
			"distance hysteresis returns to the mid band only after its enter threshold"
		);

		AnimationUpdateContext StaleContext;
		StaleContext.Environment = AnimationRuntimeEnvironment::Graphical;
		StaleContext.ViewOrigin = glm::vec3(0.0f);
		StaleContext.HasViewOrigin = true;
		StaleContext.VisibilityComplete = true;
		StaleContext.VisibilityGeneration = 1;
		StaleContext.VisibilityPublication = 1;
		const auto BeforeStale = VariableFixture.Runtime->GetMetrics().PoseEvaluations;
		VariableFixture.Runtime->Step(1.0f / 60.0f, StaleContext);
		Check(
			VariableFixture.Runtime->GetMetrics().VisibilityFeedbackDrops > 0 &&
				VariableFixture.Runtime->GetMetrics().FullRateAnimators == 1 &&
				VariableFixture.Runtime->GetMetrics().PoseEvaluations == BeforeStale + 1,
			"stale renderer feedback is rejected and falls back conservatively to full-rate evaluation"
		);
		const auto DropsBeforeRestart = VariableFixture.Runtime->GetMetrics().VisibilityFeedbackDrops;
		const auto EvaluationsBeforeRestart = VariableFixture.Runtime->GetMetrics().PoseEvaluations;
		const std::array RestartVisible{VariableFixture.Rig->GetObjectId()};
		AnimationUpdateContext RestartContext = StaleContext;
		RestartContext.VisibilityGeneration = 2;
		RestartContext.VisibilityPublication = 1;
		RestartContext.VisibleObjects = RestartVisible;
		VariableFixture.Runtime->Step(0.0f, RestartContext);
		Check(
			VariableFixture.Runtime->GetMetrics().VisibilityFeedbackDrops == DropsBeforeRestart &&
				VariableFixture.Runtime->GetMetrics().ReducedRateAnimators == 1 &&
				VariableFixture.Runtime->GetMetrics().PoseEvaluations == EvaluationsBeforeRestart,
			"a new renderer generation accepts a restarted publication sequence without forcing an unchanged pose"
		);

		auto SemanticAnchor = std::make_shared<Attachment>();
		SemanticAnchor->SetJointPath("Root/Upper");
		SemanticAnchor->SetParent(HeadlessFixture.Rig);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets, HeadlessFixture.Runtime.get());
		Spatial->RegisterAttachment(SemanticAnchor);
		Spatial->PrepareAnimationRequirements();
		Check(
			Spatial->AreAnimationRequirementsComplete() &&
				std::ranges::binary_search(Spatial->GetAnimationRequiredRigs(), HeadlessFixture.Rig->GetObjectId()),
			"semantic resolver publishes a complete sorted rig requirement before animation evaluation"
		);
		SemanticAnchor->SetJointPath("");
		Spatial->PrepareAnimationRequirements();
		Check(
			!std::ranges::binary_search(Spatial->GetAnimationRequiredRigs(), HeadlessFixture.Rig->GetObjectId()),
			"removing the final joint binding removes the rig from the next animation requirement summary"
		);
		Spatial->Shutdown();
	}

	void TestAnimationPoseJobs(
		const std::shared_ptr<gargantuan::AssetService> &Assets,
		const std::shared_ptr<gargantuan::Workspace> &WorkspaceValue,
		const std::string &MeshReference,
		const std::string &AnimationReference,
		const std::filesystem::path &Root,
		gargantuan::SourceMount &Mount,
		const AnimationGltfFixture &CompatibleFixture,
		const AnimationGltfFixture &IncompatibleFixture
	) {
		using namespace gargantuan;
		constexpr std::size_t RigCount = 64;
		std::vector<std::shared_ptr<MeshPart>> Rigs;
		std::vector<std::shared_ptr<Animator>> Animators;
		std::vector<std::shared_ptr<AnimationTrack>> Tracks;
		Rigs.reserve(RigCount);
		Animators.reserve(RigCount);
		Tracks.reserve(RigCount);
		AnimationRuntime Runtime(Assets, {}, AnimationRuntimeOptions{.PoseWorkerCount = 4});
		for (std::size_t Index = 0; Index < RigCount; ++Index) {
			auto Rig = std::make_shared<MeshPart>();
			Rig->SetMesh(MeshReference);
			Rig->SetParent(WorkspaceValue);
			auto AnimatorValue = std::make_shared<Animator>();
			AnimatorValue->SetParent(Rig);
			Runtime.RegisterAnimator(AnimatorValue);
			auto Track = AnimatorValue->CreateTrack(AnimationReference);
			Track->SetLooped(true);
			Track->Play();
			Rigs.push_back(std::move(Rig));
			Animators.push_back(std::move(AnimatorValue));
			Tracks.push_back(std::move(Track));
		}
		Runtime.Step(0.0f);
		auto InitialMetrics = Runtime.GetMetrics();
		Check(
			InitialMetrics.PoseEvaluations == RigCount && InitialMetrics.PoseJobsScheduled == RigCount &&
				InitialMetrics.PoseJobBatches > 0 && InitialMetrics.PoseJobBatches <= 4 &&
				InitialMetrics.PoseWorkerCapacity == 4 && InitialMetrics.ActivePoseJobs == 0 &&
				Runtime.GetPoseUpdates().size() == RigCount,
			"bounded pose jobs evaluate one independent rig each and retire before deterministic Main merge"
		);
		std::vector<std::pair<ObjectId, std::uint64_t>> MergeOrder;
		MergeOrder.reserve(RigCount);
		for (std::size_t Index = 0; Index < RigCount; ++Index)
			MergeOrder.emplace_back(
				Animators[Index]->GetObjectId(), Runtime.GetPose(Rigs[Index]->GetObjectId())->PoseRevision
			);
		std::ranges::sort(MergeOrder, {}, &std::pair<ObjectId, std::uint64_t>::first);
		for (std::size_t Index = 1; Index < MergeOrder.size(); ++Index)
			Check(
				MergeOrder[Index - 1].second < MergeOrder[Index].second,
				"pose job merge follows stable Animator ObjectId order instead of worker completion order"
			);

		const auto TrackPoseBefore = Runtime.GetPose(Rigs[0]->GetObjectId())->PoseRevision;
		const auto StaleBeforeTrackChange = Runtime.GetMetrics().StalePoseJobDrops;
		AnimationRuntimeTestAccess::BeforePoseMerge(Runtime, [&] { Tracks[0]->SetTimePosition(0.75f); });
		Runtime.Step(1.0f / 60.0f);
		auto TrackPoseAfterStale = Runtime.GetPose(Rigs[0]->GetObjectId());
		Check(
			Runtime.GetMetrics().StalePoseJobDrops == StaleBeforeTrackChange + 1 && TrackPoseAfterStale &&
				TrackPoseAfterStale->PoseRevision == TrackPoseBefore,
			"a track control revision changed before merge rejects exactly its stale worker result"
		);
		Runtime.Step(0.0f);
		auto TrackPoseAfterRefresh = Runtime.GetPose(Rigs[0]->GetObjectId());
		Check(
			TrackPoseAfterRefresh && TrackPoseAfterRefresh->PoseRevision > TrackPoseBefore,
			"the stale track result is replaced by one current pose without replaying old work"
		);

		const auto StaleBeforeDestroy = Runtime.GetMetrics().StalePoseJobDrops;
		AnimationRuntimeTestAccess::BeforePoseMerge(Runtime, [&] { Animators[1]->Destroy(); });
		Runtime.Step(1.0f / 60.0f);
		Check(
			Runtime.GetMetrics().StalePoseJobDrops == StaleBeforeDestroy + 1,
			"destroying an Animator before merge rejects its completed pose job"
		);
		Runtime.Step(0.0f);
		Check(
			!Runtime.GetPose(Rigs[1]->GetObjectId()),
			"a destroyed Animator's prior pose retires instead of surviving a stale job completion"
		);

		const auto ActiveBeforeReimport = Runtime.GetMetrics().TrackedAnimators;
		const auto StaleBeforeReimport = Runtime.GetMetrics().StalePoseJobDrops;
		const auto PoseBeforeReimport = Runtime.GetPose(Rigs[2]->GetObjectId())->PoseRevision;
		bool IncompatibleReimportOk = false;
		AnimationRuntimeTestAccess::BeforePoseMerge(Runtime, [&] {
			WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(IncompatibleFixture));
			IncompatibleReimportOk = Assets->ReimportProjectAsset(Mount, MeshReference).Ok;
		});
		Runtime.Step(1.0f / 60.0f);
		auto PoseAfterReimportDrop = Runtime.GetPose(Rigs[2]->GetObjectId());
		Check(
			IncompatibleReimportOk &&
				Runtime.GetMetrics().StalePoseJobDrops == StaleBeforeReimport + ActiveBeforeReimport &&
				PoseAfterReimportDrop && PoseAfterReimportDrop->PoseRevision == PoseBeforeReimport,
			"an incompatible source-group reimport rejects every old-content pose result before Main apply"
		);
		Runtime.Step(0.0f);
		Check(
			!Runtime.GetPose(Rigs[2]->GetObjectId()),
			"the incompatible skeleton revision invalidates the previously committed pose"
		);
		WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(CompatibleFixture));
		const auto CompatibleRestore = Assets->ReimportProjectAsset(Mount, MeshReference);
		Check(CompatibleRestore.Ok, "pose job stale-result test restores the compatible source group");
		Runtime.Step(0.0f);
		Check(
			Runtime.GetPose(Rigs[2]->GetObjectId()).has_value(),
			"compatible content restoration schedules one current pose after stale reimport work was dropped"
		);

		AnimationRuntimeTestAccess::BeforePoseMerge(Runtime, [&] { Runtime.Shutdown(); });
		Runtime.Step(1.0f / 60.0f);
		Check(
			Runtime.GetMetrics().TrackedAnimators == 0 && Runtime.GetMetrics().ActivePoseJobs == 0 &&
				Runtime.GetMetrics().PoseWorkerCapacity == 0,
			"Play shutdown at the pre-merge boundary drains workers and discards all pending results"
		);
		for (auto &AnimatorValue : Animators)
			if (!AnimatorValue->GetDestroyed()) AnimatorValue->Destroy();
		for (auto &Rig : Rigs)
			if (!Rig->GetDestroyed()) Rig->Destroy();
		Tracks.clear();
		Animators.clear();
		Rigs.clear();

		constexpr std::size_t LifecycleRigCount = 500;
		for (std::size_t Cycle = 0; Cycle < 10; ++Cycle) {
			AnimationRuntime CycleRuntime(Assets, {}, AnimationRuntimeOptions{.PoseWorkerCount = 4});
			std::vector<std::shared_ptr<MeshPart>> CycleRigs;
			std::vector<std::shared_ptr<Animator>> CycleAnimators;
			std::vector<std::shared_ptr<AnimationTrack>> CycleTracks;
			CycleRigs.reserve(LifecycleRigCount);
			CycleAnimators.reserve(LifecycleRigCount);
			CycleTracks.reserve(LifecycleRigCount);
			for (std::size_t Index = 0; Index < LifecycleRigCount; ++Index) {
				auto Rig = std::make_shared<MeshPart>();
				Rig->SetMesh(MeshReference);
				Rig->SetParent(WorkspaceValue);
				auto AnimatorValue = std::make_shared<Animator>();
				AnimatorValue->SetParent(Rig);
				CycleRuntime.RegisterAnimator(AnimatorValue);
				auto Track = AnimatorValue->CreateTrack(AnimationReference);
				Track->SetLooped(true);
				Track->Play();
				CycleRigs.push_back(std::move(Rig));
				CycleAnimators.push_back(std::move(AnimatorValue));
				CycleTracks.push_back(std::move(Track));
			}
			CycleRuntime.Step(0.0f);
			const auto ActiveMetrics = CycleRuntime.GetMetrics();
			Check(
				ActiveMetrics.TrackedAnimators == LifecycleRigCount &&
					ActiveMetrics.PoseEvaluations == LifecycleRigCount &&
					ActiveMetrics.PoseJobsScheduled == LifecycleRigCount && ActiveMetrics.ActivePoseJobs == 0,
				"500-Animator Play cycle completes all bounded pose jobs before publication"
			);
			CycleRuntime.Shutdown();
			Check(
				CycleRuntime.GetMetrics().TrackedAnimators == 0 && CycleRuntime.GetMetrics().ActivePoseJobs == 0 &&
					CycleRuntime.GetMetrics().PoseWorkerCapacity == 0,
				"500-Animator Stop cycle retires all scheduler, worker, and pose state"
			);
			for (auto &AnimatorValue : CycleAnimators)
				AnimatorValue->Destroy();
			for (auto &Rig : CycleRigs)
				Rig->Destroy();
			ChangeJournal::Get().Clear();
		}
	}
}

int main() {
	using namespace gargantuan;
	struct SdlProcessCleanup final { ~SdlProcessCleanup() { SDL_Quit(); } } SdlCleanup;
	BootstrapNativeRuntimeSchema();
	TestCpuSkinningGolden();

	const auto Unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-animation-" + Unique);
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
	} CleanupValue{Root};
	std::filesystem::create_directories(Root / "assets");

	auto ValidFixture = MakeAnimationGltfFixture();
	WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(ValidFixture));
	WriteBytes(Root / "assets" / "semantic-tone.wav", MakeWave());
	auto IncompatibleFixture = MakeAnimationGltfFixture();
	IncompatibleFixture.Document["nodes"][2]["name"] = "DifferentLower";
	WriteBytes(Root / "assets" / "incompatible.glb", MakeGlb(IncompatibleFixture));
	auto DuplicateLeafFixture = MakeAnimationGltfFixture();
	DuplicateLeafFixture.Document["nodes"][2]["name"] = "Upper";
	WriteBytes(Root / "assets" / "duplicate-leaf.glb", MakeGlb(DuplicateLeafFixture));
	auto InvalidJoints = MakeAnimationGltfFixture();
	InvalidJoints.Document["skins"][0]["joints"][2] = 99;
	WriteBytes(Root / "assets" / "invalid-joints.glb", MakeGlb(InvalidJoints));
	auto ZeroWeights = MakeAnimationGltfFixture();
	for (std::size_t Index = 0; Index < 4; ++Index) StoreFloat(ZeroWeights.Binary, ZeroWeights.WeightOffset + Index * 4, 0.0f);
	WriteBytes(Root / "assets" / "zero-weights.glb", MakeGlb(ZeroWeights));
	auto SingularInverseBind = MakeAnimationGltfFixture();
	for (std::size_t Index = 0; Index < 16; ++Index)
		StoreFloat(SingularInverseBind.Binary, SingularInverseBind.InverseBindOffset + Index * 4, 0.0f);
	WriteBytes(Root / "assets" / "singular-inverse-bind.glb", MakeGlb(SingularInverseBind));
	auto CubicSpline = MakeAnimationGltfFixture();
	CubicSpline.Document["animations"][0]["samplers"][0]["interpolation"] = "CUBICSPLINE";
	WriteBytes(Root / "assets" / "cubic-spline.glb", MakeGlb(CubicSpline));
	auto BadTimes = MakeAnimationGltfFixture();
	StoreFloat(BadTimes.Binary, BadTimes.TimeOffset + sizeof(float), 0.0f);
	WriteBytes(Root / "assets" / "bad-times.glb", MakeGlb(BadTimes));
	auto SingularScale = MakeAnimationGltfFixture();
	StoreFloat(SingularScale.Binary, SingularScale.ScaleOffset, 0.0f);
	WriteBytes(Root / "assets" / "singular-scale.glb", MakeGlb(SingularScale));
	auto CrossingScale = MakeAnimationGltfFixture();
	CrossingScale.Document["animations"][0]["samplers"][2]["interpolation"] = "LINEAR";
	StoreFloat(CrossingScale.Binary, CrossingScale.ScaleOffset + 3 * sizeof(float), -1.0f);
	WriteBytes(Root / "assets" / "crossing-scale.glb", MakeGlb(CrossingScale));
	auto Oversized = MakeAnimationGltfFixture();
	const auto OneAnimation = Oversized.Document["animations"][0];
	Oversized.Document["animations"] = Json::array();
	for (std::size_t Index = 0; Index <= AssetLimits::MaximumGltfAnimations; ++Index)
		Oversized.Document["animations"].push_back(OneAnimation);
	WriteBytes(Root / "assets" / "oversized.glb", MakeGlb(Oversized));

	DiskFilesystem Filesystem(Root);
	SourceMount Mount(Filesystem);
	auto World = std::make_shared<DataModel>();
	World->Root = Root;
	World->Filesystem = &Filesystem;
	auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
	auto Assets = GetAssets(World);
	Check(Assets && WorkspaceValue, "DataModel constructs animation test services");

	auto Import = Assets->ImportProjectAsset(Mount, "assets/animated-rig.glb", AssetKind::Mesh, "Animated Rig");
	auto MeshRecord = FindRecord(Import, AssetKind::Mesh);
	auto WaveRecord = FindRecord(Import, AssetKind::Animation, "Wave");
	auto CounterRecord = FindRecord(Import, AssetKind::Animation, "Counter");
	Check(Import.Ok && Import.Records.size() == 3 && MeshRecord && WaveRecord && CounterRecord,
		"one glTF source imports one skinned Mesh and one Animation asset per clip");
	auto MeshResource = MeshRecord ? Assets->ResolveMeshResource(MeshRecord->Reference.Value) : std::nullopt;
	auto WaveResource = WaveRecord ? Assets->ResolveAnimation(WaveRecord->Reference.Value) : std::nullopt;
	auto AudioImport = Assets->ImportProjectAsset(
		Mount, "assets/semantic-tone.wav", AssetKind::Audio, "Semantic Tone");
	auto AudioRecord = FindRecord(AudioImport, AssetKind::Audio);
	Check(AudioImport.Ok && AudioRecord, "semantic anchor integration fixture imports positional Audio");
	Check(MeshResource && MeshResource->Value.Skeleton && MeshResource->Value.Skeleton->Joints &&
		MeshResource->Value.Skeleton->Joints->size() == 3 && MeshResource->Value.SkinInfluences &&
		MeshResource->Value.SkinInfluences->size() == 3 && WaveResource && WaveResource->Value.Tracks &&
		WaveResource->Value.Tracks->size() == 2 && WaveResource->Value.Duration == 1.0f,
		"glTF skin, inverse binds, normalized weights, duration, and clip tracks become canonical assets");
	Check(WaveRecord && MeshRecord && WaveRecord->Dependencies == std::vector<AssetId>{MeshRecord->Id} &&
		WaveResource && WaveResource->Value.SkeletonAsset == MeshRecord->Id &&
		WaveResource->Value.SkeletonCompatibilityId == MeshResource->Value.Skeleton->CompatibilityId,
		"Animation artifacts carry a closed Mesh dependency and deterministic skeleton compatibility identity");
	if (MeshResource && MeshResource->Value.SkinInfluences) {
		const auto &Normalized = MeshResource->Value.SkinInfluences->at(2).Weights;
		Check(Near(Normalized.x + Normalized.y + Normalized.z + Normalized.w, 1.0f) &&
			Near(Normalized.y, 0.25f) && Near(Normalized.z, 0.75f),
			"import preserves four bounded normalized joint influences");
	}
	auto DuplicateLeafImport = Assets->ImportProjectAsset(
		Mount, "assets/duplicate-leaf.glb", AssetKind::Mesh, "Duplicate Leaf Rig");
	auto DuplicateLeafMeshRecord = FindRecord(DuplicateLeafImport, AssetKind::Mesh);
	auto DuplicateLeafMesh = DuplicateLeafMeshRecord
		? Assets->ResolveMeshResource(DuplicateLeafMeshRecord->Reference.Value) : std::nullopt;
	Check(DuplicateLeafImport.Ok && DuplicateLeafMesh && DuplicateLeafMesh->Value.Skeleton &&
		DuplicateLeafMesh->Value.Skeleton->Joints &&
		DuplicateLeafMesh->Value.Skeleton->Joints->at(1).Path == "Root/Upper" &&
		DuplicateLeafMesh->Value.Skeleton->Joints->at(2).Path == "Root/Upper/Upper",
		"canonical full paths distinguish duplicate joint leaf names");
	const auto ProjectAssets = Assets->CaptureProjectAssets();
	auto ReloadedAssets = std::make_shared<AssetService>();
	ReloadedAssets->LoadProjectAssetSnapshot(ProjectAssets);
	auto ReloadedMesh = ReloadedAssets->ResolveMeshResource(MeshRecord->Reference.Value);
	auto ReloadedWave = ReloadedAssets->ResolveAnimation(WaveRecord->Reference.Value);
	Check(ReloadedMesh && ReloadedWave && ReloadedMesh->Value.Skeleton &&
		ReloadedMesh->Value.Skeleton->CompatibilityId == ReloadedWave->Value.SkeletonCompatibilityId &&
		ReloadedMesh->Value.Skeleton->Joints &&
		Near(ReloadedMesh->Value.Skeleton->Joints->at(2).InverseBindMatrix[3][1], -2.0f),
		"canonical version-3 Mesh and Animation artifacts round-trip inverse-bind orientation and compatibility");
	if (MeshResource && MeshResource->Value.Skeleton) {
		auto Encoded = EncodeAssetArtifact(ImportedAsset(MeshResource->Value), AssetKind::Mesh);
		Check(Encoded.has_value(), "valid skinned Mesh encodes as a canonical version-3 artifact");
		if (Encoded) {
			auto Tampered = std::vector<std::uint8_t>((*Encoded)->begin(), (*Encoded)->end());
			const auto &Compatibility = MeshResource->Value.Skeleton->CompatibilityId.Bytes;
			auto StoredIdentity = std::search(Tampered.begin(), Tampered.end(), Compatibility.begin(), Compatibility.end());
			Check(StoredIdentity != Tampered.end(), "skinned Mesh artifact stores its skeleton compatibility identity");
			if (StoredIdentity != Tampered.end()) {
				*StoredIdentity ^= 0x80;
				auto Decoded = DecodeAssetArtifact(Tampered, AssetKind::Mesh, AssetContentId::Hash(Tampered));
				Check(!Decoded && Decoded.error().Code == "MalformedArtifact",
					"artifact decode recomputes skeleton compatibility instead of trusting stored identity bytes");
			}
		}
	}

	auto Rig = std::make_shared<MeshPart>();
	Rig->SetName("AnimatedRig");
	Rig->SetMesh(MeshRecord->Reference.Value);
	Rig->SetParent(WorkspaceValue);
	auto StaticPart = std::make_shared<Part>();
	StaticPart->SetName("StaticWitness");
	StaticPart->SetParent(WorkspaceValue);
	auto AnimatorValue = std::make_shared<Animator>();
	AnimatorValue->SetName("RigAnimator");
	AnimatorValue->SetParent(Rig);
	std::vector<std::string> RuntimeDiagnosticCodes;
	AnimationRuntime Runtime(Assets, [&](std::string Code, std::string) {
		RuntimeDiagnosticCodes.push_back(std::move(Code));
	});
	Runtime.RegisterAnimator(AnimatorValue);
	CheckThrows<std::invalid_argument>([&] { (void)AnimatorValue->CreateTrack("assets/not-an-asset"); },
		"Animator rejects raw source paths");
	CheckThrows<std::runtime_error>([&] {
		(void)AnimatorValue->CreateTrack("asset://ffffffffffffffffffffffffffffffff");
	}, "Animator rejects a missing Animation asset");
	auto CapacityRig = std::make_shared<MeshPart>();
	CapacityRig->SetMesh(MeshRecord->Reference.Value);
	CapacityRig->SetParent(WorkspaceValue);
	auto CapacityAnimator = std::make_shared<Animator>();
	CapacityAnimator->SetParent(CapacityRig);
	std::vector<std::shared_ptr<AnimationTrack>> CapacityTracks;
	for (std::size_t Index = 0; Index < Animator::MaximumTracks; ++Index)
		CapacityTracks.push_back(CapacityAnimator->CreateTrack(WaveRecord->Reference.Value));
	CheckThrows<std::length_error>([&] {
		(void)CapacityAnimator->CreateTrack(WaveRecord->Reference.Value);
	}, "Animator fails boundedly when its retained track capacity is exhausted");
	CapacityAnimator->Destroy();
	CapacityRig->Destroy();
	CapacityTracks.clear();
	auto WaveTrack = AnimatorValue->CreateTrack(WaveRecord->Reference.Value);
	Check(WaveTrack && Near(WaveTrack->GetDuration(), 1.0f) &&
		WaveTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped,
		"Animator loads an immutable runtime-only AnimationTrack");
	for (const auto Fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
		CheckPoseSample(Runtime, WaveTrack, Rig->GetObjectId(), Fraction);

	WaveTrack->Play();
	Runtime.Step(0.25f);
	Check(Near(WaveTrack->GetTimePosition(), 0.25f), "Play advances from zero using runtime delta time");
	WaveTrack->Pause();
	Runtime.Step(0.5f);
	Check(Near(WaveTrack->GetTimePosition(), 0.25f) &&
		WaveTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Paused,
		"Pause preserves time while the runtime continues stepping");
	WaveTrack->AdjustSpeed(2.0f);
	WaveTrack->Resume();
	Runtime.Step(0.125f);
	Check(Near(WaveTrack->GetTimePosition(), 0.5f), "Resume and AdjustSpeed produce deterministic advancement");
	WaveTrack->SetTimePosition(10.0f);
	Check(Near(WaveTrack->GetTimePosition(), 1.0f), "seek clamps to clip duration");
	WaveTrack->SetWeight(0.0f);
	Runtime.Step(0.0f);
	Check(!Runtime.GetPose(Rig->GetObjectId()) && Runtime.GetPoseRemoves().size() == 1,
		"a zero-weight track contributes no pose and removes prior transient state");
	WaveTrack->SetWeight(1.0f);
	WaveTrack->SetSpeed(1.0f);
	WaveTrack->SetLooped(true);
	WaveTrack->Play();
	Runtime.Step(1.25f);
	Check(Near(WaveTrack->GetTimePosition(), 0.25f) &&
		WaveTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Playing,
		"looping wraps deterministically without ending");

	std::size_t EndedCount = 0;
	auto EndedConnection = WaveTrack->Ended->Connect([&](std::monostate) { ++EndedCount; });
	WaveTrack->SetLooped(false);
	WaveTrack->Play();
	Runtime.Step(1.0f);
	Check(EndedCount == 1 && WaveTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped &&
		WaveTrack->HoldsNaturalEndPose() && Near(WaveTrack->GetTimePosition(), 1.0f),
		"natural completion holds the final pose and fires Ended exactly once");
	Runtime.Step(1.0f);
	Check(EndedCount == 1, "a held natural end pose does not repeatedly fire Ended");
	auto ReentrantConnection = WaveTrack->Ended->Once([&](std::monostate) { WaveTrack->Play(); });
	WaveTrack->Play();
	Runtime.Step(1.0f);
	Check(EndedCount == 2 && WaveTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Playing &&
		Near(WaveTrack->GetTimePosition(), 0.0f),
		"Ended callback reentrancy may safely restart the same track");
	Runtime.Step(0.0f);
	Check(Runtime.GetPose(Rig->GetObjectId()).has_value(),
		"reentrant Play is reflected on the following pose evaluation");
	for (std::size_t Cycle = 0; Cycle < 10; ++Cycle) {
		WaveTrack->Play();
		Runtime.Step(0.01f);
		WaveTrack->Stop();
		Runtime.Step(0.0f);
	}
	Check(WaveTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped &&
		Near(WaveTrack->GetTimePosition(), 0.0f) && !Runtime.GetPose(Rig->GetObjectId()),
		"ten Play/Stop cycles release transient pose state without leaking playback state");

	auto CounterTrack = AnimatorValue->CreateTrack(CounterRecord->Reference.Value);
	WaveTrack->Play();
	WaveTrack->SetTimePosition(1.0f);
	WaveTrack->SetWeight(0.5f);
	CounterTrack->Play();
	CounterTrack->SetTimePosition(1.0f);
	CounterTrack->SetWeight(0.5f);
	Runtime.Step(0.0f);
	auto BlendedPose = Runtime.GetPose(Rig->GetObjectId());
	if (BlendedPose && BlendedPose->JointModelTransforms) {
		const auto UpperAxis = glm::vec3((*BlendedPose->JointModelTransforms)[1] * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
		const auto LowerOrigin = glm::vec3((*BlendedPose->JointModelTransforms)[2][3]);
		Check(Near(UpperAxis, glm::vec3(1.0f, 0.0f, 0.0f)) &&
			Near(LowerOrigin, glm::vec3(0.5f, 2.0f, 0.0f)),
			"equal-weight opposing clips blend deterministically with bind fallback for partial coverage");
	} else Check(false, "two-track blend produces a headless pose");
	CounterTrack->SetWeight(0.0f);
	Runtime.Step(0.0f);
	Check(Runtime.GetPose(Rig->GetObjectId()).has_value(),
		"a zero-weight second track cannot suppress a visible first track");
	CounterTrack->Stop();
	WaveTrack->SetWeight(1.0f);

	auto DuplicateAnimator = std::make_shared<Animator>();
	DuplicateAnimator->SetName("DuplicateAnimator");
	DuplicateAnimator->SetParent(Rig);
	Runtime.RegisterAnimator(DuplicateAnimator);
	auto DuplicateTrack = DuplicateAnimator->CreateTrack(CounterRecord->Reference.Value);
	DuplicateTrack->Play();
	Runtime.Step(0.0f);
	Check(std::ranges::contains(RuntimeDiagnosticCodes, std::string("DuplicateAnimator")) &&
		Runtime.GetPose(Rig->GetObjectId()).has_value(),
		"duplicate Animator ownership is deterministic, bounded, and preserves the winning pose");
	WaveTrack->Stop();
	Runtime.Step(0.0f);
	Check(Runtime.GetPoseUpdates().size() == 1 && Runtime.GetPoseRemoves().empty() &&
		Runtime.GetPose(Rig->GetObjectId()).has_value(),
		"a waiting Animator can take over one target without a remove/update conflict or stale revision");
	WaveTrack->Play();
	Runtime.Step(0.0f);
	Check(Runtime.GetPoseUpdates().size() == 1 && Runtime.GetPoseRemoves().empty(),
		"the lower-identity Animator deterministically reclaims its target");
	DuplicateAnimator->Destroy();
	Runtime.Step(0.0f);

	auto MeshChanges = Assets->DrainMeshChanges();
	RenderPublisher Publisher;
	Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
	WaveTrack->SetLooped(true);
	WaveTrack->Play();
	Runtime.Step(0.1f);
	Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
	Runtime.ClearChanges();
	auto FirstPublication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
	const auto FirstAnimatedObject = std::ranges::find_if(FirstPublication->Creates, [&](const auto &Create) {
		return Create.Item.Object == Rig->GetObjectId();
	});
	Check(FirstPublication->FullResync && FirstPublication->AnimationPoseUpdates.size() == 1 &&
		FirstPublication->AnimationPoseUpdates[0].Object == Rig->GetObjectId() &&
		FirstAnimatedObject != FirstPublication->Creates.end() && FirstAnimatedObject->Mesh &&
		*FirstAnimatedObject->Mesh == FirstPublication->AnimationPoseUpdates[0].SourceMesh &&
		!FirstPublication->AnimationPoseUpdates[0].PosedMesh.IsValid() &&
		FirstPublication->AnimationPoseUpdates[0].Mode == RenderAnimationSkinningMode::GpuPalette &&
		FirstPublication->AnimationPoseUpdates[0].Palette.Entries &&
		FirstPublication->AnimationPoseUpdates[0].Palette.Entries->size() == 3,
		"full GPU publication retains the immutable source mesh and carries only the final palette");
	RenderProjection Projection;
	auto FirstApplied = Projection.Apply(*FirstPublication);
	Check(FirstApplied.AnimationPosesUpdated == 1 &&
		FirstApplied.PaletteUploadBytes == 3 * sizeof(RenderSkinPaletteEntry) &&
		Projection.GetAnimationPose(Rig->GetObjectId()),
		"headless render projection accepts the renderer-neutral source mesh and palette contract");
	Runtime.Step(0.1f);
	Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
	Runtime.ClearChanges();
	auto Incremental = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
	Check(!Incremental->FullResync && Incremental->AnimationPoseUpdates.size() == 1 &&
		Incremental->MeshVertexUpdates.empty() && Incremental->MeshCreates.empty() && Incremental->Creates.empty() &&
		std::ranges::none_of(Incremental->Updates, [&](const auto &Update) {
			return Update.Object == StaticPart->GetObjectId();
		}), "one GPU animated rig publishes one palette update, no dynamic vertices, and zero static-object updates");
	(void)Projection.Apply(*Incremental);
	auto CpuFallbackAnimator = std::make_shared<Animator>();
	CpuFallbackAnimator->SetParent(Rig);
	auto CpuFallbackTrack = CpuFallbackAnimator->CreateTrack(WaveRecord->Reference.Value);
	CpuFallbackTrack->SetLooped(true);
	CpuFallbackTrack->Play();
	CpuFallbackTrack->SetTimePosition(WaveTrack->GetTimePosition());
	AnimationRuntime CpuFallbackRuntime(
		Assets, {}, AnimationRuntimeOptions{.CpuSkinningFallback = true});
	CpuFallbackRuntime.RegisterAnimator(CpuFallbackAnimator);
	CpuFallbackRuntime.Step(0.0f);
	const auto &CpuFallbackUpdates = CpuFallbackRuntime.GetPoseUpdates();
	Check(CpuFallbackUpdates.size() == 1 &&
		CpuFallbackUpdates[0].Pose.Mode == RenderAnimationSkinningMode::CpuFallback &&
		CpuFallbackUpdates[0].Pose.PosedMesh.IsValid() && CpuFallbackUpdates[0].Vertices &&
		CpuFallbackUpdates[0].Vertices->size() == MeshResource->Value.Vertices->size() &&
		CpuFallbackRuntime.GetMetrics().SkinnedVertices == MeshResource->Value.Vertices->size(),
		"an explicitly unsupported graphical backend retains the pooled CPU-skinned fallback path");
	RenderPublisher CpuFallbackPublisher;
	CpuFallbackPublisher.SetAssetMeshChanges(FirstPublication->MeshCreates, {});
	CpuFallbackPublisher.SetAnimationPoseChanges(CpuFallbackUpdates, CpuFallbackRuntime.GetPoseRemoves());
	auto CpuFallbackPublication = CpuFallbackPublisher.Publish(
		*WorkspaceValue, RenderCameraInput{}, 640, 360);
	RenderProjection CpuFallbackProjection;
	Check(CpuFallbackPublication->MeshCreates.size() == FirstPublication->MeshCreates.size() + 1 &&
		CpuFallbackProjection.Apply(*CpuFallbackPublication).AnimationPosesUpdated == 1 &&
		CpuFallbackProjection.GetAnimationPose(Rig->GetObjectId())->Mode ==
			RenderAnimationSkinningMode::CpuFallback,
		"CPU fallback publication reconstructs its private posed mesh through the same projection contract");
	CpuFallbackRuntime.Shutdown();
	CpuFallbackAnimator->Destroy();
	Publisher.RequestFullResync();
	auto RestartPublication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
	RenderProjection RestartedProjection;
	Check(RestartPublication->FullResync && RestartPublication->AnimationPoseUpdates.size() == 1 &&
		RestartedProjection.Apply(*RestartPublication).AnimationPosesUpdated == 1 &&
		RestartedProjection.GetAnimationPose(Rig->GetObjectId()),
		"renderer restart reconstructs current source residency and semantic palette without restarting playback");
	WaveTrack->Stop();
	Runtime.Step(0.0f);
	Publisher.SetAnimationPoseChanges(Runtime.GetPoseUpdates(), Runtime.GetPoseRemoves());
	Runtime.ClearChanges();
	auto RemovalPublication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 640, 360);
	Check(RemovalPublication->AnimationPoseRemoves.size() == 1 &&
		RemovalPublication->MeshRemoves.empty() &&
		RemovalPublication->AnimationPoseRemoves[0].Object == Rig->GetObjectId(),
		"Stop retires only the GPU palette state because source-mesh residency remains shared");

	WaveTrack->SetLooped(true);
	WaveTrack->Play();
	for (std::size_t Warmup = 0; Warmup < 8; ++Warmup) Runtime.Step(1.0f / 60.0f);
	auto AllocationsAfterWarmup = Runtime.GetMetrics().BufferAllocations;
	for (std::size_t Frame = 0; Frame < 128; ++Frame) Runtime.Step(1.0f / 60.0f);
	Check(Runtime.GetMetrics().BufferAllocations == AllocationsAfterWarmup,
		"steady-state GPU animation evaluation performs no new model-transform or palette allocations");
	auto ReimportAnchor = std::make_shared<Attachment>();
	ReimportAnchor->SetName("ReimportSocket");
	ReimportAnchor->SetJointPath("Root/Upper/Lower");
	ReimportAnchor->SetCFrame(CFrame(0.25f, 0.0f, 0.0f));
	ReimportAnchor->SetParent(Rig);
	std::vector<std::string> SpatialDiagnosticCodes;
	auto ReimportSpatial = std::make_shared<SemanticSpatialResolver>(Assets, &Runtime,
		[&](std::string Code, std::string) { SpatialDiagnosticCodes.push_back(std::move(Code)); });
	ReimportSpatial->RegisterAttachment(ReimportAnchor);
	ReimportSpatial->Step();
	Check(ReimportSpatial->ResolveAttachment(ReimportAnchor)->Animated,
		"semantic binding is active before Mesh source-group reimport");

	auto ModifiedFixture = MakeAnimationGltfFixture(2.0f, 120.0f);
	WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(ModifiedFixture));
	WaveTrack->SetLooped(false);
	WaveTrack->Play();
	WaveTrack->SetTimePosition(1.0f);
	auto Reimport = Assets->ReimportProjectAsset(Mount, WaveRecord->Reference.Value);
	Check(Reimport.Ok, "animation source group reimports atomically during playback");
	Runtime.Step(0.0f);
	ReimportSpatial->Step();
	Check(ReimportSpatial->ResolveAttachment(ReimportAnchor)->Animated,
		"compatible Mesh reimport preserves canonical JointPath binding");
	auto OldRevisionPose = Runtime.GetPose(Rig->GetObjectId());
	Check(OldRevisionPose && OldRevisionPose->JointModelTransforms &&
		Near(glm::vec3((*OldRevisionPose->JointModelTransforms)[2][3]), glm::vec3(-1.0f, 2.0f, 0.0f)),
		"an active AnimationTrack retains its immutable clip revision across reimport");
	WaveTrack->Stop();
	Runtime.Step(0.0f);
	auto NewWaveTrack = AnimatorValue->CreateTrack(WaveRecord->Reference.Value);
	NewWaveTrack->Play();
	NewWaveTrack->SetTimePosition(1.0f);
	Runtime.Step(0.0f);
	auto NewRevisionPose = Runtime.GetPose(Rig->GetObjectId());
	Check(NewRevisionPose && NewRevisionPose->JointModelTransforms &&
		!Near(glm::vec3((*NewRevisionPose->JointModelTransforms)[2][3]), glm::vec3(-1.0f, 2.0f, 0.0f)),
		"tracks loaded after reimport resolve the new canonical clip revision");
	NewWaveTrack->Stop();
	Runtime.Step(0.0f);
	WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(IncompatibleFixture));
	auto IncompatibleReimport = Assets->ReimportProjectAsset(Mount, MeshRecord->Reference.Value);
	Check(IncompatibleReimport.Ok, "incompatible Mesh skeleton reimport commits atomically");
	Runtime.Step(0.0f);
	ReimportSpatial->Step();
	auto IncompatibleAnchor = ReimportSpatial->ResolveAttachment(ReimportAnchor);
	Check(
		IncompatibleAnchor && !IncompatibleAnchor->Animated &&
			std::ranges::contains(SpatialDiagnosticCodes, std::string("IncompatibleSkeleton")),
		"incompatible Mesh reimport invalidates the cached skeleton identity and falls back statically with one "
		"diagnostic"
	);
	WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(ModifiedFixture));
	auto CompatibleRestore = Assets->ReimportProjectAsset(Mount, MeshRecord->Reference.Value);
	Check(CompatibleRestore.Ok, "compatible Mesh skeleton can be restored after an incompatible revision");
	Runtime.Step(0.0f);
	ReimportSpatial->Step();
	Check(
		ReimportSpatial->ResolveAttachment(ReimportAnchor)->Animated,
		"restoring the original compatibility identity recovers the authored canonical path without a numeric-index "
		"bind"
	);

	auto IncompatibleImport = Assets->ImportProjectAsset(Mount, "assets/incompatible.glb", AssetKind::Animation, "Incompatible");
	auto IncompatibleAnimation = FindRecord(IncompatibleImport, AssetKind::Animation);
	Check(IncompatibleImport.Ok && IncompatibleAnimation, "a second valid skeleton imports independently");
	CheckThrows<std::runtime_error>([&] {
		(void)AnimatorValue->CreateTrack(IncompatibleAnimation->Reference.Value);
	}, "Animator rejects a clip whose skeleton compatibility identity differs from the target rig");

	std::filesystem::create_directories(Root / ".gargantuan");
	World->MarkPersistenceSubtreeArchivable();
	World->InitializeLoadedProjectRevision();
	auto ProjectState = Project::forDestination(&Filesystem, InstanceSerialization::InstanceFormat::Json);
	auto AuthoredSnapshot = ProjectState.CaptureGame(World, World->GetAuthoritativeRevision());
	PlaySession Session({1}, AuthoredSnapshot.Contents, InstanceSerialization::InstanceFormat::Json, Root,
		640, 360, AuthoredSnapshot.Revision, AuthoredSnapshot.Assets);
	auto PlayRig = std::dynamic_pointer_cast<MeshPart>(Session.GetWorld()->FindFirstDescendant("AnimatedRig"));
	auto PlayAnimator = std::dynamic_pointer_cast<Animator>(Session.GetWorld()->FindFirstDescendant("RigAnimator"));
	auto PlayAnchor = std::dynamic_pointer_cast<Attachment>(Session.GetWorld()->FindFirstDescendant("ReimportSocket"));
	auto PlayAssets = GetAssets(Session.GetWorld());
	AnimationRuntime PlayRuntime(PlayAssets);
	if (PlayRig && PlayAnimator && PlayAnchor) {
		PlayRuntime.RegisterAnimator(PlayAnimator);
		auto PlaySpatial = std::make_shared<SemanticSpatialResolver>(PlayAssets, &PlayRuntime);
		PlaySpatial->RegisterAttachment(PlayAnchor);
		auto PlayTrack = PlayAnimator->CreateTrack(WaveRecord->Reference.Value);
		PlayTrack->Play();
		PlayRuntime.Step(0.5f);
		PlaySpatial->Step();
		Check(PlayRuntime.GetPose(PlayRig->GetObjectId()).has_value() &&
			PlaySpatial->ResolveAttachment(PlayAnchor)->Animated,
			"saved authoring hierarchy and canonical Attachment binding clone into Play and evaluate headlessly");
		PlayAnimator->Destroy();
		PlayTrack->Play();
		Check(PlayTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped,
			"destroying Animator synchronously invalidates externally retained AnimationTracks");
		PlayRuntime.Step(0.0f);
		Check(!PlayRuntime.GetPose(PlayRig->GetObjectId()) && PlayTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped,
			"destroying Animator during playback invalidates tracks and removes its transient pose");
		PlaySpatial->Step();
		Check(PlaySpatial->ResolveAttachment(PlayAnchor)->Animated,
			"Play clone falls back to skeleton bind pose after Animator destruction");
		PlaySpatial->Shutdown();
	} else Check(false, "Play clone preserves MeshPart, Animator, and authored semantic Attachment instances");
	Session.Stop();
	Check(Session.GetState() == PlaySessionState::Stopped && AnimatorValue->GetParent().has_value() &&
		AnimatorValue->GetName() == "RigAnimator" && ReimportAnchor->GetJointPath() == "Root/Upper/Lower",
		"Stop discards runtime pose state while preserving the authored rig, Animator, and canonical binding");
	for (std::size_t Cycle = 0; Cycle < 10; ++Cycle) {
		PlaySession CycleSession({static_cast<std::uint64_t>(Cycle + 2)}, AuthoredSnapshot.Contents,
			InstanceSerialization::InstanceFormat::Json, Root, 320, 180,
			AuthoredSnapshot.Revision, AuthoredSnapshot.Assets);
		auto CycleRig = std::dynamic_pointer_cast<MeshPart>(CycleSession.GetWorld()->FindFirstDescendant("AnimatedRig"));
		auto CycleAnimator = std::dynamic_pointer_cast<Animator>(
			CycleSession.GetWorld()->FindFirstDescendant("RigAnimator"));
		auto CycleAnchor = std::dynamic_pointer_cast<Attachment>(
			CycleSession.GetWorld()->FindFirstDescendant("ReimportSocket"));
		auto CycleAssets = GetAssets(CycleSession.GetWorld());
		AnimationRuntime CycleRuntime(CycleAssets);
		auto CycleSpatial = std::make_shared<SemanticSpatialResolver>(CycleAssets, &CycleRuntime);
		if (CycleRig && CycleAnimator && CycleAnchor) {
			CycleRuntime.RegisterAnimator(CycleAnimator);
			CycleSpatial->RegisterAttachment(CycleAnchor);
			auto CycleTrack = CycleAnimator->CreateTrack(WaveRecord->Reference.Value);
			CycleTrack->Play();
			CycleRuntime.Step(0.25f);
			CycleSpatial->Step();
			Check(CycleSpatial->ResolveAttachment(CycleAnchor)->Animated,
				"repeated Play clone resolves the semantic binding without stale registration");
		} else Check(false, "repeated Play clone preserves the semantic rig hierarchy");
		CycleSpatial->Shutdown();
		CycleRuntime.Shutdown();
		CycleSession.Stop();
		Check(CycleSession.GetState() == PlaySessionState::Stopped,
			"repeated Stop tears down semantic resolver state deterministically");
	}

	auto RuntimeAssets = Assets->CaptureRuntimeAssets();
	auto PackagedWorld = std::make_shared<DataModel>();
	auto PackagedAssets = GetAssets(PackagedWorld);
	PackagedAssets->LoadRuntimeAssetSnapshot(RuntimeAssets);
	auto PackagedWorkspace = std::dynamic_pointer_cast<Workspace>(PackagedWorld->GetService("Workspace"));
	auto PackagedRig = std::make_shared<MeshPart>();
	PackagedRig->SetMesh(MeshRecord->Reference.Value);
	PackagedRig->SetParent(PackagedWorkspace);
	auto PackagedAnimator = std::make_shared<Animator>();
	PackagedAnimator->SetParent(PackagedRig);
	AnimationRuntime PackagedRuntime(PackagedAssets);
	PackagedRuntime.RegisterAnimator(PackagedAnimator);
	auto PackagedTrack = PackagedAnimator->CreateTrack(WaveRecord->Reference.Value);
	PackagedTrack->Play();
	PackagedRuntime.Step(0.5f);
	Check(PackagedRuntime.GetPose(PackagedRig->GetObjectId()).has_value(),
		"runtime-only canonical artifacts load and animate without the source glTF");
	auto MissingDependencySnapshot = RuntimeAssets;
	auto MissingDependencyCatalog = Json::parse(MissingDependencySnapshot.CatalogJson);
	for (auto &Record : MissingDependencyCatalog["Assets"]) if (Record["Kind"] == "Animation") {
		Record["Dependencies"].push_back("11111111111111111111111111111111");
		break;
	}
	MissingDependencySnapshot.CatalogJson = MissingDependencyCatalog.dump();
	CheckThrows<std::runtime_error>([&] {
		auto InvalidPackagedAssets = std::make_shared<AssetService>();
		InvalidPackagedAssets->LoadRuntimeAssetSnapshot(MissingDependencySnapshot);
	}, "runtime package validation rejects an Animation dependency outside the closed catalog");

	auto ValidationWorld = std::make_shared<DataModel>();
	auto ValidationAssets = GetAssets(ValidationWorld);
	auto ValidateFailure = [&](std::string Source, std::string Code) {
		auto Result = ValidationAssets->ImportProjectAsset(Mount, std::move(Source), AssetKind::Animation, "Invalid");
		Check(!Result.Ok && Result.Diagnostic.Code == Code,
			("import rejects malformed animation fixture with " + Code).c_str());
	};
	ValidateFailure("assets/invalid-joints.glb", "MalformedSkeleton");
	ValidateFailure("assets/zero-weights.glb", "MalformedSkinWeights");
	ValidateFailure("assets/singular-inverse-bind.glb", "MalformedSkeleton");
	ValidateFailure("assets/cubic-spline.glb", "UnsupportedInterpolation");
	ValidateFailure("assets/bad-times.glb", "MalformedAnimation");
	ValidateFailure("assets/singular-scale.glb", "MalformedAnimation");
	ValidateFailure("assets/crossing-scale.glb", "MalformedAnimation");
	ValidateFailure("assets/oversized.glb", "GltfLimit");
	if (MeshRecord && WaveRecord && CounterRecord && AudioRecord)
		TestSemanticAnimatedAnchors(
			ProjectAssets, MeshRecord->Reference.Value, WaveRecord->Reference.Value,
			CounterRecord->Reference.Value, AudioRecord->Reference.Value);

	if (DuplicateLeafMeshRecord) {
		auto DuplicateLeafRig = std::make_shared<MeshPart>();
		DuplicateLeafRig->SetMesh(DuplicateLeafMeshRecord->Reference.Value);
		DuplicateLeafRig->SetParent(WorkspaceValue);
		auto UpperAnchor = std::make_shared<Attachment>();
		UpperAnchor->SetJointPath("Root/Upper");
		UpperAnchor->SetParent(DuplicateLeafRig);
		auto NestedUpperAnchor = std::make_shared<Attachment>();
		NestedUpperAnchor->SetJointPath("Root/Upper/Upper");
		NestedUpperAnchor->SetParent(DuplicateLeafRig);
		auto DuplicateLeafSpatial = std::make_shared<SemanticSpatialResolver>(Assets);
		DuplicateLeafSpatial->RegisterAttachment(UpperAnchor);
		DuplicateLeafSpatial->RegisterAttachment(NestedUpperAnchor);
		DuplicateLeafSpatial->Step();
		auto UpperTransform = DuplicateLeafSpatial->ResolveAttachment(UpperAnchor);
		auto NestedUpperTransform = DuplicateLeafSpatial->ResolveAttachment(NestedUpperAnchor);
		Check(UpperTransform && NestedUpperTransform && UpperTransform->Animated &&
			NestedUpperTransform->Animated &&
			!Near(UpperTransform->WorldCFrame.Position, NestedUpperTransform->WorldCFrame.Position),
			"full canonical JointPath resolves duplicate leaf names to distinct skeleton joints");
		DuplicateLeafSpatial->Shutdown();
		DuplicateLeafRig->Destroy();
	}

	if (MeshRecord) {
		auto LimitRig = std::make_shared<MeshPart>();
		LimitRig->SetMesh(MeshRecord->Reference.Value);
		LimitRig->SetParent(WorkspaceValue);
		std::vector<std::string> LimitDiagnostics;
		auto LimitSpatial = std::make_shared<SemanticSpatialResolver>(Assets, nullptr,
			[&](std::string Code, std::string) { LimitDiagnostics.push_back(std::move(Code)); });
		std::vector<std::shared_ptr<Attachment>> LimitAnchors;
		LimitAnchors.reserve(SemanticSpatialResolver::MaximumSemanticAnchorsPerRig + 1);
		for (std::size_t Index = 0;
			Index <= SemanticSpatialResolver::MaximumSemanticAnchorsPerRig; ++Index) {
			auto LimitAnchor = std::make_shared<Attachment>();
			LimitAnchor->SetJointPath("Root/Upper");
			LimitAnchor->SetParent(LimitRig);
			LimitSpatial->RegisterAttachment(LimitAnchor);
			LimitAnchors.push_back(std::move(LimitAnchor));
		}
		LimitSpatial->Step();
		auto FirstLimitTransform = LimitSpatial->ResolveAttachment(LimitAnchors.front());
		auto LastAcceptedTransform = LimitSpatial->ResolveAttachment(
			LimitAnchors[SemanticSpatialResolver::MaximumSemanticAnchorsPerRig - 1]);
		auto OverflowTransform = LimitSpatial->ResolveAttachment(LimitAnchors.back());
		Check(LimitSpatial->GetMetrics().IndexedSemanticAnchors ==
				SemanticSpatialResolver::MaximumSemanticAnchorsPerRig &&
			FirstLimitTransform && FirstLimitTransform->Animated &&
			LastAcceptedTransform && LastAcceptedTransform->Animated &&
			OverflowTransform && !OverflowTransform->Animated &&
			std::ranges::contains(LimitDiagnostics, std::string("RigAnchorLimit")),
			"the per-rig semantic anchor bound accepts the first 1,024 registrations and fails the overflow statically");
		LimitSpatial->Shutdown();
		LimitRig->Destroy();
	}
	if (MeshRecord && WaveRecord)
		TestAnimationUpdatePolicy(Assets, WorkspaceValue, MeshRecord->Reference.Value, WaveRecord->Reference.Value);
	if (MeshRecord && WaveRecord)
		TestAnimationPoseJobs(
			Assets,
			WorkspaceValue,
			MeshRecord->Reference.Value,
			WaveRecord->Reference.Value,
			Root,
			Mount,
			ModifiedFixture,
			IncompatibleFixture
		);

	ValidationWorld->Destroy();
	PackagedRuntime.Shutdown();
	PackagedWorld->Destroy();
	ReimportSpatial->Shutdown();
	Runtime.Shutdown();
	ReloadedAssets.reset();
	Assets.reset();
	WorkspaceValue.reset();
	World->Destroy();
	World.reset();

	if (Failures == 0) {
		std::cout << "All Animation Foundation 1 and GPU publication tests passed\n";
		return 0;
	}
	std::cerr << Failures << " Animation Foundation test(s) failed\n";
	return 1;
}
