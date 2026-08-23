#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {
	void AppendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
		Bytes.push_back(static_cast<std::uint8_t>(Value));
		Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
	}

	void AppendU32(std::vector<std::uint8_t> &Bytes, std::uint32_t Value) {
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.push_back(static_cast<std::uint8_t>(Value >> (Shift * 8)));
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
	if (!Assets) return 1;
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
	std::cout << "METRIC asset_catalog_reload_ms " << Milliseconds(CatalogLoadEnd - CatalogLoadStart) << '\n';
	std::cout << "METRIC asset_cache_admission_60mib_ms " << Milliseconds(CacheEnd - CacheStart) << '\n';
	std::cout << "METRIC asset_texture_upload_bytes " << TextureChanges.UploadBytes << '\n';
	std::cout << "METRIC asset_mesh_creates " << MeshChanges.Creates.size() << '\n';
	std::cout << "METRIC asset_lookup_hits " << Lookup1kHits + Lookup10kHits << '\n';
	std::cout << "METRIC asset_resolved_pixels " << ResolvedPixels << '\n';
	return Lookup1kHits == 1024 && Lookup10kHits == 10240 && ResolvedPixels == 1024ull * 16 * 16 &&
		ColdImage.Ok && Mesh.Ok && Font.Ok && Reimport.Ok && FontBytes > 0 &&
		!TextureChanges.Creates.empty() && !MeshChanges.Creates.empty() &&
		LoadedAssets->IsAvailable(ColdImage.Record->Reference.Value) && CacheRejected &&
		CacheAssets->ResolveImage(RetainedReference).has_value() ? 0 : 1;
}
