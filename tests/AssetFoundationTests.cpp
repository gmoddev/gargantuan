#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/assets/AssetImporter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gargantuan {
	struct WorldRootTestAccess {
		static WorldRaycastResult Raycast(WorldRoot &World) {
			return World.ResolveRaycast({0.25f, 0.25f, 2.0f}, {0.0f, 0.0f, -4.0f});
		}
		static bool IsDirty(const WorldRoot &World, const BasePart &Part) {
			return World.DirtyBodies.contains(Part.GetObjectId());
		}
	};
}

namespace {
	// Observe the real SourceMount read boundary after reimport marks records
	// Importing, without timing races or a test-only hook in production code.
	class ObservingFilesystem final : public gargantuan::BaseFilesystem {
		gargantuan::DiskFilesystem Backend;
	  public:
		std::function<void()> BeforeRead;
		explicit ObservingFilesystem(const std::filesystem::path &Path) : BaseFilesystem(Path), Backend(Path) {}
		gargantuan::FileMetadata Metadata(const std::filesystem::path &Path) const override { return Backend.Metadata(Path); }
		bool Exists(const std::filesystem::path &Path) const override { return Backend.Exists(Path); }
		std::unique_ptr<gargantuan::FileHandle> Open(const std::filesystem::path &Path,
			const gargantuan::FileOpen &Mode = gargantuan::FileOpen::Read) override {
			if (Mode == gargantuan::FileOpen::Read && BeforeRead) {
				auto Observe = std::exchange(BeforeRead, {});
				Observe();
			}
			return Backend.Open(Path, Mode);
		}
		void CreateDirectory(const std::filesystem::path &Path) override { Backend.CreateDirectory(Path); }
		void Remove(const std::filesystem::path &Path) override { Backend.Remove(Path); }
		std::vector<gargantuan::DirectoryEntry> GetChildren(const std::filesystem::path &Path) override {
			return Backend.GetChildren(Path);
		}
		std::vector<gargantuan::DirectoryEntry> GetDescendants(const std::filesystem::path &Path) override {
			return Backend.GetDescendants(Path);
		}
	};

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

	void AlignFour(std::vector<std::uint8_t> &Bytes, std::uint8_t Padding = 0) {
		while (Bytes.size() % 4 != 0) Bytes.push_back(Padding);
	}

	std::vector<std::uint8_t> MakeBmp(std::uint32_t Width, std::uint32_t Height, std::uint8_t Seed) {
		const auto PixelBytes = static_cast<std::size_t>(Width) * Height * 4;
		std::vector<std::uint8_t> Bytes;
		Bytes.reserve(54 + PixelBytes);
		Bytes.push_back('B'); Bytes.push_back('M');
		AppendU32(Bytes, static_cast<std::uint32_t>(54 + PixelBytes));
		AppendU16(Bytes, 0); AppendU16(Bytes, 0); AppendU32(Bytes, 54);
		AppendU32(Bytes, 40); AppendU32(Bytes, Width); AppendU32(Bytes, Height);
		AppendU16(Bytes, 1); AppendU16(Bytes, 32); AppendU32(Bytes, 0);
		AppendU32(Bytes, static_cast<std::uint32_t>(PixelBytes));
		AppendU32(Bytes, 2835); AppendU32(Bytes, 2835); AppendU32(Bytes, 0); AppendU32(Bytes, 0);
		for (std::size_t Index = 0; Index < PixelBytes / 4; ++Index) {
			Bytes.push_back(static_cast<std::uint8_t>(Seed + Index));
			Bytes.push_back(static_cast<std::uint8_t>(Seed + Index * 3));
			Bytes.push_back(static_cast<std::uint8_t>(Seed + Index * 7));
			Bytes.push_back(255);
		}
		return Bytes;
	}

	std::vector<std::uint8_t> MakeHdr(std::uint32_t Width, std::uint32_t Height,
		std::array<std::uint8_t, 4> Rgbe) {
		const auto Header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(Height) +
			" +X " + std::to_string(Width) + "\n";
		std::vector<std::uint8_t> Bytes(Header.begin(), Header.end());
		for (std::uint32_t Y = 0; Y < Height; ++Y) for (std::uint32_t X = 0; X < Width; ++X)
			Bytes.insert(Bytes.end(), Rgbe.begin(), Rgbe.end());
		return Bytes;
	}

	std::vector<std::uint8_t> MakeHdrGradient() {
		auto Bytes = MakeHdr(8, 4, {0, 0, 0, 0});
		const auto HeaderBytes = Bytes.size() - 8 * 4 * 4;
		for (std::uint32_t Y = 0; Y < 4; ++Y) for (std::uint32_t X = 0; X < 8; ++X) {
			const auto Offset = HeaderBytes + (Y * 8 + X) * 4;
			Bytes[Offset] = static_cast<std::uint8_t>(32 + X * 16);
			Bytes[Offset + 1] = static_cast<std::uint8_t>(32 + Y * 32);
			Bytes[Offset + 2] = 0;
			Bytes[Offset + 3] = 136;
		}
		return Bytes;
	}

	float ReadHdrChannel(const gargantuan::ImportedImage &Image, std::size_t Pixel, unsigned Channel) {
		const auto Offset = (Pixel * 4 + Channel) * 4;
		std::uint32_t Bits = 0;
		for (unsigned Byte = 0; Byte < 4; ++Byte)
			Bits |= static_cast<std::uint32_t>((*Image.Pixels)[Offset + Byte]) << (Byte * 8);
		return std::bit_cast<float>(Bits);
	}

	std::string EncodeBase64(std::span<const std::uint8_t> Bytes) {
		static constexpr std::string_view Alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Result;
		Result.reserve((Bytes.size() + 2) / 3 * 4);
		for (std::size_t Offset = 0; Offset < Bytes.size(); Offset += 3) {
			const auto Remaining = Bytes.size() - Offset;
			const auto Value = static_cast<std::uint32_t>(Bytes[Offset]) << 16 |
				(Remaining > 1 ? static_cast<std::uint32_t>(Bytes[Offset + 1]) << 8 : 0) |
				(Remaining > 2 ? static_cast<std::uint32_t>(Bytes[Offset + 2]) : 0);
			Result.push_back(Alphabet[(Value >> 18) & 63]);
			Result.push_back(Alphabet[(Value >> 12) & 63]);
			Result.push_back(Remaining > 1 ? Alphabet[(Value >> 6) & 63] : '=');
			Result.push_back(Remaining > 2 ? Alphabet[Value & 63] : '=');
		}
		return Result;
	}

	struct GltfFixture {
		std::vector<std::uint8_t> Binary;
		nlohmann::ordered_json Document;
		std::size_t ImageOffset = 0;
		std::size_t ImageBytes = 0;
	};

	GltfFixture MakeGltfFixture(
		std::uint8_t ImageSeed,
		bool AddOrphanMaterial = false,
		bool InvalidIndex = false,
		bool NonFinitePosition = false
	) {
		GltfFixture Result;
		for (const auto Value : {
			0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 0.0f,
			0.0f, 1.0f, NonFinitePosition ? std::numeric_limits<float>::quiet_NaN() : 0.0f,
		}) AppendFloat(Result.Binary, Value);
		for (std::size_t Index = 0; Index < 3; ++Index) {
			AppendFloat(Result.Binary, 0.0f); AppendFloat(Result.Binary, 0.0f); AppendFloat(Result.Binary, 1.0f);
		}
		for (const auto Value : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f}) AppendFloat(Result.Binary, Value);
		AppendU16(Result.Binary, 0); AppendU16(Result.Binary, 1); AppendU16(Result.Binary, InvalidIndex ? 3 : 2);
		AppendU16(Result.Binary, 0); AppendU16(Result.Binary, 1); AppendU16(Result.Binary, 2);
		AlignFour(Result.Binary);
		Result.ImageOffset = Result.Binary.size();
		auto Image = MakeBmp(2, 2, ImageSeed);
		Result.ImageBytes = Image.size();
		Result.Binary.insert(Result.Binary.end(), Image.begin(), Image.end());
		AlignFour(Result.Binary);

