#include "ContentScaleGameplay.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using namespace gargantuan;
	namespace Filesystem = std::filesystem;

	constexpr std::size_t QualifiedScaleObjects = 512;
	constexpr std::size_t QualifiedScaleBytes = 273'032;
	constexpr std::uint32_t QualifiedProducerPlayerId = 1;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::string ReadText(const Filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) throw std::runtime_error("Could not open scale package input");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	bool HasError(const std::vector<PackageDiagnostic> &Diagnostics) {
		return std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
		});
	}

	PackageContentManifest CheckPackage(const Filesystem::path &Root, const GamePayload::ContentUnit &Expected) {
		Require(!HasError(PackageBuilder::Validate(Root)), "Qualified scale package validation failed");
		std::vector<PackageDiagnostic> Diagnostics;
		auto Loaded = PackageBuilder::Load(Root, Diagnostics);
		Require(Loaded.has_value() && !HasError(Diagnostics), "Qualified scale package could not be loaded");
		const auto ManifestText = ReadText(Root / test::ScaleManifestReference);
		auto Manifest = ParsePackageContentManifest(ManifestText);
		Require(Manifest.has_value() && Manifest->Entries.size() == 1,
			"Qualified scale package must have exactly one content unit");
		Require(Manifest->Package.Project == Loaded->Inspection.Identity &&
			Manifest->Package.PackageVersion == Loaded->Inspection.Revision,
			"Qualified scale package identity differs from its content manifest");
		const auto &Entry = Manifest->Entries.front();
		Require(Entry.Key == Expected.Entry.Key && Entry.BlobReference == Expected.Entry.BlobReference &&
			Entry.ObjectCount == QualifiedScaleObjects && Entry.CompressedBytes == QualifiedScaleBytes &&
			Entry.UncompressedBytes == QualifiedScaleBytes && Entry.Digest == Expected.Entry.Digest &&
			ReadText(Root / Entry.BlobReference) == Expected.Payload,
			"Qualified scale package changed canonical content metadata or bytes");
		return *Manifest;
	}

	void BuildPair(const Filesystem::path &Output, const Filesystem::path &PlayerRuntime,
		const Filesystem::path &ServerRuntime) {
		Require(Output.is_absolute() && PlayerRuntime.is_absolute() && ServerRuntime.is_absolute(),
			"All qualified scale package paths must be absolute");
		Require(!Filesystem::exists(Output), "Qualified scale output root must not exist");
		Require(Filesystem::is_directory(Output.parent_path()), "Qualified scale output parent is unavailable");
		Require(Filesystem::is_directory(PlayerRuntime) && Filesystem::is_directory(ServerRuntime),
			"Qualified scale runtime distributions are unavailable");
		Filesystem::create_directory(Output);
		const auto ProjectRoot = Filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT);
		BootstrapProjectRuntimeSchema(ProjectRoot);

		const auto Fixture = Output / "fixture";
		test::WriteScalePackage(Fixture, QualifiedScaleObjects);
		auto SourceManifest = ParsePackageContentManifest(ReadText(Fixture / test::ScaleManifestReference));
		Require(SourceManifest.has_value() && SourceManifest->Entries.size() == 1,
			"Canonical scale fixture did not produce one content unit");
		const auto &SourceEntry = SourceManifest->Entries.front();
		auto SourceBytes = ReadText(Fixture / SourceEntry.BlobReference);
		Require(SourceEntry.Key == test::ScaleContentKey && SourceEntry.ObjectCount == QualifiedScaleObjects &&
			SourceBytes.size() == QualifiedScaleBytes && SourceEntry.CompressedBytes == QualifiedScaleBytes &&
			SourceEntry.UncompressedBytes == QualifiedScaleBytes &&
			AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(SourceBytes.data()),
				SourceBytes.size())) == SourceEntry.Digest,
			"Canonical scale fixture differs from the qualified 512-object/273032-byte unit");

		DiskFilesystem ProjectFilesystem(ProjectRoot);
		auto ProjectValue = Project::fromExisting(&ProjectFilesystem);
		auto World = std::make_shared<DataModel>();
		World->SetName("Foundation3LQualifiedScale");
		World->Root = ProjectRoot;
		World->Filesystem = &ProjectFilesystem;
		World->InitializeLoadedProjectRevision();
		(void)World->GetService("Workspace");
		test::AddScaleGameplay(World, true, true, QualifiedProducerPlayerId, true);
		const auto Revision = World->GetAuthoritativeRevision();
		auto Payload = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
		Require(Payload.ContentUnits.empty(), "Qualified scale bootstrap unexpectedly generated another content unit");
		Payload.ContentUnits.push_back({.Entry = SourceEntry, .Payload = std::move(SourceBytes)});
		Require(Payload.ContentUnits.size() == 1 && Payload.Identity.IsValid() && Payload.AuthoritativeRevision != 0,
			"Qualified scale package identity or content unit is invalid");
		const auto Expected = Payload.ContentUnits.front();
		for (const auto &[Runtime, Name] : {
			std::pair{PlayerRuntime, std::string_view("Player")},
			std::pair{ServerRuntime, std::string_view("Server")}}) {
			auto Result = PackageBuilder::Build({
				.Payload = Payload, .RuntimeDistributionRoot = Runtime,
				.OutputDirectory = Output / Name, .Configuration = PackageConfiguration::Release,
			});
			Require(Result.Ok && !HasError(Result.Diagnostics), "Qualified scale runtime package build failed");
		}
		const auto PlayerManifest = CheckPackage(Output / "Player", Expected);
		const auto ServerManifest = CheckPackage(Output / "Server", Expected);
		Require(PlayerManifest.Package == ServerManifest.Package &&
			EncodePackageContentManifest(PlayerManifest) == EncodePackageContentManifest(ServerManifest),
			"Player and Server qualified scale content manifests differ");

		nlohmann::ordered_json Descriptor{
			{"format", "GargantuanQualifiedScalePackage"}, {"version", 1},
			{"project_id", Payload.Identity.ToString()}, {"revision", Payload.AuthoritativeRevision},
			{"player_package", "Player"},
			{"server_package", "Server"},
			{"content_key", Expected.Entry.Key}, {"content_blob", Expected.Entry.BlobReference},
			{"content_objects", QualifiedScaleObjects}, {"content_bytes", QualifiedScaleBytes},
			{"content_digest", Expected.Entry.Digest.ToString()},
			{"producer_policy", "first_server_session_player"},
			{"producer_player_id", QualifiedProducerPlayerId},
			{"expected_clients", 32},
			{"phases", {"baseline", "load", "resident", "evict", "reload"}},
		};
		std::ofstream DescriptorOutput(Output / "qualification-descriptor.json", std::ios::binary);
		DescriptorOutput << Descriptor.dump(2) << '\n';
		Require(static_cast<bool>(DescriptorOutput), "Could not write qualified scale package descriptor");
		std::cout << "[Content:ScalePackage] project=" << Payload.Identity.ToString()
			<< " revision=" << Payload.AuthoritativeRevision << " objects=" << QualifiedScaleObjects
			<< " bytes=" << QualifiedScaleBytes << " digest=" << Expected.Entry.Digest.ToString()
			<< " producer_player_id=" << QualifiedProducerPlayerId << " output=" << Output.string() << '\n';
	}
}

int main(int Count, char **Arguments) {
	if (Count != 4) {
		std::cerr << "[Content:ScalePackage] Usage: QualifiedScalePackage <new-output-root> <player-runtime-distribution> <server-runtime-distribution>\n";
		return 2;
	}
	if (!SDL_Init(0)) {
		std::cerr << "[Content:ScalePackage] SDL initialization failed\n";
		return 3;
	}
	try {
		BuildPair(Filesystem::path(Arguments[1]), Filesystem::path(Arguments[2]), Filesystem::path(Arguments[3]));
		SDL_Quit();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:ScalePackage] " << Error.what() << '\n';
		SDL_Quit();
		return 1;
	}
}
