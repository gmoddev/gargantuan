#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
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
	auto IncompatibleFixture = MakeAnimationGltfFixture();
	IncompatibleFixture.Document["nodes"][2]["name"] = "DifferentLower";
	WriteBytes(Root / "assets" / "incompatible.glb", MakeGlb(IncompatibleFixture));
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

	auto ModifiedFixture = MakeAnimationGltfFixture(2.0f, 120.0f);
	WriteBytes(Root / "assets" / "animated-rig.glb", MakeGlb(ModifiedFixture));
	WaveTrack->SetLooped(false);
	WaveTrack->Play();
	WaveTrack->SetTimePosition(1.0f);
	auto Reimport = Assets->ReimportProjectAsset(Mount, WaveRecord->Reference.Value);
	Check(Reimport.Ok, "animation source group reimports atomically during playback");
	Runtime.Step(0.0f);
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
	auto PlayAssets = GetAssets(Session.GetWorld());
	AnimationRuntime PlayRuntime(PlayAssets);
	if (PlayRig && PlayAnimator) {
		PlayRuntime.RegisterAnimator(PlayAnimator);
		auto PlayTrack = PlayAnimator->CreateTrack(WaveRecord->Reference.Value);
		PlayTrack->Play();
		PlayRuntime.Step(0.5f);
		Check(PlayRuntime.GetPose(PlayRig->GetObjectId()).has_value(),
			"saved authoring hierarchy clones into Play and evaluates headlessly");
		PlayAnimator->Destroy();
		PlayTrack->Play();
		Check(PlayTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped,
			"destroying Animator synchronously invalidates externally retained AnimationTracks");
		PlayRuntime.Step(0.0f);
		Check(!PlayRuntime.GetPose(PlayRig->GetObjectId()) && PlayTrack->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped,
			"destroying Animator during playback invalidates tracks and removes its transient pose");
	} else Check(false, "Play clone preserves MeshPart and authored Animator instances");
	Session.Stop();
	Check(Session.GetState() == PlaySessionState::Stopped && AnimatorValue->GetParent().has_value() &&
		AnimatorValue->GetName() == "RigAnimator",
		"Stop discards runtime tracks while preserving the authored rig and Animator");

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

	ValidationWorld->Destroy();
	PackagedRuntime.Shutdown();
	PackagedWorld->Destroy();
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
