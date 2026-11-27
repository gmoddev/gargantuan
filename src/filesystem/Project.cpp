#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/assets/GasDependencies.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "serialization/JsonCodec.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/Profiler.hpp"
#include "terrain/ScenePersistence.hpp"
#include "terrain/TerrainProfile.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <magic_enum/magic_enum.hpp>
#include <chrono>
#include <atomic>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace gargantuan {
	using InstanceFormat = InstanceSerialization::InstanceFormat;

	struct Project::DurableTerrainState {
		std::atomic<std::shared_ptr<const terrain::TerrainCaptureIndex>> Index;
	};

	namespace {
		using Json = nlohmann::ordered_json;
		constexpr std::string_view ProjectMetadataFilename = "project.json";
		constexpr std::string_view ProjectAssetCatalogPath = ".gargantuan/assets/catalog.json";
		constexpr std::uint32_t ProjectMetadataVersion = 1;
		constexpr auto FileLinkClass = SchemaId::FromNativeName("Engine", "FileLink");
		enum class AtomicWriteMode { Replace, Create };

		void PersistBytesAtomically(const std::filesystem::path &Destination, std::span<const std::uint8_t> Bytes,
			const std::function<void()> &BeforeReplace = {}, AtomicWriteMode Mode = AtomicWriteMode::Replace) {
			std::filesystem::create_directories(Destination.parent_path());
			static std::atomic_uint64_t Counter = 1;
			// A short sibling keeps the write on the same volume without
			// extending names such as the 64-character asset digests.
#if defined(_WIN32)
			const auto ProcessId = GetCurrentProcessId();
#else
			const auto ProcessId = getpid();
#endif
			const auto Temporary = Destination.parent_path() / std::format(
				".saving-{}-{}-{}",
				ProcessId,
				std::chrono::steady_clock::now().time_since_epoch().count(),
				Counter.fetch_add(1, std::memory_order_relaxed)
			);
			try {
				{
					std::ofstream Output(Temporary, std::ios::binary | std::ios::trunc);
					if (!Output) throw std::runtime_error("Could not open a temporary project file");
					Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
					Output.flush();
					if (!Output) throw std::runtime_error("Could not write a temporary project file");
					Output.close();
					if (!Output) throw std::runtime_error("Could not close a temporary project file");
				}
				if (BeforeReplace) BeforeReplace();
#if defined(_WIN32)
				const DWORD Flags = MOVEFILE_WRITE_THROUGH |
					(Mode == AtomicWriteMode::Replace ? MOVEFILE_REPLACE_EXISTING : 0);
				if (!MoveFileExW(Temporary.c_str(), Destination.c_str(), Flags))
					throw std::runtime_error(std::format("Atomic project replacement failed ({})", GetLastError()));
#else
				if (Mode == AtomicWriteMode::Replace) {
					std::filesystem::rename(Temporary, Destination);
				} else {
					// Linking in the same directory publishes the complete file and fails if the name already exists.
					std::filesystem::create_hard_link(Temporary, Destination);
					std::error_code Ignored;
					std::filesystem::remove(Temporary, Ignored);
				}
#endif
			} catch (...) {
				std::error_code Ignored;
				std::filesystem::remove(Temporary, Ignored);
				throw;
			}
		}

		void PersistStringAtomically(const std::filesystem::path &Destination, std::string_view Text) {
			PersistBytesAtomically(Destination, std::span(
				reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size()
			));
		}

		terrain::TerrainArtifactReader GetProjectTerrainReader(BaseFilesystem &Filesystem) {
			return [&Filesystem, Mount = std::optional<SourceMount>{}](const terrain::TerrainArtifactId &Id,
				std::size_t Maximum) mutable ->
				std::expected<std::vector<std::uint8_t>, terrain::TerrainPersistenceError> {
				using enum terrain::TerrainPersistenceError;
				const auto Path = terrain::GetTerrainArtifactPath(Id);
				if (!Path) return std::unexpected(InvalidBinding);
				if (!Mount) Mount.emplace(Filesystem);
				// The terrain manifest bounds the complete closure. SourceMount bounds and confines each read.
				SourceMountBudget Budget;
				const auto Bytes = Mount->ReadFile(*Path, Maximum, Budget);
				if (Bytes) return std::vector<std::uint8_t>(Bytes->begin(), Bytes->end());
				switch (Bytes.error().Code) {
					case SourceMountErrorCode::Missing: return std::unexpected(MissingArtifact);
					case SourceMountErrorCode::FileSizeLimit: return std::unexpected(LimitExceeded);
					default: return std::unexpected(ReadFailed);
				}
			};
		}

		void RequirePlainPersistenceDirectory(const std::filesystem::path &Path) {
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || !std::filesystem::is_directory(Status))
				throw std::runtime_error("[Project:Persistence] Project storage is not an accessible directory");
#if defined(_WIN32)
			const auto Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				throw std::runtime_error("[Project:Persistence] Project storage cannot traverse a reparse point");
#endif
		}

		std::string EncodeProjectMetadata(ProjectId Identity) {
			Json Document{{"Version", ProjectMetadataVersion}, {"ProjectId", Identity.ToString()}};
			auto Encoded = JsonCodec::Encode(Document, "project metadata");
			if (!Encoded) throw std::runtime_error(Encoded.error().Format());
			return std::move(*Encoded);
		}

		ProjectId DecodeProjectIdentity(std::string_view Metadata) {
			auto Parsed = JsonCodec::Parse(Metadata, 4096, "project metadata");
			if (!Parsed || !Parsed->is_object() || Parsed->size() != 2 ||
				!Parsed->contains("Version") || !(*Parsed)["Version"].is_number_unsigned() ||
				(*Parsed)["Version"].get<std::uint32_t>() != ProjectMetadataVersion ||
				!Parsed->contains("ProjectId") || !(*Parsed)["ProjectId"].is_string())
				throw std::runtime_error("Project metadata format is invalid or unsupported");
			auto Identity = ProjectId::Parse((*Parsed)["ProjectId"].get_ref<const std::string &>());
			if (!Identity) throw std::runtime_error("Project metadata identity is invalid");
			return *Identity;
		}

		ProjectId ReadProjectIdentity(BaseFilesystem &Filesystem, const std::filesystem::path &ConfigurationRoot,
			const std::filesystem::path &InstanceFile, InstanceFormat Format) {
			RequirePlainPersistenceDirectory(ConfigurationRoot);
			const auto Path = ConfigurationRoot / ProjectMetadataFilename;
			SourceMount Mount(Filesystem);
			SourceMountBudget Budget;
			const auto Relative = Path.lexically_relative(Filesystem.Root);
			const auto Metadata = Mount.ReadFile(Relative, 4096, Budget);
			if (Metadata) return DecodeProjectIdentity(*Metadata);
			if (Metadata.error().Code != SourceMountErrorCode::Missing)
				throw std::runtime_error(Metadata.error().Format());
			if (Format == InstanceFormat::Json) {
				const auto Scene = Mount.ReadFile(InstanceFile.lexically_relative(Filesystem.Root),
					MaximumProtocolDocumentBytes, Budget);
				if (!Scene) throw std::runtime_error(Scene.error().Format());
				const auto Terrain = InstanceSerialization::InspectTerrainManifest(*Scene);
				if (!Terrain) throw std::runtime_error(Terrain.error());
				if (*Terrain)
					throw std::runtime_error("[Terrain:Persistence] Restore the original project metadata before opening this terrain scene");
			}
			const auto Identity = ProjectId::New();
			const auto Text = EncodeProjectMetadata(Identity);
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size());
			try {
				PersistBytesAtomically(Path, Bytes, [&] { RequirePlainPersistenceDirectory(ConfigurationRoot); },
					AtomicWriteMode::Create);
			} catch (...) {
				SourceMountBudget RetryBudget;
				const auto Existing = Mount.ReadFile(Relative, 4096, RetryBudget);
				if (!Existing) throw;
				return DecodeProjectIdentity(*Existing);
			}
			return Identity;
		}

		bool HasPortableSourcePaths(const Gas::Document &Document) {
			for (const auto &Node : Document.Nodes) {
				if (Node.Class != FileLinkClass) continue;
				const auto Found = Node.SavedProperties.find("Path");
				if (Found == Node.SavedProperties.end()) continue;
				const auto *Wire = std::get_if<WireValue>(&Found->second);
				const auto *Text = Wire ? std::get_if<std::string>(Wire) : nullptr;
				if (!Text || Text->size() > MaximumSourceMountPathBytes || !IsValidProtocolUtf8(*Text) ||
					Text->find('\0') != std::string::npos || Text->starts_with('/') ||
					Text->find_first_of("\\:") != std::string::npos)
					return false;
				const auto Relative = std::filesystem::u8path(*Text);
				if (std::ranges::find(Relative, std::filesystem::path("..")) != Relative.end()) return false;
			}
			return true;
		}

		bool HasCanonicalServices(const Gas::Document &Document, const DataModel &World) {
			std::set<SchemaId> Seen;
			for (std::size_t Index = 1; Index < Document.Nodes.size(); ++Index) {
				const auto &Node = Document.Nodes[Index];
				if (World.IsProtectedServiceClass(Node.Class) &&
					(Node.Parent != 1 || !Seen.insert(Node.Class).second)) return false;
			}
			return true;
		}

		void ValidateGasExportDestination(const std::filesystem::path &Destination) {
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Destination, Error);
			if (Status.type() == std::filesystem::file_type::not_found &&
				(!Error || Error == std::errc::no_such_file_or_directory)) return;
			if (Error || !std::filesystem::is_regular_file(Status))
				throw std::invalid_argument("GAS export destination is not a regular place file");