		auto &Root = Result.Document;
		Root["asset"] = {{"version", "2.0"}, {"generator", "Gargantuan Asset Foundation 2A test"}};
		Root["buffers"] = nlohmann::ordered_json::array({{{"byteLength", Result.Binary.size()}}});
		Root["bufferViews"] = nlohmann::ordered_json::array({
			{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}},
			{{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}},
			{{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 24}},
			{{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 6}},
			{{"buffer", 0}, {"byteOffset", 102}, {"byteLength", 6}},
			{{"buffer", 0}, {"byteOffset", Result.ImageOffset}, {"byteLength", Result.ImageBytes}},
		});
		Root["accessors"] = nlohmann::ordered_json::array({
			{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
			{{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
			{{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
			{{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
			{{"bufferView", 4}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
		});
		Root["images"] = nlohmann::ordered_json::array({
			{{"name", "Surface"}, {"bufferView", 5}, {"mimeType", "image/bmp"}},
		});
		Root["textures"] = nlohmann::ordered_json::array({{{"source", 0}}});
		Root["materials"] = nlohmann::ordered_json::array({
			{{"name", "Textured"},
			 {"pbrMetallicRoughness", {{"baseColorFactor", {0.8, 0.7, 0.6, 1.0}},
				 {"baseColorTexture", {{"index", 0}}}, {"metallicRoughnessTexture", {{"index", 0}}},
				 {"metallicFactor", 0.25}, {"roughnessFactor", 0.75}}},
			 {"normalTexture", {{"index", 0}, {"scale", 1.0}}},
			 {"alphaMode", "MASK"}, {"alphaCutoff", 0.4}, {"doubleSided", true}},
			{{"name", "Translucent"},
			 {"pbrMetallicRoughness", {{"baseColorFactor", {0.2, 0.4, 0.9, 0.5}},
				 {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}, {"alphaMode", "BLEND"}},
		});
		if (AddOrphanMaterial) Root["materials"].push_back({
			{"name", "Orphan"}, {"pbrMetallicRoughness", {{"baseColorFactor", {1.0, 0.0, 1.0, 1.0}}}}
		});
		Root["meshes"] = nlohmann::ordered_json::array({
			{{"name", "Compound Triangle"}, {"primitives", nlohmann::ordered_json::array({
				{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", 3}, {"material", 0}},
				{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", 4}, {"material", 1}},
			})}},
		});
		return Result;
	}

	std::vector<std::uint8_t> MakeGlb(const GltfFixture &Fixture) {
		auto JsonText = Fixture.Document.dump();
		while (JsonText.size() % 4 != 0) JsonText.push_back(' ');
		std::vector<std::uint8_t> Result;
		Result.reserve(12 + 8 + JsonText.size() + 8 + Fixture.Binary.size());
		AppendU32(Result, 0x46546c67u); AppendU32(Result, 2);
		AppendU32(Result, static_cast<std::uint32_t>(12 + 8 + JsonText.size() + 8 + Fixture.Binary.size()));
		AppendU32(Result, static_cast<std::uint32_t>(JsonText.size())); AppendU32(Result, 0x4e4f534au);
		Result.insert(Result.end(), JsonText.begin(), JsonText.end());
		AppendU32(Result, static_cast<std::uint32_t>(Fixture.Binary.size())); AppendU32(Result, 0x004e4942u);
		Result.insert(Result.end(), Fixture.Binary.begin(), Fixture.Binary.end());
		return Result;
	}

	void WriteBytes(const std::filesystem::path &Path, std::span<const std::uint8_t> Bytes) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		if (!Output) throw std::runtime_error("Could not write asset test fixture");
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Text) {
		WriteBytes(Path, std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size()));
	}

	std::shared_ptr<gargantuan::AssetService> GetAssets(const std::shared_ptr<gargantuan::DataModel> &World) {
		return std::dynamic_pointer_cast<gargantuan::AssetService>(World->GetService("AssetService"));
	}

	void CheckMaterialArtifactVersions() {
		using namespace gargantuan;
		ImportedMaterial Material;
		const AssetId Image{1, 2};
		std::vector<std::uint8_t> Expected{'G', 'A', 'R', 'G', 'A', 'S', '0', '1'};
		AppendU32(Expected, 2);
		Expected.push_back(static_cast<std::uint8_t>(AssetKind::Material));
		for (std::size_t Index = 0; Index < 6; ++Index) AppendFloat(Expected, 1.0f);
		Expected.insert(Expected.end(), {0, 0, 0}); // Color reference, normal reference, opaque mode.
		AppendFloat(Expected, 0.5f);
		Expected.push_back(0); // Single sided.
		const auto V2 = EncodeAssetArtifact(Material, AssetKind::Material);
		const auto V2Decoded = DecodeAssetArtifact(Expected, AssetKind::Material, AssetContentId::Hash(Expected));
		Check(V2 && **V2 == Expected && V2Decoded &&
			!std::get<ImportedMaterial>(V2Decoded->Asset).HasAdvancedInputs(),
			"legacy v2 material bytes and defaults remain unchanged");

		Material.MetallicRoughnessTexture = Image;
		Expected[8] = 5;
		Expected.insert(Expected.end(), {1, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0});
		const auto V5 = EncodeAssetArtifact(Material, AssetKind::Material);
		const auto V5Decoded = DecodeAssetArtifact(Expected, AssetKind::Material, AssetContentId::Hash(Expected));
		Check(V5 && **V5 == Expected && V5Decoded &&
			std::get<ImportedMaterial>(V5Decoded->Asset).MetallicRoughnessTexture == Image &&
			!std::get<ImportedMaterial>(V5Decoded->Asset).HasAdvancedInputs(),
			"legacy v5 packed-material bytes and defaults remain unchanged");

		Material.MetallicRoughnessTexture.reset();
		Material.EmissiveFactor = {.25f, .5f, 1.0f};
		Material.EmissiveTexture = Image;
		Material.OcclusionTexture = Image;
		Material.OcclusionStrength = .25f;
		const auto OcclusionSlot = static_cast<std::size_t>(RenderMaterialTextureSlot::Occlusion);
		Material.TextureSamplers[OcclusionSlot].WrapV = RenderTextureWrap::MirroredRepeat;
		const auto V6 = EncodeAssetArtifact(Material, AssetKind::Material);
		Check(V6 && (*V6)->at(8) == 6, "advanced material without a packed texture uses artifact v6");
		if (!V6) return;
		const auto V6Decoded = DecodeAssetArtifact(**V6, AssetKind::Material, AssetContentId::Hash(**V6));
		bool Stable = false;
		if (V6Decoded) {
			const auto Reencoded = EncodeAssetArtifact(V6Decoded->Asset, AssetKind::Material);
			Stable = Reencoded && **Reencoded == **V6;
		}
		Check(Stable, "v6 material decode and re-encode are byte stable");
		auto InvalidSampler = **V6;
		InvalidSampler.back() = 255;
		Check(!DecodeAssetArtifact(InvalidSampler, AssetKind::Material, AssetContentId::Hash(InvalidSampler)),
			"v6 material rejects invalid sampler enum even with a valid artifact hash");
		Material.OcclusionStrength = 1.25f;
		Check(!EncodeAssetArtifact(Material, AssetKind::Material), "v6 material rejects out-of-range occlusion strength");
	}
}

int main() {
	using namespace gargantuan;
	struct SdlProcessCleanup final { ~SdlProcessCleanup() { SDL_Quit(); } } SdlCleanup;
	BootstrapNativeRuntimeSchema();
	CheckMaterialArtifactVersions();

	auto NewProjectWorld = std::make_shared<DataModel>();
	(void)NewProjectWorld->GetService("Workspace");
	NewProjectWorld->InitializeLoadedProjectRevision();
	const auto NewProjectRevision = NewProjectWorld->GetAuthoritativeRevision();
	Check(NewProjectRevision == 1 && NewProjectWorld->GetService("AssetService") &&
		NewProjectWorld->GetAuthoritativeRevision() == NewProjectRevision,
		"canonical AssetService establishment is complete before new-project revision 1 is visible");
	NewProjectWorld->Destroy();

	Check(AssetReference::Parse("asset://0123456789abcdef0123456789abcdef").has_value(),
		"canonical project asset references are accepted");
	Check(AssetReference::Parse("builtin://font/default").has_value(),
		"canonical built-in asset references are accepted");
	Check(!AssetReference::Parse("asset://0123456789ABCDEF0123456789ABCDEF") &&
		!AssetReference::Parse("asset://short") && !AssetReference::Parse("../assets/a.png") &&
		!AssetReference::Parse("builtin://Font/Default"),
		"ambiguous or non-canonical asset references are rejected");
	const std::array<std::uint8_t, 3> Abc{'a', 'b', 'c'};
	Check(AssetContentId::Hash(Abc).ToString() ==
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"content identity uses deterministic SHA-256");
	std::vector<std::uint8_t> LegacyImageArtifact{'G', 'A', 'R', 'G', 'A', 'S', '0', '1'};
	AppendU32(LegacyImageArtifact, 1);
	LegacyImageArtifact.push_back(static_cast<std::uint8_t>(AssetKind::Image));
	AppendU32(LegacyImageArtifact, 1); AppendU32(LegacyImageArtifact, 1);
	LegacyImageArtifact.insert(LegacyImageArtifact.end(), {1, 2, 3, 255});
	const auto LegacyContent = AssetContentId::Hash(LegacyImageArtifact);
	const auto LegacyId = AssetId::Parse("0123456789abcdef0123456789abcdef").value();
	nlohmann::ordered_json LegacyCatalog{{"Version", 1}, {"Assets", nlohmann::ordered_json::array({{
		{"AssetId", LegacyId.ToString()}, {"Reference", AssetReference::FromAssetId(LegacyId).Value},
		{"Kind", "Image"}, {"Name", "Legacy Image"}, {"Source", "assets/legacy.bmp"},
		{"ContentId", LegacyContent.ToString()}, {"ContentRevision", 1}, {"State", "Ready"},
		{"Diagnostic", nullptr},
	}})}};
	auto LegacyBytes = std::make_shared<const std::vector<std::uint8_t>>(LegacyImageArtifact);
	AssetProjectSnapshot LegacySnapshot{LegacyCatalog.dump(), {{
		".gargantuan/assets/artifacts/" + LegacyContent.ToString() + ".gasset", LegacyBytes,
	}}};
	auto LegacyAssets = std::make_shared<AssetService>();
	LegacyAssets->LoadProjectAssetSnapshot(LegacySnapshot);
	Check(LegacyAssets->ResolveImage(AssetReference::FromAssetId(LegacyId).Value).has_value() &&
		LegacyAssets->GetAsset(AssetReference::FromAssetId(LegacyId).Value)->SourceGroupId == LegacyId,
		"Foundation 1 catalog and artifact version 1 load with explicit single-asset grouping compatibility");

	const auto Unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-assets-" + Unique);
	const auto Relocated = std::filesystem::temp_directory_path() / ("gargantuan-assets-relocated-" + Unique);
	struct Cleanup {
		std::filesystem::path First;
		std::filesystem::path Second;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(First, Ignored); std::filesystem::remove_all(Second, Ignored); }
	} CleanupValue{Root, Relocated};
	std::filesystem::create_directories(Root / "assets");
	WriteBytes(Root / "assets" / "checker.bmp", MakeBmp(2, 2, 7));
	WriteText(Root / "assets" / "triangle.obj",
		"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n");
	WriteText(Root / "assets" / "bad-index.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n");
	WriteText(Root / "assets" / "non-finite.obj", "v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
	WriteText(Root / "assets" / "thin.obj", "v 0 0 0\nv 1 0 0\nv 0 0.000001 0\nf 1 2 3\n");
	WriteText(Root / "assets" / "extreme.obj", "v 0 0 0\nv 1000001 0 0\nv 0 1 0\nf 1 2 3\n");
	WriteText(Root / "assets" / "bad.png", "not a png");
	WriteText(Root / "assets" / "bad.ttf", "not a font");
	std::filesystem::copy_file(std::filesystem::path(GARGANTUAN_ASSET_TEST_FONT_PATH),
		Root / "assets" / "font.ttf", std::filesystem::copy_options::overwrite_existing);
	std::filesystem::copy_file(std::filesystem::path(GARGANTUAN_ASSET_TEST_PNG_PATH),
		Root / "assets" / "representative.png", std::filesystem::copy_options::overwrite_existing);
	auto CompoundFixture = MakeGltfFixture(91);
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(CompoundFixture));
	auto ExternalFixture = MakeGltfFixture(37);
	auto ExternalImage = MakeBmp(2, 2, 37);
	ExternalFixture.Document["buffers"][0]["uri"] = "external.bin";
	ExternalFixture.Document["images"][0].erase("bufferView");
	ExternalFixture.Document["images"][0].erase("mimeType");
	ExternalFixture.Document["images"][0]["uri"] = "external.bmp";
	WriteBytes(Root / "assets" / "external.bin", ExternalFixture.Binary);
	WriteBytes(Root / "assets" / "external.bmp", ExternalImage);
	WriteText(Root / "assets" / "external.gltf", ExternalFixture.Document.dump());
	auto DataFixture = MakeGltfFixture(44);
	DataFixture.Document["buffers"][0]["uri"] =
		"data:application/octet-stream;base64," + EncodeBase64(DataFixture.Binary);
	auto DataImage = MakeBmp(2, 2, 44);
	DataFixture.Document["images"][0].erase("bufferView");
	DataFixture.Document["images"][0]["uri"] = "data:image/bmp;base64," + EncodeBase64(DataImage);
	WriteText(Root / "assets" / "data.gltf", DataFixture.Document.dump());
	WriteText(Root / "assets" / "malformed.gltf", "{ this is not json");
	WriteText(Root / "assets" / "traversal.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4,"uri":"../secret.bin"}],"meshes":[]})");
	WriteText(Root / "assets" / "absolute.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4,"uri":"C:/secret.bin"}],"meshes":[]})");
	WriteText(Root / "assets" / "remote.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4,"uri":"https://example.invalid/a.bin"}],"meshes":[]})");
	WriteText(Root / "assets" / "malformed.glb", "not a glb");
	WriteBytes(Root / "assets" / "invalid-index.glb", MakeGlb(MakeGltfFixture(1, false, true)));
	WriteBytes(Root / "assets" / "nonfinite.glb", MakeGlb(MakeGltfFixture(1, false, false, true)));
	auto OutOfRangeAccessor = MakeGltfFixture(2);
	OutOfRangeAccessor.Document["accessors"][0]["count"] = 4;
	WriteBytes(Root / "assets" / "out-of-range-accessor.glb", MakeGlb(OutOfRangeAccessor));
	auto UnsupportedAttribute = MakeGltfFixture(3);
	UnsupportedAttribute.Document["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"] = 0;
	WriteBytes(Root / "assets" / "unsupported-attribute.glb", MakeGlb(UnsupportedAttribute));
	nlohmann::ordered_json LimitDocument{{"asset", {{"version", "2.0"}}},
		{"meshes", nlohmann::ordered_json::array()}};
	for (std::size_t Index = 0; Index <= AssetLimits::MaximumGltfMeshes; ++Index)
		LimitDocument["meshes"].push_back(nlohmann::ordered_json::object());
	WriteText(Root / "assets" / "limit.gltf", LimitDocument.dump());

	ObservingFilesystem Filesystem(Root);
	SourceMount Mount(Filesystem);
	auto World = std::make_shared<DataModel>();
	World->Root = Root;
	World->Filesystem = &Filesystem;
	auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
	auto Assets = GetAssets(World);
	Check(Assets && WorkspaceValue, "DataModel constructs the canonical AssetService and Workspace");

	auto ImageImport = Assets->ImportProjectAsset(Mount, "assets/checker.bmp", AssetKind::Image, "Checker");
	Check(ImageImport.Ok && ImageImport.Record && ImageImport.Record->State == AssetState::Ready &&
		ImageImport.Record->ContentRevision == 1 && ImageImport.Record->Asset &&
		std::holds_alternative<ImportedImage>(*ImageImport.Record->Asset),
		"bounded image import produces one canonical catalog record");
	const auto ImageReference = ImageImport.Record ? ImageImport.Record->Reference.Value : std::string{};
	const auto ImageId = ImageImport.Record ? ImageImport.Record->Id : AssetId{};
	const auto ImageContent = ImageImport.Record ? ImageImport.Record->ContentId : AssetContentId{};
	auto ImageResource = Assets->ResolveImage(ImageReference);
	Check(ImageResource && ImageResource->Width == 2 && ImageResource->Height == 2,
		"image resolution returns renderer-neutral dimensions and texture identity");
	auto InitialTextures = Assets->DrainTextureChanges();
	Check(!InitialTextures.Creates.empty() && InitialTextures.UploadBytes >= 16,
		"image import reaches the existing texture publication boundary");

	auto DuplicateContent = Assets->ImportProjectAsset(Mount, "assets/checker.bmp", AssetKind::Image, "Checker Copy");
	Check(DuplicateContent.Ok && DuplicateContent.Record && DuplicateContent.Record->Id != ImageId &&
		DuplicateContent.Record->ContentId == ImageContent,
		"stable logical identity remains distinct from deterministic content identity");
	const auto PersistentImageReference = DuplicateContent.Record->Reference.Value;
	auto PngImport = Assets->ImportProjectAsset(Mount, "assets/representative.png", AssetKind::Image, "Representative PNG");
	WriteBytes(Root / "assets" / "uniform.hdr", MakeHdr(4, 2, {128, 64, 32, 137}));
	auto HdrImport = Assets->ImportProjectAsset(Mount, "assets/uniform.hdr", AssetKind::Image, "HDR Panorama");
	Check(HdrImport.Ok && HdrImport.Records.size() == 6 && HdrImport.Record &&
		HdrImport.Record->LogicalKey == "positive-x", "HDR panorama creates six canonical Image assets");
	std::array<AssetId, 6> HdrIds{};
	if (HdrImport.Records.size() == 6) {
		for (std::size_t Index = 0; Index < HdrIds.size(); ++Index) {
			const auto &Record = HdrImport.Records[Index];
			HdrIds[Index] = Record.Id;
			const auto *Image = Record.Asset ? std::get_if<ImportedImage>(Record.Asset.get()) : nullptr;
			Check(Image && Image->Format == RenderTextureFormat::Rgba32Float && Image->Width == 1 &&
				Image->Height == 1 && Image->Pixels && Image->Pixels->size() == 16,
				"HDR cube face retains linear float pixels");
			if (Image && Image->Pixels && Image->Pixels->size() >= 4) {
				std::uint32_t Bits = 0;
				for (unsigned Byte = 0; Byte < 4; ++Byte) Bits |= static_cast<std::uint32_t>((*Image->Pixels)[Byte]) << (Byte * 8);
				Check(std::abs(std::bit_cast<float>(Bits) - 256.0f) < 0.01f,
					"HDR source radiance survives import without SDR clipping");
			}
		}
		const auto &Face = std::get<ImportedImage>(*HdrImport.Records[0].Asset);
		const auto Encoded = EncodeAssetArtifact(Face, AssetKind::Image);
		if (Encoded) {
			auto InvalidArtifact = std::vector<std::uint8_t>(**Encoded);
			const auto NanBits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
			for (unsigned Byte = 0; Byte < 4; ++Byte)
				InvalidArtifact[InvalidArtifact.size() - 16 + Byte] = static_cast<std::uint8_t>(NanBits >> (Byte * 8));
			const auto Rejected = DecodeAssetArtifact(InvalidArtifact, AssetKind::Image,
				AssetContentId::Hash(InvalidArtifact));
			Check(!Rejected && Rejected.error().Code == "InvalidRadiance",
				"version 7 image artifact rejects non-finite radiance after integrity validation");
			const auto ExtremeBits = std::bit_cast<std::uint32_t>(1000000.0f);
			for (unsigned Byte = 0; Byte < 4; ++Byte)
				InvalidArtifact[InvalidArtifact.size() - 16 + Byte] = static_cast<std::uint8_t>(ExtremeBits >> (Byte * 8));
			const auto Extreme = DecodeAssetArtifact(InvalidArtifact, AssetKind::Image,
				AssetContentId::Hash(InvalidArtifact));
			Check(Extreme && std::abs(ReadHdrChannel(std::get<ImportedImage>(Extreme->Asset), 0, 0) - 1000000.0f) < 1.0f,
				"canonical float artifact preserves finite radiance above the half-float GPU range");
		} else Check(false, "HDR face encodes as a canonical float image artifact");
	}
	const auto HdrChanges = Assets->DrainTextureChanges();
	Check(std::ranges::count_if(HdrChanges.Creates, [](const auto &Create) {
		return Create.Format == RenderTextureFormat::Rgba32Float;
	}) == 6, "HDR faces publish explicit float GPU texture format");
	WriteBytes(Root / "assets" / "gradient.hdr", MakeHdrGradient());
	const auto GradientImport = Assets->ImportProjectAsset(Mount, "assets/gradient.hdr", AssetKind::Image, "HDR Orientation");
	if (GradientImport.Ok && GradientImport.Records.size() == 6) {
		const auto &PositiveX = std::get<ImportedImage>(*GradientImport.Records[0].Asset);
		const auto &NegativeX = std::get<ImportedImage>(*GradientImport.Records[1].Asset);
		const auto &PositiveY = std::get<ImportedImage>(*GradientImport.Records[2].Asset);
		const auto &NegativeY = std::get<ImportedImage>(*GradientImport.Records[3].Asset);
		Check(ReadHdrChannel(PositiveX, 0, 0) > ReadHdrChannel(NegativeX, 0, 0) + 20.0f &&
			ReadHdrChannel(PositiveY, 0, 1) < ReadHdrChannel(NegativeY, 0, 1) - 20.0f,
			"equirectangular import follows the Sky +X and +Y face orientation");
	} else Check(false, "bounded HDR gradient imports as a six-face panorama");
	if (HdrImport.Record) {
		const auto Saved = Assets->CaptureProjectAssets();
		auto Restarted = std::make_shared<AssetService>();
		Restarted->LoadProjectAssetSnapshot(Saved);
		const auto Restored = Restarted->GetAsset(HdrImport.Record->Reference.Value);
		Check(Restored && Restored->Id == HdrImport.Record->Id && Restored->Asset &&
			std::get<ImportedImage>(*Restored->Asset).Format == RenderTextureFormat::Rgba32Float,
			"HDR artifact survives an asset-service restart with stable identity");
		WriteBytes(Root / "assets" / "uniform.hdr", MakeHdr(4, 2, {64, 32, 16, 137}));
		const auto Reimported = Assets->ReimportProjectAsset(Mount, HdrImport.Record->Reference.Value);
		Check(Reimported.Ok && Reimported.Records.size() == 6 &&
			std::ranges::all_of(Reimported.Records, [&](const auto &Record) {
				return std::ranges::find(HdrIds, Record.Id) != HdrIds.end() && Record.ContentRevision == 2;
			}), "HDR panorama reimport keeps all six derived identities");
	}
	WriteBytes(Root / "assets" / "invalid-radiance.hdr", MakeHdr(2, 2, {128, 0, 0, 255}));
	Check(!Assets->ImportProjectAsset(Mount, "assets/invalid-radiance.hdr", AssetKind::Image).Ok,
		"non-finite HDR radiance is rejected");
	WriteBytes(Root / "assets" / "extreme.hdr", MakeHdr(2, 2, {128, 0, 0, 145}));
	const auto ExtremeImport = Assets->ImportProjectAsset(Mount, "assets/extreme.hdr", AssetKind::Image);
	Check(ExtremeImport.Ok && ExtremeImport.Record && ExtremeImport.Record->Asset &&
		ReadHdrChannel(std::get<ImportedImage>(*ExtremeImport.Record->Asset), 0, 0) > 65504.0f,
		"finite HDR radiance above half-float range survives canonical source import");
	auto TruncatedHdr = MakeHdr(2, 2, {128, 0, 0, 129});
	TruncatedHdr.resize(TruncatedHdr.size() - 4);
	WriteBytes(Root / "assets" / "truncated.hdr", TruncatedHdr);
	Check(!Assets->ImportProjectAsset(Mount, "assets/truncated.hdr", AssetKind::Image).Ok,
		"truncated HDR pixel payload is rejected");
	WriteText(Root / "assets" / "oversized.hdr", "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1024 +X 2048\n");
	Check(!Assets->ImportProjectAsset(Mount, "assets/oversized.hdr", AssetKind::Image).Ok,
		"oversized HDR dimensions are rejected before pixel decode");
	(void)Assets->DrainTextureChanges();
	Check(PngImport.Ok && PngImport.Record && PngImport.Record->Asset &&
		std::get<ImportedImage>(*PngImport.Record->Asset).Width == 512,
		"representative PNG input passes header preflight and canonical RGBA decode");
	auto MalformedImage = Assets->ImportProjectAsset(Mount, "assets/bad.png", AssetKind::Image, "Bad Image");
	Check(!MalformedImage.Ok && MalformedImage.Diagnostic.Code == "MalformedImage",
		"malformed image input fails with a structured diagnostic");

	auto MeshImport = Assets->ImportProjectAsset(Mount, "assets/triangle.obj", AssetKind::Mesh, "Triangle");
	auto Mesh = MeshImport.Record ? Assets->ResolveMesh(MeshImport.Record->Reference.Value) : std::nullopt;
	auto ObjCollision = MeshImport.Record ? Assets->ResolveCollisionForMesh(MeshImport.Record->Reference.Value) : std::nullopt;
	Check(MeshImport.Ok && Mesh && Mesh->Vertices && Mesh->Vertices->size() == 3 &&
		Mesh->Indices && Mesh->Indices->size() == 3 && Mesh->SubmeshCount == 1 &&
		MeshImport.Records.size() == 2 && ObjCollision && ObjCollision->Value.Positions->size() == 3 &&
		ObjCollision->Value.Indices->size() == 3 && Mesh->CollisionAsset == ObjCollision->Id,
		"OBJ import atomically publishes render mesh and separate bounded collision triangles");
	const auto InitialCollisionStats = Assets->ReadCollisionStats();
	WriteText(Root / "assets" / "repeated.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
	auto RepeatedMesh = Assets->ImportProjectAsset(Mount, "assets/repeated.obj", AssetKind::Mesh, "Repeated");
	bool RepeatedReimportsOk = RepeatedMesh.Ok && RepeatedMesh.Record.has_value();
	auto CollisionConsumer = std::make_shared<MeshPart>();
	if (RepeatedMesh.Record) {
		CollisionConsumer->SetMesh(RepeatedMesh.Record->Reference.Value);
		CollisionConsumer->SetAnchored(true);
		CollisionConsumer->SetSize({1.0f, 1.0f, 1.0f});
		CollisionConsumer->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		CollisionConsumer->SetParent(WorkspaceValue);
		const auto PriorCollision = Assets->ResolveCollisionForMesh(RepeatedMesh.Record->Reference.Value);
		const auto VerifyInFlight = [&] {
			const auto Record = Assets->GetAsset(RepeatedMesh.Record->Reference.Value);
			const auto Collision = Assets->ResolveCollisionForMesh(RepeatedMesh.Record->Reference.Value);
			const auto Hit = WorldRootTestAccess::Raycast(*WorkspaceValue);
			const auto VisualMesh = Assets->ResolveMeshResource(RepeatedMesh.Record->Reference.Value);
			auto Reopened = std::make_shared<DataModel>();
			auto ReopenedAssets = GetAssets(Reopened);
			ReopenedAssets->LoadProjectAssetSnapshot(Assets->CaptureProjectAssets());
			const auto SavedCollision = ReopenedAssets->ResolveCollisionForMesh(RepeatedMesh.Record->Reference.Value);
			Check(Record && Record->State == AssetState::Importing && Collision && PriorCollision &&
				Collision->Id == PriorCollision->Id && Collision->ContentRevision == PriorCollision->ContentRevision &&
				Hit.Succeeded() && Hit.Instance == CollisionConsumer && std::abs(Hit.Distance - 2.0f) < 0.001f,
				"in-flight reimport keeps the complete previous triangle collision queryable");
			Check(VisualMesh && SavedCollision && PriorCollision && SavedCollision->Id == PriorCollision->Id &&
				SavedCollision->ContentRevision == PriorCollision->ContentRevision,
				"in-flight project capture and visual resolution retain the complete published bake");
		};
		Filesystem.BeforeRead = VerifyInFlight;
		Check(Assets->ReimportProjectAsset(Mount, RepeatedMesh.Record->Reference.Value).Ok && !Filesystem.BeforeRead,
			"successful reimport exercises the importing-state collision boundary");
		const auto UnchangedCollision = Assets->ResolveCollisionForMesh(RepeatedMesh.Record->Reference.Value);
		const auto UnchangedHit = WorldRootTestAccess::Raycast(*WorkspaceValue);
		Check(UnchangedCollision && PriorCollision &&
			UnchangedCollision->ContentRevision == PriorCollision->ContentRevision &&
			UnchangedCollision->Value.Positions == PriorCollision->Value.Positions &&
			UnchangedCollision->Value.Indices == PriorCollision->Value.Indices &&
			UnchangedHit.Instance == CollisionConsumer && !WorldRootTestAccess::IsDirty(*WorkspaceValue, *CollisionConsumer),
			"unchanged reimport retains canonical buffers and completes the existing native projection without retry");
		WriteText(Root / "assets" / "repeated.obj", "invalid mesh");
		Filesystem.BeforeRead = VerifyInFlight;
		const auto Rejected = Assets->ReimportProjectAsset(Mount, RepeatedMesh.Record->Reference.Value);
		const auto RetainedHit = WorldRootTestAccess::Raycast(*WorkspaceValue);
		Check(!Rejected.Ok && !Filesystem.BeforeRead && RetainedHit.Instance == CollisionConsumer &&
			std::abs(RetainedHit.Distance - 2.0f) < 0.001f,
			"rejected reimport preserves the previous collision rather than projecting a temporary box");
		WriteText(Root / "assets" / "repeated.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
	}
	if (RepeatedReimportsOk) for (std::uint32_t Iteration = 0; Iteration < 8; ++Iteration) {
		WriteText(Root / "assets" / "repeated.obj", "v 0 0 0\nv " + std::to_string(Iteration + 2) +
			" 0 0\nv 0 1 0\nf 1 2 3\n");
		RepeatedReimportsOk = Assets->ReimportProjectAsset(Mount, RepeatedMesh.Record->Reference.Value).Ok &&
			RepeatedReimportsOk;
	}
	const auto RepeatedStats = Assets->ReadCollisionStats();
	Check(RepeatedReimportsOk && RepeatedStats.ResidentRecords == InitialCollisionStats.ResidentRecords + 1 &&
		RepeatedStats.RetainedArtifactBytes < InitialCollisionStats.RetainedArtifactBytes + 1024 &&
		RepeatedStats.TotalRetainedArtifactBytes < InitialCollisionStats.TotalRetainedArtifactBytes + 2048 &&
		RepeatedStats.PendingMeshPublications == InitialCollisionStats.PendingMeshPublications + 1 &&
		RepeatedStats.PendingMeshRemovals == InitialCollisionStats.PendingMeshRemovals &&
		RepeatedStats.PendingMeshBytes < InitialCollisionStats.PendingMeshBytes + 1024 &&
		RepeatedStats.RetiredArtifacts == InitialCollisionStats.RetiredArtifacts + 7 && RepeatedStats.RetiredArtifactBytes > 0,
		"repeated collision replacement retains only the current bounded canonical artifact");
	if (RepeatedMesh.Record) {
		CollisionConsumer->Destroy();
		Check(Assets->DeleteProjectAsset(RepeatedMesh.Record->Reference.Value).Ok &&
			Assets->ReadCollisionStats().ResidentRecords == InitialCollisionStats.ResidentRecords &&
			Assets->ReadCollisionStats().PendingMeshPublications == InitialCollisionStats.PendingMeshPublications &&
			Assets->ReadCollisionStats().PendingMeshRemovals == InitialCollisionStats.PendingMeshRemovals,
			"deleting a mesh source group retires its collision record and artifact");
	}
	{
		auto ProjectionWorld = std::make_shared<DataModel>();
		auto ProjectionAssets = GetAssets(ProjectionWorld);
		auto ProjectionWorkspace = std::dynamic_pointer_cast<Workspace>(ProjectionWorld->GetService("Workspace"));
		WriteText(Root / "assets" / "projection.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
		const auto First = ProjectionAssets->ImportProjectAsset(Mount, "assets/projection.obj", AssetKind::Mesh);
		RenderPublisher Publisher;
		if (First.Record) for (int Revision = 2; Revision <= 3; ++Revision) {
			WriteText(Root / "assets" / "projection.obj", "v 0 0 0\nv " + std::to_string(Revision) +
				" 0 0\nv 0 1 0\nf 1 2 3\n");
			Check(ProjectionAssets->ReimportProjectAsset(Mount, First.Record->Reference.Value).Ok,
				"several mesh revisions can complete before the first renderer drain");
		}
		auto Initial = ProjectionAssets->DrainMeshChanges();
		Check(Initial.Creates.size() == 1 && Initial.Removes.empty(),
			"an unpublished mesh retains one final creation and no stale removals");
		Publisher.SetAssetMeshChanges(std::move(Initial.Creates), std::move(Initial.Removes));
		const auto FirstPublication = Publisher.Publish(*ProjectionWorkspace, RenderCameraInput{}, 320, 180);
		Check(FirstPublication->MeshCreates.size() == 1 && FirstPublication->MeshRemoves.empty(),
			"the latest undrained mesh revision publishes successfully to a fresh renderer");
		if (First.Record) {
			for (int Revision = 2; Revision <= 3; ++Revision) {
				WriteText(Root / "assets" / "projection.obj", "v 0 0 0\nv " + std::to_string(Revision) +
					" 0 0\nv 0 1 0\nf 1 2 3\n");
				Check(ProjectionAssets->ReimportProjectAsset(Mount, First.Record->Reference.Value).Ok,
					"multiple mesh revisions prepare before the next renderer drain");
			}
			const auto Queued = ProjectionAssets->ReadCollisionStats();
			auto Latest = ProjectionAssets->DrainMeshChanges();
			Check(Queued.PendingMeshPublications == 1 && Queued.PendingMeshRemovals == 1 &&
				Latest.Creates.size() == 1 && Latest.Removes.size() == 1,
				"mesh projection retains only the latest creation and the last delivered removal");
			Publisher.SetAssetMeshChanges(std::move(Latest.Creates), std::move(Latest.Removes));
			const auto Publication = Publisher.Publish(*ProjectionWorkspace, RenderCameraInput{}, 320, 180);
			Check(Publication->MeshCreates.size() == 1 && Publication->MeshRemoves.size() == 1,
				"coalesced mesh revisions publish without a stale intermediate resource removal");
		}
	}
	AssetCancellationToken CancelledCollision;
	CancelledCollision.Cancel();
	auto CancelledMesh = Assets->ImportProjectAsset(Mount, "assets/triangle.obj", AssetKind::Mesh,
		"Cancelled Mesh", CancelledCollision);
	Check(!CancelledMesh.Ok && CancelledMesh.Records.empty() && CancelledMesh.Diagnostic.Code == "Cancelled",
		"cancelled mesh import publishes neither mesh nor collision child");
	{
		// Exercise the compiler through the actual compound transaction, not only
		// its candidate API. Runtime receives the bake without its source graph.
		auto SolidWorld = std::make_shared<DataModel>();
		auto SolidAssets = GetAssets(SolidWorld);
		nlohmann::ordered_json SolidSource = {
			{"Version", 1}, {"Root", "cut"}, {"Nodes", nlohmann::ordered_json::array({
				{{"Id", "box"}, {"Kind", "Box"}, {"Size", {2, 2, 2}}},
				{{"Id", "sphere"}, {"Kind", "Sphere"}, {"Radius", 0.5}},
				{{"Id", "cut"}, {"Kind", "Subtraction"}, {"Inputs", {"box", "sphere"}}},
			})},
		};
		WriteText(Root / "assets" / "cut.solid", SolidSource.dump());
		const auto First = SolidAssets->ImportProjectAsset(Mount, "assets/cut.solid", AssetKind::Mesh, "Cut");
		const auto FirstCollision = First.Record ? SolidAssets->ResolveCollisionForMesh(First.Record->Reference.Value) : std::nullopt;
		Check(First.Ok && First.Record && First.Records.size() == 2 && FirstCollision,
			"solid compilation atomically publishes an ordinary mesh and collision pair");
		if (First.Record && FirstCollision) {
			const auto SavedFirst = SolidAssets->CaptureProjectAssets();
			SolidSource["Nodes"][0]["Size"][0] = 3;
			WriteText(Root / "assets" / "cut.solid", SolidSource.dump());
			const auto Second = SolidAssets->ReimportProjectAsset(Mount, First.Record->Reference.Value);
			const auto SecondCollision = SolidAssets->ResolveCollisionForMesh(First.Record->Reference.Value);
			Check(Second.Ok && Second.Record && SecondCollision && Second.Record->Id == First.Record->Id &&
				SecondCollision->Id == FirstCollision->Id && Second.Record->ContentId != First.Record->ContentId &&
				SecondCollision->ContentRevision > FirstCollision->ContentRevision,
				"compatible solid recompilation retains both logical identities and advances changed revisions");
			WriteText(Root / "assets" / "cut.solid", "{invalid source}");
			const auto Failed = SolidAssets->ReimportProjectAsset(Mount, First.Record->Reference.Value);
			Check(!Failed.Ok && Failed.Record && Second.Record && Failed.Record->State == AssetState::Stale &&
				Failed.Record->ContentId == Second.Record->ContentId &&
				SolidAssets->ResolveCollisionForMesh(First.Record->Reference.Value).has_value(),
				"failed solid recompilation preserves the complete last-known-good bake");
			auto RuntimeWorld = std::make_shared<DataModel>();
			auto RuntimeAssets = GetAssets(RuntimeWorld);
			RuntimeAssets->LoadRuntimeAssetSnapshot(SolidAssets->CaptureRuntimeAssets());
			Check(RuntimeAssets->ResolveMesh(First.Record->Reference.Value).has_value() &&
				RuntimeAssets->ResolveCollisionForMesh(First.Record->Reference.Value).has_value(),
				"packaged solid runtime resolves compiled artifacts without source or Boolean evaluation");
			auto RuntimeWorkspace = std::dynamic_pointer_cast<Workspace>(RuntimeWorld->GetService("Workspace"));
			auto RuntimePart = std::make_shared<MeshPart>();
			RuntimePart->SetAnchored(true);
			RuntimePart->SetSize({1.0f, 1.0f, 1.0f});
			RuntimePart->SetMesh(First.Record->Reference.Value);
			RuntimePart->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
			RuntimePart->SetParent(RuntimeWorkspace);
			const auto RuntimeHit = WorldRootTestAccess::Raycast(*RuntimeWorkspace);
			Check(RuntimeHit.Succeeded() && RuntimeHit.Instance == RuntimePart &&
				std::abs(RuntimeHit.Distance - 1.0f) < 0.001f,
				"a packaged Boolean bake creates queryable triangle collision without its source graph");
			auto OldWorld = std::make_shared<DataModel>();
			auto OldAssets = GetAssets(OldWorld);
			OldAssets->LoadProjectAssetSnapshot(SavedFirst);
			const auto OldMesh = OldAssets->GetAsset(First.Record->Reference.Value);
			const auto OldCollision = OldAssets->ResolveCollisionForMesh(First.Record->Reference.Value);
			Check(OldMesh && OldCollision && OldMesh->ContentId == First.Record->ContentId &&
				OldCollision->ContentRevision == FirstCollision->ContentRevision,
				"captured old revisions remain independently reopenable after service artifact retirement");
		}
	}
	{
		auto PackageWorld = std::make_shared<DataModel>();
		auto PackageAssets = GetAssets(PackageWorld);
		auto PackageMesh = PackageAssets->ImportProjectAsset(Mount, "assets/triangle.obj", AssetKind::Mesh,
			"Packaged Mesh");
		if (PackageMesh.Ok && PackageMesh.Record) {
			auto RuntimeSnapshot = PackageAssets->CaptureRuntimeAssets();
			auto RuntimeWorld = std::make_shared<DataModel>();
			auto RuntimeAssets = GetAssets(RuntimeWorld);
			RuntimeAssets->LoadRuntimeAssetSnapshot(RuntimeSnapshot);
			auto RuntimeCollision = RuntimeAssets->ResolveCollisionForMesh(PackageMesh.Record->Reference.Value);
			Check(RuntimeCollision && RuntimeCollision->Id ==
				PackageAssets->ResolveCollisionForMesh(PackageMesh.Record->Reference.Value)->Id &&
				RuntimeCollision->ContentRevision == 1,
				"packaged runtime resolves explicit mesh collision dependency without source grouping");
			RuntimeAssets.reset(); RuntimeWorld->Destroy();
		} else Check(false, "package collision fixture imports through the shared asset catalog");
		PackageAssets.reset(); PackageWorld->Destroy();
	}
	auto InvalidIndex = Assets->ImportProjectAsset(Mount, "assets/bad-index.obj", AssetKind::Mesh, "Bad Index");
	auto NonFinite = Assets->ImportProjectAsset(Mount, "assets/non-finite.obj", AssetKind::Mesh, "Non-finite");
	auto Thin = Assets->ImportProjectAsset(Mount, "assets/thin.obj", AssetKind::Mesh, "Thin Collision");
	auto Extreme = Assets->ImportProjectAsset(Mount, "assets/extreme.obj", AssetKind::Mesh, "Extreme Collision");
	Check(!InvalidIndex.Ok && InvalidIndex.Diagnostic.Code == "InvalidMeshIndex" &&
		!NonFinite.Ok && NonFinite.Diagnostic.Code == "MalformedMesh" &&
		!Thin.Ok && Thin.Diagnostic.Code == "MalformedCollision" && Thin.Records.empty() &&
		!Extreme.Ok && Extreme.Diagnostic.Code == "MalformedCollision" && Extreme.Records.empty(),
		"OBJ import atomically rejects invalid, non-finite, thin, and extreme collision geometry");
	auto MeshChanges = Assets->DrainMeshChanges();
	RenderPublisher Publisher;
	Publisher.SetUiTextureChanges(
		std::move(InitialTextures.Creates), std::move(InitialTextures.Updates), std::move(InitialTextures.Removes)
	);
	Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
	auto Publication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	Check(!Publication->TextureCreates.empty() && !Publication->MeshCreates.empty(),
		"imported images and meshes are published through RenderPublication");
	RenderProjection Projection;
	auto Applied = Projection.Apply(*Publication);
	Check(Applied.MeshesCreated == Publication->MeshCreates.size(),
		"headless RenderProjection accepts imported mesh residency");
	Publisher.RequestFullResync();
	auto Resynchronized = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	Check(Resynchronized->FullResync && !Resynchronized->TextureCreates.empty() &&
		!Resynchronized->MeshCreates.empty(),
		"renderer restart/full resync recreates disposable image and mesh residency");

	auto GltfImport = Assets->ImportProjectAsset(Mount, "assets/compound.glb", AssetKind::Mesh, "Compound GLB");
	Check(GltfImport.Ok && GltfImport.Record && GltfImport.Records.size() == 5,
		"one GLB transaction atomically generates mesh, collision, materials, and image");
	auto FindGenerated = [&](AssetKind Kind, std::string_view Name) -> std::optional<AssetRecord> {
		for (const auto &Record : GltfImport.Records)
			if (Record.Kind == Kind && Record.Name == Name) return Record;
		return std::nullopt;
	};
	auto GltfMeshRecord = std::optional<AssetRecord>{};
	for (const auto &Record : GltfImport.Records) if (Record.Kind == AssetKind::Mesh) {
		GltfMeshRecord = Record;
		break;
	}
	auto TexturedMaterialRecord = FindGenerated(AssetKind::Material, "Textured");
	auto TranslucentMaterialRecord = FindGenerated(AssetKind::Material, "Translucent");
	auto GltfImageRecord = FindGenerated(AssetKind::Image, "Surface");
	auto GltfMesh = GltfMeshRecord ? Assets->ResolveMeshResource(GltfMeshRecord->Reference.Value) : std::nullopt;
	auto GltfCollision = GltfMeshRecord ? Assets->ResolveCollisionForMesh(GltfMeshRecord->Reference.Value) : std::nullopt;
	auto GltfMaterial = TexturedMaterialRecord ? Assets->ResolveMaterial(TexturedMaterialRecord->Reference.Value) : std::nullopt;
	Check(GltfMesh && GltfMesh->Value.Vertices && GltfMesh->Value.Vertices->size() == 6 &&
		GltfMesh->Value.Primitives && GltfMesh->Value.Primitives->size() == 2 &&
		(*GltfMesh->Value.Primitives)[0].Material == TexturedMaterialRecord->Id &&
		(*GltfMesh->Value.Primitives)[1].Material == TranslucentMaterialRecord->Id,
		"glTF conversion preserves canonical vertices, UVs, tangents, winding, and primitive material slots");
	Check(GltfMaterial && GltfMaterial->Value.BaseColorTexture == GltfImageRecord->Id &&
		GltfMaterial->Value.NormalTexture == GltfImageRecord->Id &&
		GltfMaterial->Value.MetallicRoughnessTexture == GltfImageRecord->Id &&
		GltfMaterial->Value.MetallicFactor == 0.25f && GltfMaterial->Value.RoughnessFactor == 0.75f &&
		GltfMaterial->Value.AlphaMode == AssetMaterialAlphaMode::Mask && GltfMaterial->Value.DoubleSided &&
		TexturedMaterialRecord->Dependencies.size() == 1 && GltfMeshRecord->Dependencies.size() == 3 &&
		GltfCollision && GltfMesh->Value.CollisionAsset == GltfCollision->Id &&
		GltfCollision->ContentRevision == 1 && GltfCollision->CanonicalBytes > 0,
		"glTF metallic-roughness material semantics and deduplicated dependency edges are durable");
	if (GltfMesh && GltfMesh->Value.Vertices) {
		const auto &Vertex = GltfMesh->Value.Vertices->at(2);
		Check(Vertex.Position.z == 0.0f && std::isfinite(Vertex.Tangent.x) && std::abs(Vertex.Tangent.w) == 1.0f &&
			GltfMesh->Value.Indices->at(1) == 2,
			"glTF right-handed +Z-forward data is converted once to Gargantuan left-handed +Y-up/-Z-forward winding");
	}
	Check(Assets->GetAsset("builtin://material/default").has_value() && Assets->ResolveMaterial("").has_value(),
		"the deterministic built-in material resolves through AssetService");

	auto MeshPartA = std::make_shared<MeshPart>();
	auto MeshPartB = std::make_shared<MeshPart>();
	CheckThrows<std::invalid_argument>([&] { MeshPartA->SetMesh("assets/compound.glb"); },
		"MeshPart rejects raw mesh source paths");
	MeshPartA->SetName("Imported Mesh A"); MeshPartB->SetName("Imported Mesh B");
	MeshPartA->SetMesh(GltfMeshRecord->Reference.Value); MeshPartB->SetMesh(GltfMeshRecord->Reference.Value);
	MeshPartA->SetAnchored(true);
	MeshPartA->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
	MeshPartA->SetParent(WorkspaceValue); MeshPartB->SetParent(WorkspaceValue);
	auto CompoundTextures = Assets->DrainTextureChanges();
	auto CompoundMeshes = Assets->DrainMeshChanges();
	RenderPublisher CompoundPublisher;
	CompoundPublisher.SetUiTextureChanges(std::move(CompoundTextures.Creates),
		std::move(CompoundTextures.Updates), std::move(CompoundTextures.Removes));
	CompoundPublisher.SetAssetMeshChanges(std::move(CompoundMeshes.Creates), std::move(CompoundMeshes.Removes));
	auto CompoundPublication = CompoundPublisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	auto MeshCreates = std::vector<const RenderObjectCreate *>{};
	for (const auto &Create : CompoundPublication->Creates)
		if (Create.Item.Object == MeshPartA->GetObjectId() || Create.Item.Object == MeshPartB->GetObjectId())
			MeshCreates.push_back(&Create);
	Check(MeshCreates.size() == 2 && MeshCreates[0]->Mesh == MeshCreates[1]->Mesh &&
		MeshCreates[0]->Primitives && MeshCreates[0]->Primitives->size() == 2 &&
		MeshCreates[0]->Primitives->at(0).Material.BaseColorTexture.has_value() &&
		MeshCreates[0]->Primitives->at(0).Material.MetallicRoughnessTexture ==
			MeshCreates[0]->Primitives->at(0).Material.BaseColorTexture,
		"two MeshParts share one mesh residency while publishing per-primitive material and texture state");
	RenderProjection CompoundProjection;
	Check(CompoundProjection.Apply(*CompoundPublication).Created >= 2,
		"headless projection validates the compound MeshPart resource graph");
	CompoundPublisher.RequestFullResync();
	auto MeshPartResync = CompoundPublisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	const auto ResyncedMeshParts = std::ranges::count_if(MeshPartResync->Creates, [&](const RenderObjectCreate &Create) {
		return Create.Item.Object == MeshPartA->GetObjectId() || Create.Item.Object == MeshPartB->GetObjectId();
	});
	Check(MeshPartResync->FullResync && ResyncedMeshParts == 2 && MeshPartResync->MeshCreates.size() == 1,
		"renderer restart recreates both MeshParts while retaining one shared mesh residency");
	(void)CompoundProjection.Apply(*MeshPartResync);
	MeshPartA->SetArchivable(true);
	std::shared_ptr<MeshPart> DuplicatedMeshPart;
	try { DuplicatedMeshPart = std::dynamic_pointer_cast<MeshPart>(MeshPartA->Clone()); }
	catch (const std::exception &Failure) {
		std::cerr << "FAIL: MeshPart clone raised: " << Failure.what() << '\n';
		++Failures;
	}
	Check(DuplicatedMeshPart && DuplicatedMeshPart->GetMesh() == MeshPartA->GetMesh() &&
		DuplicatedMeshPart->GetMaterial() == MeshPartA->GetMaterial() &&
		DuplicatedMeshPart->GetCollisionMode() == Enums::MeshCollisionMode::StaticMesh,
		"MeshPart duplication preserves strict mesh and material references");
	if (DuplicatedMeshPart) {
		DuplicatedMeshPart->SetParent(WorkspaceValue);
		DuplicatedMeshPart->Destroy();
	}
	DuplicatedMeshPart.reset();
	Check(Assets->GetAsset(GltfMeshRecord->Reference.Value).has_value(),
		"destroying a MeshPart releases only object ownership and never deletes its shared asset");
	auto MissingMeshPart = std::make_shared<MeshPart>();
	MissingMeshPart->SetMesh("asset://ffffffffffffffffffffffffffffffff");
	MissingMeshPart->SetParent(WorkspaceValue);
	auto MissingPublication = CompoundPublisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	Check(std::ranges::none_of(MissingPublication->Creates, [&](const RenderObjectCreate &Create) {
		return Create.Item.Object == MissingMeshPart->GetObjectId();
	}), "a syntactically valid missing Mesh asset produces deterministic no-draw behavior");
	(void)CompoundProjection.Apply(*MissingPublication);
	MissingMeshPart->Destroy();
	MissingMeshPart.reset();

	std::unordered_map<std::string, AssetId> StableIds;
	std::unordered_map<std::string, std::uint64_t> PreviousRevisions;
	for (const auto &Record : GltfImport.Records) {
		StableIds.emplace(Record.LogicalKey, Record.Id);
		PreviousRevisions.emplace(Record.LogicalKey, Record.ContentRevision);
	}
	CompoundFixture = MakeGltfFixture(92);
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(CompoundFixture));
	auto GltfReimport = Assets->ReimportProjectAsset(Mount, TexturedMaterialRecord->Reference.Value);
	Check(GltfReimport.Ok && GltfReimport.Records.size() == 5 && std::ranges::all_of(GltfReimport.Records,
		[&](const AssetRecord &Record) { return StableIds.at(Record.LogicalKey) == Record.Id; }),
		"compound reimport through a generated child preserves every semantic AssetId");
	auto ReimportedImage = std::ranges::find(GltfReimport.Records, GltfImageRecord->Id, &AssetRecord::Id);
	auto ReimportedTextured = std::ranges::find(GltfReimport.Records, TexturedMaterialRecord->Id, &AssetRecord::Id);
	auto ReimportedTranslucent = std::ranges::find(GltfReimport.Records, TranslucentMaterialRecord->Id, &AssetRecord::Id);
	auto ReimportedMesh = std::ranges::find(GltfReimport.Records, GltfMeshRecord->Id, &AssetRecord::Id);
	Check(ReimportedImage != GltfReimport.Records.end() && ReimportedTextured != GltfReimport.Records.end() &&
		ReimportedMesh != GltfReimport.Records.end() && ReimportedTranslucent != GltfReimport.Records.end() &&
		ReimportedImage->ContentRevision == 2 && ReimportedTextured->ContentRevision == 2 &&
		ReimportedMesh->ContentRevision == 2 && ReimportedTranslucent->ContentRevision == 1 &&
		MeshImport.Record->ContentRevision == Assets->GetAsset(MeshImport.Record->Reference.Value)->ContentRevision,
		"one changed image propagates revisions only through its material and mesh dependency chain");
	auto ReimportTextures = Assets->DrainTextureChanges();
	auto ReimportMeshes = Assets->DrainMeshChanges();
	CompoundPublisher.SetUiTextureChanges(std::move(ReimportTextures.Creates),
		std::move(ReimportTextures.Updates), std::move(ReimportTextures.Removes));
	if (!ReimportMeshes.Creates.empty() || !ReimportMeshes.Removes.empty())
		CompoundPublisher.SetAssetMeshChanges(std::move(ReimportMeshes.Creates), std::move(ReimportMeshes.Removes));
	auto SelectivePublication = CompoundPublisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	Check(SelectivePublication->Updates.size() == 2 && SelectivePublication->Creates.empty() &&
		SelectivePublication->MeshCreates.empty() && SelectivePublication->TextureUpdates.size() == 1,
		"texture dependency reimport updates only the two affected MeshParts and the shared texture residency");
	(void)CompoundProjection.Apply(*SelectivePublication);

	WriteText(Root / "assets" / "compound.glb", "corrupt reimport");
	auto CorruptGraph = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	Check(!CorruptGraph.Ok && CorruptGraph.Record && CorruptGraph.Record->State == AssetState::Stale &&
		CorruptGraph.Record->ContentRevision == 2 && Assets->ResolveMeshResource(GltfMeshRecord->Reference.Value) &&
		Assets->ResolveCollisionForMesh(GltfMeshRecord->Reference.Value) &&
		Assets->ResolveCollisionForMesh(GltfMeshRecord->Reference.Value)->Id == GltfCollision->Id,
		"a corrupt compound reimport retains the complete last-known-good mesh and collision graph");
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(CompoundFixture));

	auto WithOrphanFixture = MakeGltfFixture(92, true);
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(WithOrphanFixture));
	auto AddedChild = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	auto Orphan = std::ranges::find(AddedChild.Records, std::string("Orphan"), &AssetRecord::Name);
	Check(AddedChild.Ok && AddedChild.Records.size() == 6 && Orphan != AddedChild.Records.end() &&
		std::ranges::all_of(AddedChild.Records, [&](const AssetRecord &Record) {
			const auto Existing = StableIds.find(Record.LogicalKey);
			return Existing == StableIds.end() || Existing->second == Record.Id;
		}), "compound reimport allocates only the newly added logical child's AssetId");
	MeshPartA->SetMaterial(Orphan->Reference.Value);
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(CompoundFixture));
	auto ReferencedRemoval = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	Check(!ReferencedRemoval.Ok && ReferencedRemoval.Diagnostic.Code == "AssetReferenced" &&
		Assets->GetAsset(Orphan->Reference.Value).has_value(),
		"compound reimport atomically rejects removal of a generated child referenced by the scene");
	MeshPartA->SetMaterial("");
	auto RemovedChild = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	Check(RemovedChild.Ok && RemovedChild.Records.size() == 5 && !Assets->GetAsset(Orphan->Reference.Value),
		"an unreferenced disappearing logical child is retired atomically");
	Check(!Assets->DeleteProjectAsset(GltfMeshRecord->Reference.Value).Ok,
		"compound deletion rejects live MeshPart references rather than leaving dangling material graphs");

	auto ExternalImport = Assets->ImportProjectAsset(Mount, "assets/external.gltf", AssetKind::Mesh, "External glTF");
	auto DataImport = Assets->ImportProjectAsset(Mount, "assets/data.gltf", AssetKind::Mesh, "Data glTF");
	Check(ExternalImport.Ok && DataImport.Ok,
		".gltf relative resources and bounded base64 data resources import through the controlled source boundary");
	const auto CatalogBeforeInvalidGraph = Assets->GetCatalog(false).size();
	auto InvalidGltfIndex = Assets->ImportProjectAsset(Mount, "assets/invalid-index.glb", AssetKind::Mesh, "Invalid Index GLB");
	auto NonFiniteGltf = Assets->ImportProjectAsset(Mount, "assets/nonfinite.glb", AssetKind::Mesh, "Non-finite GLB");
	auto OutOfRangeGltf = Assets->ImportProjectAsset(Mount, "assets/out-of-range-accessor.glb", AssetKind::Mesh, "Out-of-range accessor GLB");
	auto UnsupportedAttributeGltf = Assets->ImportProjectAsset(Mount, "assets/unsupported-attribute.glb", AssetKind::Mesh, "Unsupported attribute GLB");
	auto LimitGltf = Assets->ImportProjectAsset(Mount, "assets/limit.gltf", AssetKind::Mesh, "Oversized glTF");
	Check(!InvalidGltfIndex.Ok && InvalidGltfIndex.Diagnostic.Code == "InvalidMeshIndex" &&
		!NonFiniteGltf.Ok && NonFiniteGltf.Diagnostic.Code == "NonFiniteGltf" &&
		!OutOfRangeGltf.Ok && OutOfRangeGltf.Diagnostic.Code == "AccessorOutOfRange" &&
		!UnsupportedAttributeGltf.Ok && UnsupportedAttributeGltf.Diagnostic.Code == "UnsupportedGltfFeature" &&
		!LimitGltf.Ok && LimitGltf.Diagnostic.Code == "GltfLimit" &&
		Assets->GetCatalog(false).size() == CatalogBeforeInvalidGraph,
		"accessor, semantic, finite-value, index, and limit failures commit no partial compound catalog records");
	auto MalformedGltf = Assets->ImportProjectAsset(Mount, "assets/malformed.gltf", AssetKind::Mesh, "Malformed glTF");
	auto MalformedGlb = Assets->ImportProjectAsset(Mount, "assets/malformed.glb", AssetKind::Mesh, "Malformed GLB");
	auto TraversalGltf = Assets->ImportProjectAsset(Mount, "assets/traversal.gltf", AssetKind::Mesh, "Traversal glTF");
	auto AbsoluteGltf = Assets->ImportProjectAsset(Mount, "assets/absolute.gltf", AssetKind::Mesh, "Absolute glTF");
	auto RemoteGltf = Assets->ImportProjectAsset(Mount, "assets/remote.gltf", AssetKind::Mesh, "Remote glTF");
	Check(!MalformedGltf.Ok && MalformedGltf.Diagnostic.Code == "MalformedGltf" &&
		!MalformedGlb.Ok && MalformedGlb.Diagnostic.Code == "MalformedGlb" &&
		!TraversalGltf.Ok && TraversalGltf.Diagnostic.Code == "PathEscape" &&
		!AbsoluteGltf.Ok && AbsoluteGltf.Diagnostic.Code == "ExternalUriRejected" &&
		!RemoteGltf.Ok && RemoteGltf.Diagnostic.Code == "ExternalUriRejected",
		"malformed containers and traversal, absolute, and remote glTF URIs fail with structured diagnostics");

	auto AdvancedFixture = MakeGltfFixture(92);
	AdvancedFixture.Document["samplers"] =
		nlohmann::ordered_json::array({{{"minFilter", 9986}, {"magFilter", 9729}, {"wrapS", 33648}, {"wrapT", 33071}}});
	AdvancedFixture.Document["textures"].push_back({{"source", 0}, {"sampler", 0}});
	auto &AdvancedSource = AdvancedFixture.Document["materials"][0];
	AdvancedSource["normalTexture"] = {{"index", 1}, {"scale", -0.5}};
	AdvancedSource["emissiveFactor"] = {0.125, 0.25, 0.5};
	AdvancedSource["emissiveTexture"] = {{"index", 0}};
	AdvancedSource["occlusionTexture"] = {{"index", 1}, {"strength", 0.375}};
	const RenderSamplerState ExplicitSampler{RenderTextureMinFilter::NearestMipmapLinear, RenderTextureMagFilter::Linear,
		RenderTextureWrap::MirroredRepeat, RenderTextureWrap::ClampToEdge};
	const auto NormalSlot = static_cast<std::size_t>(RenderMaterialTextureSlot::Normal);
	const auto OcclusionSlot = static_cast<std::size_t>(RenderMaterialTextureSlot::Occlusion);
	const auto ImageBeforeAdvanced = Assets->ResolveImage(GltfImageRecord->Reference.Value);
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(AdvancedFixture));
	const auto AdvancedImport = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	const auto AdvancedMaterial = Assets->ResolveMaterial(TexturedMaterialRecord->Reference.Value);
	const auto ImageAfterAdvanced = Assets->ResolveImage(GltfImageRecord->Reference.Value);
	Check(AdvancedImport.Ok && AdvancedImport.Records.size() == 5 && AdvancedMaterial && ImageBeforeAdvanced &&
		ImageAfterAdvanced && ImageAfterAdvanced->Texture == ImageBeforeAdvanced->Texture &&
		ImageAfterAdvanced->ContentRevision == ImageBeforeAdvanced->ContentRevision &&
		std::ranges::all_of(AdvancedImport.Records, [&](const AssetRecord &Record) {
			return StableIds.at(Record.LogicalKey) == Record.Id;
		}) && AdvancedMaterial->Value.EmissiveTexture == GltfImageRecord->Id &&
		AdvancedMaterial->Value.OcclusionTexture == GltfImageRecord->Id &&
		AdvancedMaterial->Value.EmissiveFactor == glm::vec3(0.125f, 0.25f, 0.5f) &&
		AdvancedMaterial->Value.OcclusionStrength == 0.375f && AdvancedMaterial->Value.NormalScale == -0.5f &&
		AdvancedMaterial->Value.TextureSamplers[NormalSlot] == ExplicitSampler &&
		AdvancedMaterial->Value.TextureSamplers[OcclusionSlot] == ExplicitSampler &&
		AdvancedMaterial->Value.TextureSamplers[0] == RenderSamplerState{} &&
		Assets->GetAsset(TexturedMaterialRecord->Reference.Value)->Dependencies == std::vector{GltfImageRecord->Id},
		"advanced glTF roles and samplers preserve deduplicated image IDs, residency, revision, and dependencies");

	CompoundPublisher.RequestFullResync();
	auto AdvancedPublication = CompoundPublisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 180);
	Check(std::ranges::any_of(AdvancedPublication->Creates, [&](const auto &Create) {
		if (Create.Item.Object != MeshPartA->GetObjectId() || !Create.Primitives || !ImageAfterAdvanced) return false;
		const auto &Material = Create.Primitives->front().Material;
		return Material.EmissiveTexture == ImageAfterAdvanced->Texture && Material.OcclusionTexture == ImageAfterAdvanced->Texture &&
			Material.EmissiveFactor == glm::vec3(.125f, .25f, .5f) && Material.OcclusionStrength == .375f &&
			Material.NormalScale == -.5f && Material.TextureSamplers[OcclusionSlot] == ExplicitSampler;
	}), "full RenderPublication carries advanced material state without separate image residency");
	(void)CompoundProjection.Apply(*AdvancedPublication);

	auto InvalidAdvanced = AdvancedFixture;
	InvalidAdvanced.Document["samplers"][0]["magFilter"] = 9987;
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(InvalidAdvanced));
	const auto InvalidAdvancedReimport = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	const auto StaleAdvanced = Assets->GetAsset(TexturedMaterialRecord->Reference.Value);
	Check(!InvalidAdvancedReimport.Ok && InvalidAdvancedReimport.Diagnostic.Code == "InvalidSampler" &&
		StaleAdvanced && StaleAdvanced->State == AssetState::Stale && AdvancedMaterial &&
		StaleAdvanced->ContentRevision == AdvancedMaterial->ContentRevision,
		"bad sampler reimport preserves last-known-good advanced material");
	InvalidAdvanced = AdvancedFixture;
	InvalidAdvanced.Document["materials"][0]["occlusionTexture"]["index"] = 99;
	WriteBytes(Root / "assets" / "invalid-occlusion.glb", MakeGlb(InvalidAdvanced));
	const auto BeforeInvalidOcclusion = Assets->GetCatalog(false).size();
	const auto InvalidOcclusion = Assets->ImportProjectAsset(Mount, "assets/invalid-occlusion.glb", AssetKind::Mesh);
	Check(!InvalidOcclusion.Ok && InvalidOcclusion.Diagnostic.Code == "InvalidTexture" &&
		Assets->GetCatalog(false).size() == BeforeInvalidOcclusion,
		"bad occlusion reference creates no partial source group");
	WriteBytes(Root / "assets" / "compound.glb", MakeGlb(AdvancedFixture));
	const auto RetriedAdvanced = Assets->ReimportProjectAsset(Mount, GltfMeshRecord->Reference.Value);
	const auto RetriedMaterial = Assets->ResolveMaterial(TexturedMaterialRecord->Reference.Value);
	Check(RetriedAdvanced.Ok && AdvancedMaterial && RetriedMaterial &&
		RetriedMaterial->ContentRevision == AdvancedMaterial->ContentRevision,
		"identical v6 reimport restores availability without changing content revision");

	auto FontImport = Assets->ImportProjectAsset(Mount, "assets/font.ttf", AssetKind::Font, "Project Font");
	auto Font = FontImport.Record ? Assets->ResolveFont(FontImport.Record->Reference.Value) : std::nullopt;
	Check(FontImport.Ok && Font && Font->Bytes && !Font->Bytes->empty() && Font->FaceCount > 0,
		"font import validates usable in-memory font bytes");
	auto MalformedFont = Assets->ImportProjectAsset(Mount, "assets/bad.ttf", AssetKind::Font, "Bad Font");
	Check(!MalformedFont.Ok && MalformedFont.Diagnostic.Code == "FontValidation",
		"malformed font input fails bounded validation");

	auto ImageObject = std::make_shared<ImageLabel>();
	auto TextObject = std::make_shared<TextLabel>();
	CheckThrows<std::invalid_argument>([&] { ImageObject->SetImage("assets/checker.bmp"); },
		"ImageLabel rejects raw source paths");
	CheckThrows<std::invalid_argument>([&] { TextObject->SetFontFace("Project Font"); },
		"TextLabel rejects display-name font lookups");
	ImageObject->SetImage(ImageReference);
	TextObject->SetFontFace(FontImport.Record->Reference.Value);
	ImageObject->SetParent(WorkspaceValue);
	TextObject->SetParent(WorkspaceValue);
	Check(!Assets->DeleteProjectAsset(ImageReference).Ok,
		"asset deletion rejects live ImageLabel references");
	ImageObject->SetImage("");
	Check(Assets->DeleteProjectAsset(ImageReference).Ok,
		"unreferenced project assets can be deleted deterministically");
	Check(!Assets->DrainTextureChanges().Removes.empty(),
		"image deletion publishes texture disposal");

	WriteText(Root / "assets" / "checker.bmp", "not a bitmap");
	auto FailedReimport = Assets->ReimportProjectAsset(Mount, PersistentImageReference);
	Check(!FailedReimport.Ok && FailedReimport.Record && FailedReimport.Record->Id == DuplicateContent.Record->Id &&
		FailedReimport.Record->ContentRevision == 1 && FailedReimport.Record->State == AssetState::Stale &&
		Assets->ResolveImage(PersistentImageReference).has_value(),
		"failed reimport preserves stable identity and last-known-good content");
	WriteBytes(Root / "assets" / "checker.bmp", MakeBmp(2, 2, 29));
	auto SuccessfulReimport = Assets->ReimportProjectAsset(Mount, PersistentImageReference);
	Check(SuccessfulReimport.Ok && SuccessfulReimport.Record && SuccessfulReimport.Record->Id == DuplicateContent.Record->Id &&
		SuccessfulReimport.Record->ContentRevision == 2 && SuccessfulReimport.Record->ContentId != ImageContent &&
		!Assets->DrainTextureChanges().Updates.empty(),
		"successful reimport advances content revision without changing logical identity");

	AssetCancellationToken Cancelled;
	Cancelled.Cancel();
	auto CancelledImport = Assets->ImportProjectAsset(Mount, "assets/checker.bmp", AssetKind::Image, "Cancelled", Cancelled);
	Check(!CancelledImport.Ok && CancelledImport.Diagnostic.Code == "Cancelled",
		"cancelled background import commits a deterministic failed status");
	auto EscapedImport = Assets->ImportProjectAsset(Mount, "../outside.bmp", AssetKind::Image, "Escape");
	Check(!EscapedImport.Ok && (EscapedImport.Diagnostic.Code == "PathEscape" || EscapedImport.Diagnostic.Code == "AbsolutePath"),
		"asset import cannot escape the project SourceMount");
	WriteBytes(Root / "assets" / "oversized-header.bmp", MakeBmp(2048, 1, 1));
	auto OversizedImage = Assets->ImportProjectAsset(Mount, "assets/oversized-header.bmp", AssetKind::Image, "Too Wide");
	Check(!OversizedImage.Ok && OversizedImage.Diagnostic.Code == "ImageLimit",
		"image dimension bombs fail before full decode");
	{
		auto BoundedAssets = std::make_shared<AssetService>();
		std::vector<std::uint8_t> FullImage(1024 * 1024 * 4, 127);
		std::string RetainedReference;
		for (std::size_t Index = 0; Index < 15; ++Index)
			RetainedReference = BoundedAssets->RegisterMemoryImage(
				"Bounded " + std::to_string(Index), 1024, 1024, FullImage
			);
		CheckThrows<std::length_error>([&] {
			(void)BoundedAssets->RegisterMemoryImage("Rejected", 1024, 1024, FullImage);
		}, "the canonical CPU cache rejects admission before exceeding 64 MiB");
		Check(BoundedAssets->ResolveImage(RetainedReference).has_value(),
			"cache admission failure preserves previously retained assets");
	}

	std::filesystem::create_directories(Root / ".gargantuan");
	World->MarkPersistenceSubtreeArchivable();
	World->InitializeLoadedProjectRevision();
	auto ProjectState = Project::forDestination(&Filesystem, InstanceSerialization::InstanceFormat::Json);
	auto Snapshot = ProjectState.CaptureGame(World, World->GetAuthoritativeRevision());
	if (GltfCollision) {
		auto TamperedCollisionSnapshot = Snapshot.Assets;
		const auto CollisionPath = GltfCollision->Id;
		const auto CollisionRecord = Assets->GetAsset(AssetReference::FromAssetId(CollisionPath).Value);
		bool Modified = false;
		if (CollisionRecord) for (auto &Artifact : TamperedCollisionSnapshot.Artifacts)
			if (Artifact.RelativePath.find(CollisionRecord->ContentId.ToString()) != std::string::npos && Artifact.Bytes) {
				auto Bytes = std::make_shared<std::vector<std::uint8_t>>(*Artifact.Bytes);
				Bytes->back() ^= 0x5a;
				Artifact.Bytes = std::move(Bytes);
				Modified = true;
				break;
			}
		auto TamperedCollisionWorld = std::make_shared<DataModel>();
		auto TamperedCollisionAssets = GetAssets(TamperedCollisionWorld);
		TamperedCollisionAssets->LoadProjectAssetSnapshot(TamperedCollisionSnapshot);
		const auto FailedCollision = TamperedCollisionAssets->GetAsset(AssetReference::FromAssetId(CollisionPath).Value);
		Check(Modified && FailedCollision && FailedCollision->State == AssetState::Failed &&
			!TamperedCollisionAssets->ResolveCollisionForMesh(GltfMeshRecord->Reference.Value),
			"tampered collision artifact fails closed while mesh identity remains catalogued");
		TamperedCollisionAssets.reset(); TamperedCollisionWorld->Destroy();
	}
	for (std::uint64_t Iteration = 1; Iteration <= 2; ++Iteration) {
		PlaySession Session(
			{Iteration}, Snapshot.Contents, InstanceSerialization::InstanceFormat::Json, Root,
			320, 180, Snapshot.Revision, Snapshot.Assets
		);
		auto PlayAssets = GetAssets(Session.GetWorld());
		auto PlayImage = PlayAssets ? PlayAssets->GetAsset(PersistentImageReference) : std::nullopt;
		Check(Session.GetState() == PlaySessionState::Running && PlayImage &&
			PlayImage->Id == SuccessfulReimport.Record->Id &&
			PlayImage->ContentRevision == SuccessfulReimport.Record->ContentRevision &&
			PlayAssets->ResolveImage(PersistentImageReference).has_value() &&
			PlayAssets->ResolveMesh(MeshImport.Record->Reference.Value).has_value() &&
			PlayAssets->ResolveMeshResource(GltfMeshRecord->Reference.Value).has_value() &&
			PlayAssets->ResolveCollisionForMesh(GltfMeshRecord->Reference.Value).has_value() &&
			PlayAssets->ResolveMaterial(TexturedMaterialRecord->Reference.Value).has_value() &&
			PlayAssets->ResolveFont(FontImport.Record->Reference.Value).has_value(),
			"Play clones the exact image, mesh, material, dependency, and font asset revisions");
		Check(std::dynamic_pointer_cast<MeshPart>(Session.GetWorld()->FindFirstDescendant("Imported Mesh A")) != nullptr,
			"Play clones schema-backed MeshPart instances with their strict asset references");
		Session.Stop();
		Check(Session.GetState() == PlaySessionState::Stopped && !Session.GetWorld() &&
			Assets->ResolveImage(PersistentImageReference).has_value(),
			"Stop releases runtime asset ownership without changing authoring assets");
	}
	ProjectState.PersistGameAtomically(Snapshot);
	Check(std::filesystem::is_regular_file(Root / ".gargantuan" / "assets" / "catalog.json") &&
		!Snapshot.Assets.Artifacts.empty(), "save captures catalog and canonical artifacts atomically");

	DiskFilesystem ReopenFilesystem(Root);
	auto ReopenedProject = Project::fromExisting(&ReopenFilesystem);
	auto ReopenedWorld = ReopenedProject.DeserializeGame();
	auto ReopenedAssets = GetAssets(ReopenedWorld);
	const auto ReopenedMaterial = ReopenedAssets->ResolveMaterial(TexturedMaterialRecord->Reference.Value);
	Check(ReopenedMaterial && ReopenedMaterial->Value.EmissiveTexture == GltfImageRecord->Id &&
		ReopenedMaterial->Value.OcclusionTexture == GltfImageRecord->Id && ReopenedMaterial->Value.NormalScale == -.5f &&
		ReopenedMaterial->Value.TextureSamplers[OcclusionSlot] == ExplicitSampler,
		"advanced material artifact and dependency graph survive fresh project reopen");
	Check(ReopenedAssets && ReopenedAssets->IsAvailable(PersistentImageReference) &&
		ReopenedAssets->ResolveMesh(MeshImport.Record->Reference.Value).has_value() &&
		ReopenedAssets->ResolveMeshResource(GltfMeshRecord->Reference.Value).has_value() &&
		ReopenedAssets->ResolveCollisionForMesh(GltfMeshRecord->Reference.Value).has_value() &&
		ReopenedAssets->ResolveMaterial(TexturedMaterialRecord->Reference.Value).has_value() &&
		ReopenedAssets->ResolveFont(FontImport.Record->Reference.Value).has_value(),
		"saved image, mesh, material dependency graph, and font assets survive restart/reload");
	auto ReopenedMeshPart = std::dynamic_pointer_cast<MeshPart>(ReopenedWorld->FindFirstDescendant("Imported Mesh A"));
	Check(ReopenedMeshPart && ReopenedMeshPart->GetMesh() == GltfMeshRecord->Reference.Value &&
		ReopenedMeshPart->GetMaterial().empty() &&
		ReopenedMeshPart->GetCollisionMode() == Enums::MeshCollisionMode::StaticMesh,
		"MeshPart schema properties survive save and reopen");
	auto ReopenedCancelled = ReopenedAssets->GetAsset(CancelledImport.Record->Reference.Value);
	Check(ReopenedCancelled && ReopenedCancelled->State == AssetState::Failed && ReopenedCancelled->Diagnostic &&
		ReopenedCancelled->Diagnostic->Code == "Cancelled",
		"failed import status and bounded diagnostic survive restart/reload");

	std::filesystem::create_directories(Relocated);
	std::filesystem::copy(Root, Relocated,
		std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
	DiskFilesystem RelocatedFilesystem(Relocated);
	auto RelocatedProject = Project::fromExisting(&RelocatedFilesystem);
	auto RelocatedWorld = RelocatedProject.DeserializeGame();
	auto RelocatedAssets = GetAssets(RelocatedWorld);
	SourceMount RelocatedMount(RelocatedFilesystem);
	auto RelocatedReimport = RelocatedAssets->ReimportProjectAsset(RelocatedMount, PersistentImageReference);
	Check(RelocatedReimport.Ok && RelocatedReimport.Record && RelocatedReimport.Record->Id == SuccessfulReimport.Record->Id,
		"project-relative source identity survives root relocation");

	const auto ArtifactPath = Relocated / ".gargantuan" / "assets" / "artifacts" /
		(SuccessfulReimport.Record->ContentId.ToString() + ".gasset");
	{
		std::fstream Artifact(ArtifactPath, std::ios::binary | std::ios::in | std::ios::out);
		Artifact.seekg(-1, std::ios::end);
		char Byte = 0;
		Artifact.read(&Byte, 1);
		Byte ^= 0x5a;
		Artifact.seekp(-1, std::ios::end);
		Artifact.write(&Byte, 1);
	}
	DiskFilesystem TamperedFilesystem(Relocated);
	auto TamperedProject = Project::fromExisting(&TamperedFilesystem);
	auto TamperedWorld = TamperedProject.DeserializeGame();
	auto TamperedAssets = GetAssets(TamperedWorld);
	auto TamperedRecord = TamperedAssets->GetAsset(PersistentImageReference);
	Check(TamperedRecord && TamperedRecord->State == AssetState::Failed && !TamperedRecord->Asset &&
		TamperedRecord->Diagnostic && (TamperedRecord->Diagnostic->Code == "ContentMismatch" ||
			TamperedRecord->Diagnostic->Code == "IntegrityFailure"),
		"tampered canonical artifacts fail closed without crashing project load");

	TamperedAssets.reset(); TamperedWorld->Destroy(); TamperedWorld.reset();
	RelocatedAssets.reset(); RelocatedWorld->Destroy(); RelocatedWorld.reset();
	ReopenedAssets.reset(); ReopenedWorld->Destroy(); ReopenedWorld.reset();
	ImageObject.reset(); TextObject.reset(); Assets.reset(); WorkspaceValue.reset(); World->Destroy(); World.reset();

	WriteBytes(Root / "assets" / "editor.bmp", MakeBmp(2, 2, 51));
	EditorHost Host("asset-token");
	std::size_t RequestNumber = 0;
	auto Call = [&](std::string Method, nlohmann::json Parameters = nlohmann::json::object()) {
		nlohmann::json Request{
			{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++RequestNumber)},
			{"SessionToken", "asset-token"}, {"Method", std::move(Method)}, {"Params", std::move(Parameters)},
		};
		return nlohmann::json::parse(Host.HandleRequest(Request.dump()));
	};
	auto Handshake = Call("Handshake");
	Check(Handshake["Ok"] && std::ranges::contains(Handshake["Result"]["Capabilities"], "AssetCatalog") &&
		std::ranges::contains(Handshake["Result"]["Capabilities"], "StrictAssetReferences") &&
		std::ranges::contains(Handshake["Result"]["Capabilities"], "AdvancedMaterialMetadata"),
		"EditorHost advertises the Asset Foundation command contract");
	auto Opened = Call("OpenProject", {{"Root", Root.generic_string()}});
	Check(Opened["Ok"], "EditorHost opens a persisted asset project");
	auto Catalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", true}});
	Check(Catalog["Ok"] && Catalog["Result"]["CatalogVersion"] == 2 &&
		std::ranges::any_of(Catalog["Result"]["Assets"], [&](const auto &Record) {
			return Record["Reference"] == PersistentImageReference && Record["State"] == "Ready";
		}) && std::ranges::any_of(Catalog["Result"]["Assets"], [&](const auto &Record) {
			return Record["Kind"] == "Material" && Record.contains("SourceGroupId") &&
				Record.contains("LogicalKey") && Record.contains("Dependencies");
		}), "EditorHost exposes bounded v2 grouping, dependency, and material metadata without raw resources");
	const auto AdvancedCatalog = Call("GetAssetCatalog", {{"IncludeAdvancedMaterials", true}});
	const auto AdvancedOff = Call("GetAssetCatalog", {{"IncludeAdvancedMaterials", false}});
	const auto BadAdvancedRequest = Call("GetAssetCatalog", {{"IncludeAdvancedMaterials", 1}});
	Check(AdvancedCatalog["Ok"] && AdvancedCatalog["Result"]["CatalogVersion"] == 2 &&
		AdvancedOff["Ok"] && AdvancedOff["Result"]["Assets"] == Catalog["Result"]["Assets"] &&
		!BadAdvancedRequest["Ok"] && BadAdvancedRequest["Error"]["Code"] == "MalformedRequest" &&
		std::ranges::none_of(Catalog["Result"]["Assets"], [](const auto &Record) {
			return Record["Metadata"].contains("AdvancedMaterial");
		}) && std::ranges::any_of(AdvancedCatalog["Result"]["Assets"], [&](const auto &Record) {
			if (Record["Reference"] != TexturedMaterialRecord->Reference.Value ||
				!Record["Metadata"].contains("AdvancedMaterial")) return false;
			const auto &Metadata = Record["Metadata"]["AdvancedMaterial"];
			return Metadata["EmissiveFactor"] == nlohmann::json::array({.125, .25, .5}) &&
				Metadata["EmissiveTexture"] == GltfImageRecord->Id.ToString() &&
				Metadata["OcclusionTexture"] == GltfImageRecord->Id.ToString() &&
				Metadata["OcclusionStrength"] == .375 && Metadata["NormalScale"] == -.5 &&
				Metadata["TextureSamplers"]["Occlusion"]["MinFilter"] == "NearestMipmapLinear" &&
				Metadata["TextureSamplers"]["Occlusion"]["WrapU"] == "MirroredRepeat";
		}), "advanced metadata is bounded opt-in and leaves legacy v2 catalog responses unchanged");
	auto Revision = Catalog["Result"]["ProjectState"]["AuthoritativeRevision"].get<std::uint64_t>();
	auto HostImport = Call("ImportAsset", {
		{"Source", "assets/editor.bmp"}, {"Kind", "Image"}, {"Name", "Editor Image"}, {"ExpectedRevision", Revision},
	});
	Check(HostImport["Ok"] && HostImport["Result"]["OperationSucceeded"] &&
		HostImport["Result"]["Asset"]["Reference"].get<std::string>().starts_with("asset://"),
		"EditorHost imports a project-relative asset through SourceMount");
	Revision = HostImport["Result"]["ProjectState"]["AuthoritativeRevision"].get<std::uint64_t>();
	auto HostEscape = Call("ImportAsset", {
		{"Source", "../escape.bmp"}, {"Kind", "Image"}, {"Name", "Escape"}, {"ExpectedRevision", Revision},
	});
	Check(HostEscape["Ok"] && !HostEscape["Result"]["OperationSucceeded"] &&
		HostEscape["Result"]["Diagnostic"]["Code"] == "PathEscape",
		"EditorHost returns committed failed-import state and a structured SourceMount diagnostic");
	Revision = HostEscape["Result"]["ProjectState"]["AuthoritativeRevision"].get<std::uint64_t>();
	auto HostDelete = Call("DeleteAsset", {
		{"Reference", HostImport["Result"]["Asset"]["Reference"]}, {"ExpectedRevision", Revision},
	});
	Check(HostDelete["Ok"] && HostDelete["Result"]["OperationSucceeded"] &&
		HostDelete["Result"]["Asset"]["State"] == "Missing",
		"EditorHost deletes an unreferenced catalog asset with optimistic revision checking");

	if (Failures != 0) return 1;
	std::cout << "All Asset Foundation 2A tests passed\n";
	return 0;
}
