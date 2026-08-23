#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
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

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
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
		if (!Output) throw std::runtime_error("Could not write asset test fixture");
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Text) {
		WriteBytes(Path, std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size()));
	}

	std::shared_ptr<gargantuan::AssetService> GetAssets(const std::shared_ptr<gargantuan::DataModel> &World) {
		return std::dynamic_pointer_cast<gargantuan::AssetService>(World->GetService("AssetService"));
	}
}

int main() {
	using namespace gargantuan;
	BootstrapNativeRuntimeSchema();

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
	WriteText(Root / "assets" / "bad.png", "not a png");
	WriteText(Root / "assets" / "bad.ttf", "not a font");
	std::filesystem::copy_file(std::filesystem::path(GARGANTUAN_ASSET_TEST_FONT_PATH),
		Root / "assets" / "font.ttf", std::filesystem::copy_options::overwrite_existing);
	std::filesystem::copy_file(std::filesystem::path(GARGANTUAN_ASSET_TEST_PNG_PATH),
		Root / "assets" / "representative.png", std::filesystem::copy_options::overwrite_existing);

	DiskFilesystem Filesystem(Root);
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
	Check(PngImport.Ok && PngImport.Record && PngImport.Record->Asset &&
		std::get<ImportedImage>(*PngImport.Record->Asset).Width == 512,
		"representative PNG input passes header preflight and canonical RGBA decode");
	auto MalformedImage = Assets->ImportProjectAsset(Mount, "assets/bad.png", AssetKind::Image, "Bad Image");
	Check(!MalformedImage.Ok && MalformedImage.Diagnostic.Code == "MalformedImage",
		"malformed image input fails with a structured diagnostic");

	auto MeshImport = Assets->ImportProjectAsset(Mount, "assets/triangle.obj", AssetKind::Mesh, "Triangle");
	auto Mesh = MeshImport.Record ? Assets->ResolveMesh(MeshImport.Record->Reference.Value) : std::nullopt;
	Check(MeshImport.Ok && Mesh && Mesh->Vertices && Mesh->Vertices->size() == 3 &&
		Mesh->Indices && Mesh->Indices->size() == 3 && Mesh->SubmeshCount == 1,
		"OBJ import validates and normalizes bounded renderer-neutral mesh data");
	auto InvalidIndex = Assets->ImportProjectAsset(Mount, "assets/bad-index.obj", AssetKind::Mesh, "Bad Index");
	auto NonFinite = Assets->ImportProjectAsset(Mount, "assets/non-finite.obj", AssetKind::Mesh, "Non-finite");
	Check(!InvalidIndex.Ok && InvalidIndex.Diagnostic.Code == "InvalidMeshIndex" &&
		!NonFinite.Ok && NonFinite.Diagnostic.Code == "MalformedMesh",
		"OBJ import rejects invalid indices and non-finite attributes");
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
			PlayAssets->ResolveFont(FontImport.Record->Reference.Value).has_value(),
			"Play clones the captured image, mesh, and font asset revision");
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
	Check(ReopenedAssets && ReopenedAssets->IsAvailable(PersistentImageReference) &&
		ReopenedAssets->ResolveMesh(MeshImport.Record->Reference.Value).has_value() &&
		ReopenedAssets->ResolveFont(FontImport.Record->Reference.Value).has_value(),
		"saved image, mesh, and font assets survive restart/reload");
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
		std::ranges::contains(Handshake["Result"]["Capabilities"], "StrictAssetReferences"),
		"EditorHost advertises the Asset Foundation command contract");
	auto Opened = Call("OpenProject", {{"Root", Root.generic_string()}});
	Check(Opened["Ok"], "EditorHost opens a persisted asset project");
	auto Catalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", true}});
	Check(Catalog["Ok"] && Catalog["Result"]["CatalogVersion"] == 1 &&
		std::ranges::any_of(Catalog["Result"]["Assets"], [&](const auto &Record) {
			return Record["Reference"] == PersistentImageReference && Record["State"] == "Ready";
		}), "EditorHost exposes bounded catalog metadata without raw resource ownership");
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
	std::cout << "All Asset Foundation 1 tests passed\n";
	return 0;
}