#if defined(_WIN32)
			const auto Attributes = GetFileAttributesW(Destination.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				throw std::invalid_argument("GAS export destination is redirected");
#endif
			std::ifstream Existing(Destination, std::ios::binary | std::ios::ate);
			const auto Length = Existing.tellg();
			if (Length <= 0 || Length > static_cast<std::streamoff>(Gas::MaximumFileBytes))
				throw std::invalid_argument("Existing GAS export destination has an invalid length");
			Existing.seekg(0);
			if (Length>static_cast<std::streamoff>(Gas::MaximumDocumentBytes)) {
				std::array<std::uint8_t,Gas::FilePrefixBytes> Prefix{};
				if (!Existing.read(reinterpret_cast<char*>(Prefix.data()),Prefix.size()) ||
					!Gas::AdmitsFileLength(Prefix,static_cast<std::uintmax_t>(Length)))
					throw std::invalid_argument("Existing GAS export destination exceeds its document profile");
				Existing.seekg(0);
			}
			Gas::Binary Bytes(static_cast<std::size_t>(Length));
			if (!Existing.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())) ||
				Existing.peek() != std::char_traits<char>::eof())
				throw std::runtime_error("Could not read the existing GAS export destination");
			const auto Document = Gas::Decode(Bytes);
			if (!Document || Document->Kind != Gas::DocumentKind::Place)
				throw std::invalid_argument("Existing GAS export destination does not hold a valid place document");
		}

		void RequireAbsentInstanceFile(const std::filesystem::path &Path) {
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Status.type() != std::filesystem::file_type::not_found ||
				(Error && Error != std::errc::no_such_file_or_directory))
				throw std::invalid_argument("GAS place restore requires an origin without a saved Instance file");
