#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/assets/AssetImporter.hpp"

#include <array>
#include <chrono>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace {
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

	struct BenchmarkGltf {
		nlohmann::ordered_json Document;
		std::vector<std::uint8_t> Binary;
	};

	BenchmarkGltf MakeBenchmarkGltf(std::size_t PrimitiveCount, std::size_t MaterialCount, std::uint8_t ImageSeed) {
		BenchmarkGltf Result;
		for (const auto Value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f})
			AppendFloat(Result.Binary, Value);
		for (std::size_t Index = 0; Index < 3; ++Index) {
			AppendFloat(Result.Binary, 0.0f); AppendFloat(Result.Binary, 0.0f); AppendFloat(Result.Binary, 1.0f);
		}
		for (const auto Value : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f}) AppendFloat(Result.Binary, Value);
		AppendU16(Result.Binary, 0); AppendU16(Result.Binary, 1); AppendU16(Result.Binary, 2);
		AlignFour(Result.Binary);

		auto &Root = Result.Document;
		Root["asset"] = {{"version", "2.0"}, {"generator", "Gargantuan benchmark"}};
		Root["bufferViews"] = nlohmann::ordered_json::array({
			{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}},
			{{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}},
			{{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 24}},
			{{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 6}},
		});
		Root["accessors"] = nlohmann::ordered_json::array({
			{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
			{{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
			{{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
			{{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
		});
		Root["images"] = nlohmann::ordered_json::array();
		Root["textures"] = nlohmann::ordered_json::array();
		Root["materials"] = nlohmann::ordered_json::array();
		for (std::size_t Index = 0; Index < MaterialCount; ++Index) {
			const auto ImageOffset = Result.Binary.size();
			auto Image = MakeBmp(64, 64, static_cast<std::uint8_t>(ImageSeed + Index * 17));
			Result.Binary.insert(Result.Binary.end(), Image.begin(), Image.end());
			AlignFour(Result.Binary);
			Root["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", ImageOffset}, {"byteLength", Image.size()}});
			Root["images"].push_back({{"name", "Texture " + std::to_string(Index)},
				{"bufferView", 4 + Index}, {"mimeType", "image/bmp"}});
			Root["textures"].push_back({{"source", Index}});
			Root["materials"].push_back({{"name", "Material " + std::to_string(Index)},
				{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", Index}}},
					{"metallicFactor", static_cast<double>(Index) / MaterialCount}, {"roughnessFactor", 0.75}}}});
		}
		Root["meshes"] = nlohmann::ordered_json::array({{{"name", "Benchmark Mesh"},
			{"primitives", nlohmann::ordered_json::array()}}});
		for (std::size_t Index = 0; Index < PrimitiveCount; ++Index)
			Root["meshes"][0]["primitives"].push_back({
				{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
				{"indices", 3}, {"material", Index % MaterialCount},
			});
		Root["buffers"] = nlohmann::ordered_json::array({{{"byteLength", Result.Binary.size()}}});
		return Result;
	}

	std::vector<std::uint8_t> MakeGlb(const BenchmarkGltf &Fixture) {
		auto JsonText = Fixture.Document.dump();
		while (JsonText.size() % 4 != 0) JsonText.push_back(' ');
		std::vector<std::uint8_t> Result;
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
		if (!Output) throw std::runtime_error("Could not write asset benchmark fixture");
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Text) {
		WriteBytes(Path, std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size()));
	}
}

int main() {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	BootstrapNativeRuntimeSchema();
	const auto Unique = std::to_string(Clock::now().time_since_epoch().count());
	const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-asset-benchmark-" + Unique);
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
	} CleanupValue{Root};
	WriteBytes(Root / "assets" / "image.bmp", MakeBmp(64, 64, 7));
	WriteText(Root / "assets" / "mesh.obj",
		"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n");
	auto SmallGltf = MakeBenchmarkGltf(1, 1, 31);
	auto MediumGltf = MakeBenchmarkGltf(128, 4, 47);
	auto CompoundGltf = MakeBenchmarkGltf(4, 4, 63);
	auto SmallGlb = MakeGlb(SmallGltf);
	auto MediumGlb = MakeGlb(MediumGltf);
	auto CompoundGlb = MakeGlb(CompoundGltf);
	WriteBytes(Root / "assets" / "small.glb", SmallGlb);
	WriteBytes(Root / "assets" / "medium.glb", MediumGlb);
	WriteBytes(Root / "assets" / "compound.glb", CompoundGlb);
	std::filesystem::copy_file(
		std::filesystem::path(GARGANTUAN_ASSET_TEST_FONT_PATH), Root / "assets" / "font.ttf",
		std::filesystem::copy_options::overwrite_existing
	);
	DiskFilesystem Filesystem(Root);
	SourceMount Mount(Filesystem);
	auto World = std::make_shared<DataModel>();
	World->Root = Root;
	World->Filesystem = &Filesystem;
	auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
	auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
	if (!Assets || !WorkspaceValue) return 1;
	std::array<std::uint8_t, 16 * 16 * 4> Pixels{};
	std::vector<std::string> References;
	References.reserve(1024);
	const auto ImportStart = Clock::now();
	for (std::size_t Index = 0; Index < 1024; ++Index) {
		Pixels[Index % Pixels.size()] = static_cast<std::uint8_t>(Index);
		References.push_back(Assets->RegisterMemoryImage("benchmark/" + std::to_string(Index), 16, 16, Pixels));
	}
	const auto ImportEnd = Clock::now();
	std::uint64_t Lookup1kHits = 0;
	const auto Lookup1kStart = Clock::now();
	for (const auto &Reference : References) Lookup1kHits += Assets->IsAvailable(Reference);
	const auto Lookup1kEnd = Clock::now();
	std::uint64_t Lookup10kHits = 0;
	const auto Lookup10kStart = Clock::now();
	for (std::size_t Iteration = 0; Iteration < 10; ++Iteration)
		for (const auto &Reference : References) Lookup10kHits += Assets->IsAvailable(Reference);
	const auto Lookup10kEnd = Clock::now();
	std::uint64_t ResolvedPixels = 0;
	const auto ResolveStart = Clock::now();
	for (const auto &Reference : References) {
		auto Image = Assets->ResolveImage(Reference);
		if (Image) ResolvedPixels += static_cast<std::uint64_t>(Image->Width) * Image->Height;
	}
	const auto ResolveEnd = Clock::now();
	const auto ColdImageStart = Clock::now();
	auto ColdImage = Assets->ImportProjectAsset(Mount, "assets/image.bmp", AssetKind::Image, "Cold Image");
	const auto ColdImageEnd = Clock::now();
	const auto MeshStart = Clock::now();
	auto Mesh = Assets->ImportProjectAsset(Mount, "assets/mesh.obj", AssetKind::Mesh, "Mesh");
	const auto MeshEnd = Clock::now();
	const auto FontStart = Clock::now();
	auto Font = Assets->ImportProjectAsset(Mount, "assets/font.ttf", AssetKind::Font, "Font");
	const auto FontEnd = Clock::now();
	std::uint64_t FontBytes = 0;
	const auto FontResolveStart = Clock::now();
	for (std::size_t Iteration = 0; Iteration < 1024; ++Iteration) {
		auto Resolved = Font.Record ? Assets->ResolveFont(Font.Record->Reference.Value) : std::nullopt;
		if (Resolved && Resolved->Bytes) FontBytes += Resolved->Bytes->size();
	}
	const auto FontResolveEnd = Clock::now();
	WriteBytes(Root / "assets" / "image.bmp", MakeBmp(64, 64, 29));
	const auto ReimportStart = Clock::now();
	auto Reimport = ColdImage.Record
		? Assets->ReimportProjectAsset(Mount, ColdImage.Record->Reference.Value)
		: AssetOperationResult{};
	const auto ReimportEnd = Clock::now();
	const auto ResidencyStart = Clock::now();
	auto TextureChanges = Assets->DrainTextureChanges();
	auto MeshChanges = Assets->DrainMeshChanges();
	const auto ResidencyEnd = Clock::now();

	const auto ParseStart = Clock::now();
	auto ParsedMediumJson = nlohmann::ordered_json::parse(MediumGltf.Document.dump());
	const auto ParseEnd = Clock::now();
	auto GltfImporter = CreateGltfImporter();
	const auto ConversionStart = Clock::now();
	auto MediumGraph = GltfImporter->ImportGraph(MediumGlb, {
		AssetKind::Mesh, ".glb", {}, Clock::now() + std::chrono::seconds(10), {},
	});
	const auto ConversionEnd = Clock::now();
	std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> CanonicalArtifacts;
	const auto SerializationStart = Clock::now();
	if (MediumGraph) for (const auto &Node : MediumGraph->Nodes) {
		auto Artifact = EncodeAssetArtifact(Node.Asset, Node.Kind);
		if (Artifact) CanonicalArtifacts.push_back(*Artifact);
	}
	const auto SerializationEnd = Clock::now();
	std::size_t HashedBytes = 0;
	const auto HashStart = Clock::now();
	for (const auto &Artifact : CanonicalArtifacts) {
		(void)AssetContentId::Hash(*Artifact);
		HashedBytes += Artifact->size();
	}
	const auto HashEnd = Clock::now();
	std::size_t GraphEdges = 0;
	const auto GraphStart = Clock::now();
	std::unordered_map<std::string, std::unordered_set<std::string>> DependencyGraph;
	if (MediumGraph) for (const auto &Node : MediumGraph->Nodes)
		for (const auto &Binding : Node.Bindings)
			GraphEdges += DependencyGraph[Node.LogicalKey].insert(Binding.LogicalKey).second;
	const auto GraphEnd = Clock::now();

	const auto SmallGltfStart = Clock::now();
	auto SmallImport = Assets->ImportProjectAsset(Mount, "assets/small.glb", AssetKind::Mesh, "Small GLB");
	const auto SmallGltfEnd = Clock::now();
	const auto MediumGltfStart = Clock::now();
	auto MediumImport = Assets->ImportProjectAsset(Mount, "assets/medium.glb", AssetKind::Mesh, "Medium GLB");
	const auto MediumGltfEnd = Clock::now();
	const auto CompoundGltfStart = Clock::now();
	auto CompoundImport = Assets->ImportProjectAsset(Mount, "assets/compound.glb", AssetKind::Mesh, "Compound GLB");
	const auto CompoundGltfEnd = Clock::now();
	const auto NoChangeStart = Clock::now();
	auto NoChange = CompoundImport.Record ?
		Assets->ReimportProjectAsset(Mount, CompoundImport.Record->Reference.Value) : AssetOperationResult{};
	const auto NoChangeEnd = Clock::now();
	CompoundGltf = MakeBenchmarkGltf(4, 4, 81);
	CompoundGlb = MakeGlb(CompoundGltf);
	WriteBytes(Root / "assets" / "compound.glb", CompoundGlb);
	const auto TextureReimportStart = Clock::now();
	auto TextureReimport = CompoundImport.Record ?
		Assets->ReimportProjectAsset(Mount, CompoundImport.Record->Reference.Value) : AssetOperationResult{};
	const auto TextureReimportEnd = Clock::now();

	std::string SharedMeshReference;
	std::vector<std::string> MaterialReferences;
	for (const auto &Record : TextureReimport.Records) {
		if (Record.Kind == AssetKind::Mesh) SharedMeshReference = Record.Reference.Value;
		if (Record.Kind == AssetKind::Material) MaterialReferences.push_back(Record.Reference.Value);
	}
	std::vector<std::shared_ptr<MeshPart>> MeshParts;
	MeshParts.reserve(1000);
	const auto MeshPartStart = Clock::now();
	for (std::size_t Index = 0; Index < 1000; ++Index) {
		auto Part = std::make_shared<MeshPart>();
		Part->SetName("Benchmark MeshPart " + std::to_string(Index));
		Part->SetMesh(SharedMeshReference);
		if (Index % 2 == 0 && !MaterialReferences.empty())
			Part->SetMaterial(MaterialReferences[Index % MaterialReferences.size()]);
		Part->SetParent(WorkspaceValue);
		MeshParts.push_back(std::move(Part));
	}
	const auto MeshPartEnd = Clock::now();
	auto CompoundTextureChanges = Assets->DrainTextureChanges();
	auto CompoundMeshChanges = Assets->DrainMeshChanges();
	RenderPublisher Publisher;
	Publisher.SetUiTextureChanges(std::move(CompoundTextureChanges.Creates),
		std::move(CompoundTextureChanges.Updates), std::move(CompoundTextureChanges.Removes));
	Publisher.SetAssetMeshChanges(std::move(CompoundMeshChanges.Creates), std::move(CompoundMeshChanges.Removes));
	const auto PublicationStart = Clock::now();
	auto SharedPublication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 1280, 720);
	const auto PublicationEnd = Clock::now();
	Publisher.RequestFullResync();
	const auto FullResyncStart = Clock::now();
	auto FullResync = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 1280, 720);
	const auto FullResyncEnd = Clock::now();

	auto DependencyCatalog = Assets->GetCatalog(false);
	std::size_t DependencyHits = 0;
	const auto DependencyLookupStart = Clock::now();
	for (std::size_t Iteration = 0; Iteration < 1000; ++Iteration)
		for (const auto &Record : DependencyCatalog)
			for (const auto Dependency : Record.Dependencies)
				DependencyHits += Assets->GetAsset(AssetReference::FromAssetId(Dependency).Value).has_value();
	const auto DependencyLookupEnd = Clock::now();
	auto Snapshot = Assets->CaptureProjectAssets();
	const auto CatalogLoadStart = Clock::now();
	auto LoadedAssets = std::make_shared<AssetService>();
	LoadedAssets->LoadProjectAssetSnapshot(Snapshot);
	const auto CatalogLoadEnd = Clock::now();

	auto CacheAssets = std::make_shared<AssetService>();
	std::vector<std::uint8_t> FullImage(1024 * 1024 * 4, 127);
	std::string RetainedReference;
	const auto CacheStart = Clock::now();
	for (std::size_t Index = 0; Index < 15; ++Index)
		RetainedReference = CacheAssets->RegisterMemoryImage("cache/" + std::to_string(Index), 1024, 1024, FullImage);
	bool CacheRejected = false;
	try { (void)CacheAssets->RegisterMemoryImage("cache/rejected", 1024, 1024, FullImage); }
	catch (const std::length_error &) { CacheRejected = true; }
	const auto CacheEnd = Clock::now();
	auto Milliseconds = [](auto Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	};
	std::cout << "METRIC asset_memory_import_1024_ms " << Milliseconds(ImportEnd - ImportStart) << '\n';
	std::cout << "METRIC asset_catalog_lookup_1024_ms " << Milliseconds(Lookup1kEnd - Lookup1kStart) << '\n';
	std::cout << "METRIC asset_catalog_lookup_10240_ms " << Milliseconds(Lookup10kEnd - Lookup10kStart) << '\n';
	std::cout << "METRIC asset_resolve_image_1024_ms " << Milliseconds(ResolveEnd - ResolveStart) << '\n';
	std::cout << "METRIC asset_cold_image_import_ms " << Milliseconds(ColdImageEnd - ColdImageStart) << '\n';
	std::cout << "METRIC asset_mesh_import_ms " << Milliseconds(MeshEnd - MeshStart) << '\n';
	std::cout << "METRIC asset_font_import_ms " << Milliseconds(FontEnd - FontStart) << '\n';
	std::cout << "METRIC asset_font_resolve_1024_ms " << Milliseconds(FontResolveEnd - FontResolveStart) << '\n';
	std::cout << "METRIC asset_reimport_ms " << Milliseconds(ReimportEnd - ReimportStart) << '\n';
	std::cout << "METRIC asset_residency_drain_ms " << Milliseconds(ResidencyEnd - ResidencyStart) << '\n';
	std::cout << "METRIC asset_gltf_source_parse_ms " << Milliseconds(ParseEnd - ParseStart) << '\n';
	std::cout << "METRIC asset_gltf_canonical_conversion_ms " << Milliseconds(ConversionEnd - ConversionStart) << '\n';
	std::cout << "METRIC asset_gltf_artifact_serialization_ms " << Milliseconds(SerializationEnd - SerializationStart) << '\n';
	std::cout << "METRIC asset_gltf_hashing_ms " << Milliseconds(HashEnd - HashStart) << '\n';
	std::cout << "METRIC asset_gltf_dependency_graph_build_ms " << Milliseconds(GraphEnd - GraphStart) << '\n';
	std::cout << "METRIC asset_gltf_cold_small_import_ms " << Milliseconds(SmallGltfEnd - SmallGltfStart) << '\n';
	std::cout << "METRIC asset_gltf_medium_import_ms " << Milliseconds(MediumGltfEnd - MediumGltfStart) << '\n';
	std::cout << "METRIC asset_gltf_mesh_4_materials_textures_import_ms " << Milliseconds(CompoundGltfEnd - CompoundGltfStart) << '\n';
	std::cout << "METRIC asset_gltf_reimport_no_change_ms " << Milliseconds(NoChangeEnd - NoChangeStart) << '\n';
	std::cout << "METRIC asset_gltf_reimport_one_texture_changed_ms " << Milliseconds(TextureReimportEnd - TextureReimportStart) << '\n';
	std::cout << "METRIC asset_meshpart_shared_1000_create_ms " << Milliseconds(MeshPartEnd - MeshPartStart) << '\n';
	std::cout << "METRIC asset_meshpart_shared_1000_render_publication_ms " << Milliseconds(PublicationEnd - PublicationStart) << '\n';
	std::cout << "METRIC asset_renderer_full_resync_shared_1000_ms " << Milliseconds(FullResyncEnd - FullResyncStart) << '\n';
	std::cout << "METRIC asset_dependency_catalog_lookup_ms " << Milliseconds(DependencyLookupEnd - DependencyLookupStart) << '\n';
	std::cout << "METRIC asset_catalog_reload_ms " << Milliseconds(CatalogLoadEnd - CatalogLoadStart) << '\n';
	std::cout << "METRIC asset_cache_admission_60mib_ms " << Milliseconds(CacheEnd - CacheStart) << '\n';
	std::cout << "METRIC asset_texture_upload_bytes " << TextureChanges.UploadBytes << '\n';
	std::cout << "METRIC asset_mesh_creates " << MeshChanges.Creates.size() << '\n';
	std::cout << "METRIC asset_lookup_hits " << Lookup1kHits + Lookup10kHits << '\n';
	std::cout << "METRIC asset_resolved_pixels " << ResolvedPixels << '\n';
	std::cout << "METRIC asset_gltf_hashed_bytes " << HashedBytes << '\n';
	std::cout << "METRIC asset_gltf_dependency_edges " << GraphEdges << '\n';
	std::cout << "METRIC asset_dependency_lookup_hits " << DependencyHits << '\n';
	return Lookup1kHits == 1024 && Lookup10kHits == 10240 && ResolvedPixels == 1024ull * 16 * 16 &&
		ColdImage.Ok && Mesh.Ok && Font.Ok && Reimport.Ok && FontBytes > 0 &&
		!ParsedMediumJson.empty() && MediumGraph && CanonicalArtifacts.size() == MediumGraph->Nodes.size() &&
		SmallImport.Ok && MediumImport.Ok && CompoundImport.Ok && NoChange.Ok && TextureReimport.Ok &&
		!SharedMeshReference.empty() && MaterialReferences.size() == 4 && MeshParts.size() == 1000 &&
		SharedPublication->Creates.size() == 1000 && FullResync->FullResync && FullResync->Creates.size() == 1000 &&
		GraphEdges > 0 && DependencyHits > 0 && HashedBytes > 0 &&
		!TextureChanges.Creates.empty() && !MeshChanges.Creates.empty() &&
		LoadedAssets->IsAvailable(ColdImage.Record->Reference.Value) && CacheRejected &&
		CacheAssets->ResolveImage(RetainedReference).has_value() ? 0 : 1;
}