#if defined(_WIN32)
			if (GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES)
				throw std::invalid_argument("GAS place restore destination is already in use");
#endif
		}
	}

	std::optional<std::tuple<std::filesystem::path, InstanceSerialization::InstanceFormat>>
	ResolveInstanceFile(std::filesystem::path rootConfiguration) {
		SDL_PathInfo binaryInfo;
		std::filesystem::path binaryPath = rootConfiguration / "project.instance.bin";
		if (SDL_GetPathInfo(Paths::ToUtf8(binaryPath).c_str(), &binaryInfo) && binaryInfo.type == SDL_PATHTYPE_FILE) {
			return std::tuple{binaryPath, InstanceFormat::Binary};
		}

		SDL_PathInfo jsonInfo;
		std::filesystem::path jsonPath = rootConfiguration / "project.instance.json";
		if (SDL_GetPathInfo(Paths::ToUtf8(jsonPath).c_str(), &jsonInfo) && jsonInfo.type == SDL_PATHTYPE_FILE) {
			return std::tuple{jsonPath, InstanceFormat::Json};
		}

		return std::nullopt;
	}

	std::string GetProjectInstanceFilename(InstanceFormat format) {
		return std::format("project.instance.{}", format == InstanceFormat::Json ? "json" : "bin");
	}

	Project::Project(BaseFilesystem *fs)
		: Root(fs->Root), RootConfiguration(fs->Root / ".gargantuan"), Filesystem(fs),
			DurableTerrain(std::make_shared<DurableTerrainState>()) {}

	Project Project::fromInit(
		BaseFilesystem *fs, std::string projectName, std::shared_ptr<Instance> instance, InstanceFormat format
	) {
		Project self(fs);
		if (!SDL_CreateDirectory(Paths::ToUtf8(self.RootConfiguration).c_str())) {
			throw std::runtime_error(std::format("Failed to create .gargantuan directory: {}", SDL_GetError()));
		}
		self.Identity = ProjectId::New();
		self.Filesystem->WriteStringToFile(
			self.RootConfiguration / ProjectMetadataFilename,
			EncodeProjectMetadata(self.Identity)
		);

		std::string instanceFileContents;
		self.InstanceFilePath = self.RootConfiguration / GetProjectInstanceFilename(format);
		self.InstanceFileFormat = format;
		if (instance) {
			blockgrid::SegmentCapture Grids;
			instanceFileContents=InstanceSerialization::Serialize(format,instance,{},{},&Grids);
			self.PersistGridSegments(instanceFileContents,Grids.Parts);
		} else if (format == InstanceFormat::Json) {
			instanceFileContents = InstanceSerialization::SerializeEmptyProject(format, projectName);
		} else if (format == InstanceFormat::Binary) {
			throw std::runtime_error("Binary instance formats are not yet implemented");
		}

		self.Filesystem->WriteStringToFile(self.InstanceFilePath, instanceFileContents);

		return self;
	}

	Project Project::fromExisting(BaseFilesystem *fs) {
		Project self(fs);

		SDL_PathInfo configurationInfo;
		if (!SDL_GetPathInfo(Paths::ToUtf8(self.RootConfiguration).c_str(), &configurationInfo)) {
			throw std::runtime_error(std::format("Failed to open .gargantuan directory: {}", SDL_GetError()));
		} else if (configurationInfo.type != SDL_PATHTYPE_DIRECTORY) {
			auto pathType = magic_enum::enum_name(configurationInfo.type);
			throw std::runtime_error(std::format("Expected .gargantuan to be a directory, got {}", pathType));
		}

		auto resolvedInstance = ResolveInstanceFile(self.RootConfiguration);
		if (!resolvedInstance) {
			throw std::runtime_error("Failed to resolve the project's instance file inside .gargantuan");
		}

		auto [instanceFilePath, instanceFileFormat] = resolvedInstance.value();
		self.InstanceFilePath = instanceFilePath;
		self.InstanceFileFormat = instanceFileFormat;
		self.Identity = ReadProjectIdentity(*self.Filesystem, self.RootConfiguration, self.InstanceFilePath,
			self.InstanceFileFormat);

		return self;
	}

	Project Project::forDestination(BaseFilesystem *fs, InstanceFormat format) {
		Project self(fs);
		self.InstanceFileFormat = format;
		self.InstanceFilePath = self.RootConfiguration / GetProjectInstanceFilename(format);
		self.Identity = ProjectId::New();
		return self;
	}

	Project Project::FromGasOrigin(BaseFilesystem *Filesystem) {
		if (!Filesystem) throw std::invalid_argument("GAS origin requires a project filesystem");
		Project Origin(Filesystem);
		SourceMount Mount(*Filesystem);
		SourceMountBudget Budget;
		const auto Metadata = Mount.ReadFile(std::filesystem::path(".gargantuan") / ProjectMetadataFilename, 4096, Budget);
		if (!Metadata) throw std::runtime_error("Could not read existing GAS origin metadata");
		Origin.Identity = DecodeProjectIdentity(*Metadata);
		Origin.InstanceFileFormat = InstanceFormat::Json;
		Origin.InstanceFilePath = Origin.RootConfiguration / GetProjectInstanceFilename(InstanceFormat::Json);
		return Origin;
	}

	Project::PreparedGame Project::PrepareGameLoad() const {
		if (InstanceFileFormat != InstanceFormat::Json)
			throw std::runtime_error("Binary instance formats are not yet implemented");
		PreparedGame Result;
		Result.Source = Filesystem;
		Result.Identity = Identity;
		SourceMount Mount(*Filesystem);
		SourceMountBudget Budget;
		auto Contents = Mount.ReadFile(InstanceFilePath.lexically_relative(Root), MaximumProtocolDocumentBytes, Budget);
		if (!Contents) throw std::runtime_error(Contents.error().Format());
		blockgrid::SegmentRequirements Required;
		auto Manifest = InstanceSerialization::InspectTerrainManifest(*Contents, &Result.Bindings,&Required);
		if (!Manifest) throw std::runtime_error(Manifest.error());
		Result.Contents = std::move(*Contents);
		Result.GridSegments=blockgrid::ReadSegments(Required,[&](const AssetContentId& Id,std::size_t Size) {
			SourceMountBudget SegmentBudget;
			const auto Bytes=Mount.ReadFile(std::filesystem::path(".gargantuan")/"blockgrid"/blockgrid::SegmentFileName(Id),Size,SegmentBudget);
			if (!Bytes) throw std::runtime_error(Bytes.error().Format());
			return blockgrid::Segment{std::make_shared<const blockgrid::SegmentData>(Bytes->begin(),Bytes->end())};
		});
		Result.Manifest = *Manifest;
		const auto Reader = terrain::BudgetDocumentTerrainReads(GetProjectTerrainReader(*Filesystem));
		if (Result.Manifest)
			Result.Terrain = terrain::LoadSceneTerrain(Identity, *Result.Manifest, Reader);
		for (const auto &[Node, Digest] : Result.Bindings)
			Result.DomainFields.emplace(Node, terrain::LoadDomainTerrain(Identity, Node, Digest, Reader));
		return Result;
	}

	std::shared_ptr<DataModel> Project::DeserializeGame() {
		return DeserializeGame(PrepareGameLoad());
	}

	std::shared_ptr<DataModel> Project::DeserializeGame(PreparedGame Prepared) {
		if (Prepared.Source != Filesystem || Prepared.Identity != Identity)
			throw std::invalid_argument("Prepared scene differs from its source project");
		std::istringstream stream(std::move(Prepared.Contents));
		auto deserialized = InstanceSerialization::Deserialize(InstanceFileFormat, stream,
			[&](DataModel &World, const AssetContentId &Digest) {
				if (Prepared.Manifest != Digest || !Prepared.Terrain)
					throw terrain::TerrainPersistenceFailure(terrain::TerrainPersistenceError::InvalidBinding);
				const auto Space = std::dynamic_pointer_cast<Workspace>(World.GetService("Workspace"));
				if (!Space) throw std::runtime_error("[Terrain:Persistence] Expected a canonical Workspace");
				Space->RestoreTerrainField(std::move(Prepared.Terrain));
			}, [&](SpatialRegion &Region, std::uint32_t Node, const AssetContentId &Digest) {
				const auto It = Prepared.DomainFields.find(Node);
				if (It == Prepared.DomainFields.end() || !It->second || Prepared.Bindings.at(Node) != Digest)
					throw terrain::TerrainPersistenceFailure(terrain::TerrainPersistenceError::InvalidBinding);
				Region.RestoreTerrainField(std::move(It->second));
			},&Prepared.GridSegments);
		if (!deserialized.Ok) {
			std::ostringstream err;
			err << "Failed to deserialize instance file:" << std::endl;
			for (auto &reason : deserialized.Errors) {
				err << "* " << reason << std::endl;
			}
			throw std::runtime_error(err.str());
		}
		try {
			auto game = std::dynamic_pointer_cast<DataModel>(deserialized.Instance);
			if (!game)
				throw std::runtime_error(std::format("Expected project instance to be a DataModel, got a {}",
					deserialized.Instance->CLASS_DEFINITION.ClassName));
			game->MarkPersistenceSubtreeArchivable();
			game->Root = Root;
			game->Filesystem = Filesystem;
			if (Filesystem->Exists(Root / ProjectAssetCatalogPath)) {
				auto Assets = std::dynamic_pointer_cast<AssetService>(game->GetService("AssetService"));
				if (!Assets) throw std::runtime_error("AssetService schema resolved to an incompatible native service");
				Assets->LoadProjectAssets(*Filesystem);
			}
			return game;
		} catch (...) {
			deserialized.Instance->Destroy();
			throw;
		}
	};

	SerializationResult<Gas::Document> Project::CaptureGasPlace(const std::shared_ptr<DataModel> &World,
		std::optional<terrain::TerrainCapture> *CapturedTerrain, terrain::DomainTerrainCaptures *CapturedDomains) const {
		try {
			AssertAuthoritativeMutation("CaptureGasPlace");
			RequireFrozenRuntimeSchema("CaptureGasPlace");
			if (!Identity.IsValid() || !World || World->GetDestroyed() || World->IsDestroying() ||
				World->Filesystem != Filesystem || World->Root != Root)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Expected a world owned by this project");
			auto Terrain = terrain::CaptureSceneTerrain(*World, Identity);
			auto Domains = terrain::CaptureDocumentTerrain(World, Identity, Terrain ? Terrain->ClosureBytes : 0);
			if ((Terrain && !CapturedTerrain) || (!Domains.empty() && !CapturedDomains))
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Terrain place capture requires its artifact closure");
			const std::array<std::shared_ptr<Instance>, 1> Roots{World};
			auto Document = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Place,
				Gas::PlaceMetadata{Identity, ReadProjectPreRunSource(Root),
					Terrain ? std::optional(Terrain->ManifestDigest) : std::nullopt, terrain::GetDomainTerrainBindings(Domains)});
			if (!Document) return Document;
			if (!HasPortableSourcePaths(*Document))
				return SerializationFailure(SerializationErrorCode::InvalidValue, "GAS place has a nonportable FileLink path");
			if (!HasCanonicalServices(*Document, *World))
				return SerializationFailure(SerializationErrorCode::InvalidValue, "GAS place has a misplaced or duplicate service");
			const auto Assets = std::dynamic_pointer_cast<AssetService>(World->FindFirstChildOfClass("AssetService", false));
			auto Dependencies = Gas::ResolveDependencies(*Document, Assets.get(), true);
			if (!Dependencies) return std::unexpected(std::move(Dependencies.error()));
			Document->Dependencies = std::move(*Dependencies);
			if (CapturedTerrain) *CapturedTerrain = std::move(Terrain);
			if (CapturedDomains) *CapturedDomains = std::move(Domains);
			return Document;
		} catch (const std::exception &) {
			return SerializationFailure(SerializationErrorCode::InternalFailure, "Could not capture GAS place");
		}
	}

	void Project::ExportGasPlace(const std::shared_ptr<DataModel> &World, const std::filesystem::path &Destination,
		const std::optional<std::string> &ExpectedPreRunSource) const {
		if (Destination.extension() != ".gasp") throw std::invalid_argument("GAS place export requires a .gasp destination");
		std::optional<terrain::TerrainCapture> CapturedTerrain;
		terrain::DomainTerrainCaptures Domains;
		const auto Document = CaptureGasPlace(World, &CapturedTerrain, &Domains);
		if (!Document) throw std::runtime_error(Document.error().Format());
		if (Document->Place->PreRunSource != ExpectedPreRunSource)
			throw PreRunSourceConflict("Project PreRun source differs from the initialized schema source");
		const auto Bytes = Gas::Encode(*Document);
		if (!Bytes) throw std::runtime_error(Bytes.error().Format());
		const auto Output = std::filesystem::absolute(Destination);
		ValidateGasExportDestination(Output);
		if (CapturedTerrain) PersistTerrainArtifacts(*CapturedTerrain);
		for (const auto &[Node, Capture] : Domains) PersistTerrainArtifacts(Capture, Node);
		PersistBytesAtomically(Output, *Bytes, [&] {
			if (ExpectedPreRunSource != ReadProjectPreRunSource(Root))
				throw PreRunSourceConflict("Project PreRun source changed during GAS export");
			ValidateGasExportDestination(Output);
		});
	}

	std::shared_ptr<DataModel> Project::LoadGasPlace(const Gas::Document &Document) const {
		AssertAuthoritativeMutation("LoadGasPlace");
		RequireFrozenRuntimeSchema("LoadGasPlace");
		if (Document.Kind != Gas::DocumentKind::Place || !Document.Place || !Identity.IsValid() ||
			Document.Place->Identity != Identity)
			throw std::invalid_argument("GAS place does not match the selected project identity");
		if (Document.Place->PreRunSource != ReadProjectPreRunSource(Root))
			throw std::invalid_argument("GAS place PreRun source differs from the selected project source");
		if (Document.Nodes.size() > Gas::MaximumInstances || !HasPortableSourcePaths(Document))
			throw std::invalid_argument("GAS place has an invalid hierarchy or FileLink path");

		ScopedChangeJournalSuppression Suppression;
		const auto Reader = terrain::BudgetDocumentTerrainReads(GetProjectTerrainReader(*Filesystem));
		auto Materialized = InstanceSerialization::MaterializeGasDocument(Document, true,
			terrain::GetSceneTerrainAdmission(Identity, Reader), terrain::GetDomainTerrainAdmission(Identity, Reader));
		try {
			if (!Materialized.Succeeded())
				throw std::runtime_error(Materialized.Errors.empty()
					? "Could not reconstruct the GAS place graph" : Materialized.Errors.front());
			auto World = std::dynamic_pointer_cast<DataModel>(Materialized.Roots.front());
			if (!World || Materialized.Roots.size() != 1)
				throw std::runtime_error("GAS place did not reconstruct one DataModel");
			if (!HasCanonicalServices(Document, *World))
				throw std::invalid_argument("GAS place has a misplaced or duplicate service");
			auto Assets = std::dynamic_pointer_cast<AssetService>(World->FindFirstChildOfClass("AssetService", false));
			if (Filesystem->Exists(Root / ProjectAssetCatalogPath)) {
				if (!Assets) Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
				if (!Assets) throw std::runtime_error("GAS place has no canonical AssetService");
				Assets->LoadProjectAssets(*Filesystem);
			}
			const auto Dependencies = Gas::ResolveDependencies(Document, Assets.get(), true);
			if (!Dependencies) throw std::runtime_error(Dependencies.error().Format());
			if (*Dependencies != Document.Dependencies)
				throw std::invalid_argument("GAS place asset pins differ from the selected project catalog");
			World->MarkPersistenceSubtreeArchivable();
			World->Root = Root;
			World->Filesystem = Filesystem;
			return World;
		} catch (...) {
			for (const auto &RootInstance : Materialized.Roots)
				if (RootInstance) RootInstance->Destroy();
			throw;
		}
	}

	void Project::RestoreGasPlace(const std::shared_ptr<DataModel> &World,
		const std::optional<std::string> &ExpectedPreRunSource) const {
		AssertAuthoritativeMutation("RestoreGasPlace");
		if (!World || World->GetDestroyed() || World->IsDestroying() || World->Root != Root ||
			World->Filesystem != Filesystem || !Identity.IsValid())
			throw std::invalid_argument("Expected a restored world from this GAS origin");
		// Resolve the canonical .gargantuan directory through the existing root-confinement check.
		const auto Configuration = ResolveProjectPreRunPath(Root).parent_path();
		const auto Destination = Configuration / GetProjectInstanceFilename(InstanceFormat::Json);
		const auto CheckDestination = [&] {
			if (ResolveProjectPreRunPath(Root).parent_path() != Configuration ||
				FromGasOrigin(Filesystem).Identity != Identity)
				throw std::runtime_error("GAS origin changed during place restore");
			if (ReadProjectPreRunSource(Root) != ExpectedPreRunSource)
				throw PreRunSourceConflict("Project PreRun source changed during place restore");
			RequireAbsentInstanceFile(Configuration / GetProjectInstanceFilename(InstanceFormat::Binary));
			RequireAbsentInstanceFile(Destination);
		};
		CheckDestination();
		const auto Snapshot = CaptureGame(World, World->GetAuthoritativeRevision());
		if (Snapshot.Contents.size() > MaximumProtocolDocumentBytes)
			throw std::length_error("Restored project exceeds the JSON persistence byte limit");
		if (Snapshot.Terrain) PersistTerrainArtifacts(*Snapshot.Terrain);
		PersistGridSegments(Snapshot.Contents,Snapshot.GridSegments);
		for (const auto &[Node, Capture] : Snapshot.Domains) PersistTerrainArtifacts(Capture, Node);
		PersistBytesAtomically(Destination, std::span(reinterpret_cast<const std::uint8_t *>(Snapshot.Contents.data()),
			Snapshot.Contents.size()),
			CheckDestination, AtomicWriteMode::Create);
	}

	Project::PersistenceSnapshot Project::CaptureGame(
		const std::shared_ptr<DataModel> &game,
		std::uint64_t revision,
		bool ReuseDurableTerrain
	) const {
		if (!game) throw std::invalid_argument("Cannot persist a null DataModel");
		if (revision == 0) throw std::invalid_argument("Cannot persist revision zero");
		const auto Prior = ReuseDurableTerrain ? DurableTerrain->Index.load() : nullptr;
		auto Terrain = terrain::CaptureSceneTerrain(*game, Identity, Prior.get());
		auto Domains = terrain::CaptureDocumentTerrain(game, Identity, Terrain ? Terrain->ClosureBytes : 0);
		auto root = std::static_pointer_cast<Instance>(game);
		blockgrid::SegmentCapture Grids;
		auto Contents = InstanceSerialization::Serialize(InstanceFileFormat, root,
			Terrain ? std::optional(Terrain->ManifestDigest) : std::nullopt, terrain::GetDomainTerrainBindings(Domains),&Grids);
		AssetProjectSnapshot AssetSnapshot;
		if (auto Assets = std::dynamic_pointer_cast<AssetService>(game->FindFirstChildOfClass("AssetService", false)))
			AssetSnapshot = Assets->CaptureProjectAssets();
		return {revision, std::move(Contents), std::move(AssetSnapshot), std::move(Terrain), std::move(Domains),std::move(Grids.Parts)};
	}
	void Project::PersistGridSegments(std::string_view Scene,const blockgrid::SegmentMap& Segments) const {
		const auto Required=InstanceSerialization::InspectGridSegments(Scene);
		blockgrid::VerifySegments(Required,Segments);
		if (Segments.empty()) return;
		const auto Dir=RootConfiguration/"blockgrid";
		RequirePlainPersistenceDirectory(RootConfiguration);
		std::filesystem::create_directory(Dir);
		const auto CheckDirectory=[&] { RequirePlainPersistenceDirectory(RootConfiguration);RequirePlainPersistenceDirectory(Dir); };
		CheckDirectory();
		SourceMount Mount(*Filesystem);
		for (const auto& [Id,Part]:Segments) {
			const auto File=Dir/blockgrid::SegmentFileName(Id);
			const auto Existing=[&] {
				SourceMountBudget Budget;
				const auto Bytes=Mount.ReadFile(File.lexically_relative(Root),Part.Bytes->size(),Budget);
				if (!Bytes) {
					if (Bytes.error().Code==SourceMountErrorCode::Missing) return false;
					throw std::runtime_error(Bytes.error().Format());
				}
				const std::span Data(reinterpret_cast<const std::uint8_t*>(Bytes->data()),Bytes->size());
				if (Data.size()!=Part.Bytes->size() || AssetContentId::Hash(Data)!=Id)
					throw std::runtime_error("[BlockGrid:Persistence] Existing segment differs from its content address");
				return true;
			};
			if (Existing()) continue;
			try { PersistBytesAtomically(File,*Part.Bytes,CheckDirectory,AtomicWriteMode::Create); }
			catch (...) { if (!Existing()) throw; }
		}
	}

	void Project::PersistTerrainArtifacts(const terrain::TerrainCapture &Capture, std::optional<std::uint32_t> DomainNode) const {
		G_PROFILE("Terrain::PersistProjectArtifacts");
		using namespace terrain;
		using enum TerrainPersistenceError;
		const TerrainArtifactId ManifestId{TerrainArtifactPurpose::Manifest, Capture.ManifestDigest};
		const auto *ManifestArtifact = FindTerrainArtifact(Capture.Artifacts, ManifestId);
		if (!ManifestArtifact || !ManifestArtifact->Bytes ||
			ManifestArtifact->Bytes->size() > MaximumTerrainManifestBytes ||
			AssetContentId::Hash(*ManifestArtifact->Bytes) != ManifestId.Digest)
			throw TerrainPersistenceFailure(CorruptArtifact);
		const auto Dataset = DomainNode ? GetDomainTerrainDatasetId(Identity, *DomainNode) : GetTerrainDatasetId(Identity);
		const auto Manifest = DecodeTerrainManifest(*ManifestArtifact->Bytes, Identity, Dataset);
		if (!Manifest) throw TerrainPersistenceFailure(InvalidBinding);
		if (Manifest->Chunks.size() > MaximumStartupSourceChunks) throw TerrainPersistenceFailure(LimitExceeded);
		std::size_t Bytes = ManifestArtifact->Bytes->size();
		for (const auto &Ref : Manifest->Chunks) {
			if (Ref.ByteCount > MaximumTerrainClosureBytes - Bytes)
				throw TerrainPersistenceFailure(LimitExceeded);
			Bytes += Ref.ByteCount;
		}
		std::filesystem::create_directories(Root);
		const auto Configuration = ResolveProjectPreRunPath(Root).parent_path();
		std::filesystem::create_directory(Configuration);
		RequirePlainPersistenceDirectory(Configuration);
		const auto Directory = Configuration / "terrain";
		std::filesystem::create_directory(Directory);
		RequirePlainPersistenceDirectory(Directory);
		for (const auto *Leaf : {"chunks", "manifests"}) {
			std::filesystem::create_directory(Directory / Leaf);
			RequirePlainPersistenceDirectory(Directory / Leaf);
		}
		const auto Reader = GetProjectTerrainReader(*Filesystem);
		const auto Store = [&](const TerrainArtifactId &Id, std::size_t Size) {
			const auto Existing = [&] {
				const auto Stored = Reader(Id, Size);
				if (!Stored) {
					if (Stored.error() == MissingArtifact) return false;
					throw TerrainPersistenceFailure(Stored.error());
				}
				if (Stored->size() != Size || AssetContentId::Hash(*Stored) != Id.Digest)
					throw TerrainPersistenceFailure(CorruptArtifact);
				return true;
			};
			if (Existing()) return;
			const auto *Artifact = FindTerrainArtifact(Capture.Artifacts, Id);
			if (!Artifact || !Artifact->Bytes) throw TerrainPersistenceFailure(MissingArtifact);
			if (Artifact->Bytes->size() != Size || AssetContentId::Hash(*Artifact->Bytes) != Id.Digest)
				throw TerrainPersistenceFailure(CorruptArtifact);
			const auto Path = *GetTerrainArtifactPath(Id);
			const auto Destination = Configuration.parent_path() / Path;
			const auto CheckDirectory = [&] {
				if (ResolveProjectPreRunPath(Root).parent_path() != Configuration)
					throw TerrainPersistenceFailure(ReadFailed);
				RequirePlainPersistenceDirectory(Directory);
				RequirePlainPersistenceDirectory(Destination.parent_path());
			};
			CheckDirectory();
			try {
				PersistBytesAtomically(Destination, *Artifact->Bytes, CheckDirectory, AtomicWriteMode::Create);
			} catch (...) {
				// A concurrent save can publish the same immutable artifact first.
				if (!Existing()) throw;
			}
		};
		for (const auto &Ref : Manifest->Chunks)
			Store({TerrainArtifactPurpose::Chunk, AssetContentId{Ref.Digest.Bytes}}, Ref.ByteCount);
		Store(ManifestId, ManifestArtifact->Bytes->size());
	}

	std::optional<AssetContentId> Project::GetPersistedTerrainManifest(terrain::DomainTerrainBindings *Domains) const {
		if (Domains) Domains->clear();
		if (InstanceFileFormat != InstanceFormat::Json || !Filesystem->Exists(InstanceFilePath)) return std::nullopt;
		SourceMount Mount(*Filesystem);
		SourceMountBudget Budget;
		const auto Contents = Mount.ReadFile(InstanceFilePath.lexically_relative(Root), MaximumProtocolDocumentBytes, Budget);
		if (!Contents) throw std::runtime_error(Contents.error().Format());
		const auto Manifest = InstanceSerialization::InspectTerrainManifest(*Contents, Domains);
		if (!Manifest) throw std::runtime_error(Manifest.error());
		return *Manifest;
	}

	void Project::ValidatePersistenceDestination() const {
		if (!Identity.IsValid()) throw std::invalid_argument("Cannot persist an invalid project identity");
		if (std::filesystem::exists(RootConfiguration)) RequirePlainPersistenceDirectory(RootConfiguration);
		const auto Metadata = RootConfiguration / ProjectMetadataFilename;
		if (Filesystem->Exists(Metadata) && FromGasOrigin(Filesystem).Identity != Identity)
			throw std::runtime_error("Project destination belongs to a different project identity");
	}

	void Project::PersistGameAtomically(
		const PersistenceSnapshot &snapshot,
		const std::function<void()> &beforeReplace
	) const {
		if (snapshot.Revision == 0) throw std::invalid_argument("Cannot persist revision zero");
		ValidatePersistenceDestination();
		if (InstanceFileFormat == InstanceFormat::Binary)
			throw std::runtime_error("Binary instance formats are not yet implemented");
		terrain::DomainTerrainBindings Domains;
		const auto Digest = InstanceSerialization::InspectTerrainManifest(snapshot.Contents, &Domains);
		if (!Digest || *Digest != (snapshot.Terrain ? std::optional(snapshot.Terrain->ManifestDigest) : std::nullopt))
			throw std::runtime_error("[Terrain:Persistence] Project scene and terrain closure do not match");
		std::shared_ptr<const terrain::TerrainCaptureIndex> Index;
		if (Domains != terrain::GetDomainTerrainBindings(snapshot.Domains))
			throw std::runtime_error("[Terrain:Persistence] Scene and domain terrain closures differ");
		for (const auto &[Node, Capture] : snapshot.Domains) PersistTerrainArtifacts(Capture, Node);
		if (snapshot.Terrain) {
			Index = std::make_shared<const terrain::TerrainCaptureIndex>(snapshot.Terrain->Index);
			PersistTerrainArtifacts(*snapshot.Terrain);
		}
		if (beforeReplace) beforeReplace();
		ValidatePersistenceDestination();
		const auto Metadata = RootConfiguration / ProjectMetadataFilename;
		const auto CheckIdentity = [&] {
			if (FromGasOrigin(Filesystem).Identity != Identity)
				throw std::runtime_error("Project identity changed during persistence");
		};
		if (!Filesystem->Exists(Metadata)) {
			const auto Encoded = EncodeProjectMetadata(Identity);
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
			try {
				PersistBytesAtomically(Metadata, Bytes, [&] { RequirePlainPersistenceDirectory(RootConfiguration); },
					AtomicWriteMode::Create);
			} catch (...) {
				// A second save may establish the project first. Accept only an identical identity.
				if (!Filesystem->Exists(Metadata)) throw;
			}
		}
		CheckIdentity();
		PersistGridSegments(snapshot.Contents,snapshot.GridSegments);
		for (const auto &Artifact : snapshot.Assets.Artifacts) {
			if (!Artifact.Bytes || Artifact.RelativePath.empty())
				throw std::runtime_error("Asset persistence snapshot contains an invalid artifact");
			PersistBytesAtomically(Root / std::filesystem::path(Artifact.RelativePath), *Artifact.Bytes);
		}
		if (!snapshot.Assets.CatalogJson.empty())
			PersistStringAtomically(Root / ".gargantuan" / "assets" / "catalog.json", snapshot.Assets.CatalogJson);
		PersistBytesAtomically(InstanceFilePath,
			std::span(reinterpret_cast<const std::uint8_t *>(snapshot.Contents.data()), snapshot.Contents.size()),
			CheckIdentity);
		DurableTerrain->Index.store(std::move(Index));
	}

	bool Project::ReplacePreRunSource(const std::optional<std::string> &ExpectedSource,
		std::string_view Source, const std::function<void()> &BeforeReplace) const {
		ValidateProtocolString(Source, MaximumPreRunSourceBytes, "PreRun source");
		const auto Destination = ResolveProjectPreRunPath(Root);
		if (ReadProjectPreRunSource(Root) != ExpectedSource) return false;
		PersistBytesAtomically(Destination, std::span(reinterpret_cast<const std::uint8_t *>(Source.data()), Source.size()), [&] {
			if (BeforeReplace) BeforeReplace();
			if (ResolveProjectPreRunPath(Root) != Destination || ReadProjectPreRunSource(Root) != ExpectedSource)
				throw PreRunSourceConflict("PreRun source changed during atomic replacement");
		});
		return true;
	}
}
