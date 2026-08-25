#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
#include <format>
#include <fstream>
#include <magic_enum/magic_enum.hpp>
#include <chrono>
#include <atomic>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <tuple>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace gargantuan {
	using InstanceFormat = InstanceSerialization::InstanceFormat;

	namespace {
		using Json = nlohmann::ordered_json;
		constexpr std::string_view ProjectMetadataFilename = "project.json";
		constexpr std::uint32_t ProjectMetadataVersion = 1;

		void PersistBytesAtomically(const std::filesystem::path &Destination, std::span<const std::uint8_t> Bytes) {
			std::filesystem::create_directories(Destination.parent_path());
			static std::atomic_uint64_t Counter = 1;
			const auto Suffix = std::format(
				".saving-{}-{}",
				std::chrono::steady_clock::now().time_since_epoch().count(),
				Counter.fetch_add(1, std::memory_order_relaxed)
			);
			const auto Temporary = std::filesystem::path(Destination.string() + Suffix);
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
#if defined(_WIN32)
				if (!MoveFileExW(Temporary.c_str(), Destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					throw std::runtime_error(std::format("Atomic project replacement failed ({})", GetLastError()));
#else
				std::filesystem::rename(Temporary, Destination);
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

		std::string EncodeProjectMetadata(ProjectId Identity) {
			Json Document{{"Version", ProjectMetadataVersion}, {"ProjectId", Identity.ToString()}};
			auto Encoded = JsonCodec::Encode(Document, "project metadata");
			if (!Encoded) throw std::runtime_error(Encoded.error().Format());
			return std::move(*Encoded);
		}

		ProjectId ReadProjectIdentity(BaseFilesystem &Filesystem, const std::filesystem::path &ConfigurationRoot) {
			const auto Path = ConfigurationRoot / ProjectMetadataFilename;
			if (!Filesystem.Exists(Path)) {
				const auto Identity = ProjectId::New();
				PersistStringAtomically(Path, EncodeProjectMetadata(Identity));
				return Identity;
			}
			const auto Metadata = Filesystem.Metadata(Path);
			if (Metadata.Type != FileType::File || Metadata.Size > 4096)
				throw std::runtime_error("Project metadata exceeds its byte limit");
			auto Parsed = JsonCodec::Parse(Filesystem.ReadFileToString(Path), 4096, "project metadata");
			if (!Parsed || !Parsed->is_object() || Parsed->size() != 2 ||
				!Parsed->contains("Version") || !(*Parsed)["Version"].is_number_unsigned() ||
				(*Parsed)["Version"].get<std::uint32_t>() != ProjectMetadataVersion ||
				!Parsed->contains("ProjectId") || !(*Parsed)["ProjectId"].is_string())
				throw std::runtime_error("Project metadata format is invalid or unsupported");
			auto Identity = ProjectId::Parse((*Parsed)["ProjectId"].get_ref<const std::string &>());
			if (!Identity) throw std::runtime_error("Project metadata identity is invalid");
			return *Identity;
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
		: Filesystem(fs), Root(fs->Root), RootConfiguration(fs->Root / ".gargantuan") {}

	Project Project::fromInit(
		BaseFilesystem *fs, std::string projectName, std::shared_ptr<Instance> instance, InstanceFormat format
	) {
		Project self(fs);
		if (!SDL_CreateDirectory(self.RootConfiguration.string().c_str())) {
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
			instanceFileContents = InstanceSerialization::Serialize(format, instance);
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
		self.Identity = ReadProjectIdentity(*self.Filesystem, self.RootConfiguration);

		return self;
	}

	Project Project::forDestination(BaseFilesystem *fs, InstanceFormat format) {
		Project self(fs);
		self.InstanceFileFormat = format;
		self.InstanceFilePath = self.RootConfiguration / GetProjectInstanceFilename(format);
		self.Identity = ProjectId::New();
		return self;
	}

	std::shared_ptr<DataModel> Project::DeserializeGame() {
		auto stream = Filesystem->ReadFileToStringStream(InstanceFilePath);

		auto deserialized = InstanceSerialization::Deserialize(InstanceFileFormat, stream);
		if (!deserialized.Ok) {
			std::ostringstream err;
			err << "Failed to deserialize instance file:" << std::endl;
			for (auto &reason : deserialized.Errors) {
				err << "* " << reason << std::endl;
			}
			throw std::runtime_error(err.str());
		} else if (!deserialized.Instance->IsA("DataModel")) {
			throw std::runtime_error(
				std::format(
					"Expected project instance to be a DataModel, got a {}",
					deserialized.Instance->CLASS_DEFINITION.ClassName
				)
			);
		} else {
			auto game = std::dynamic_pointer_cast<DataModel>(deserialized.Instance);
			if (!game) throw std::runtime_error("Project root has inconsistent DataModel type metadata");
			game->MarkPersistenceSubtreeArchivable();
			game->Root = Root;
			game->Filesystem = Filesystem;
			if (Filesystem->Exists(Root / std::filesystem::path(".gargantuan/assets/catalog.json"))) {
				auto Assets = std::dynamic_pointer_cast<AssetService>(game->GetService("AssetService"));
				if (!Assets) throw std::runtime_error("AssetService schema resolved to an incompatible native service");
				Assets->LoadProjectAssets(*Filesystem);
			}
			return game;
		}
	};

	Project::PersistenceSnapshot Project::CaptureGame(
		const std::shared_ptr<DataModel> &game,
		std::uint64_t revision
	) const {
		if (!game) throw std::invalid_argument("Cannot persist a null DataModel");
		if (revision == 0) throw std::invalid_argument("Cannot persist revision zero");
		auto root = std::static_pointer_cast<Instance>(game);
		auto Contents = InstanceSerialization::Serialize(InstanceFileFormat, root);
		AssetProjectSnapshot AssetSnapshot;
		if (auto Assets = std::dynamic_pointer_cast<AssetService>(game->FindFirstChildOfClass("AssetService", false)))
			AssetSnapshot = Assets->CaptureProjectAssets();
		return {revision, std::move(Contents), std::move(AssetSnapshot)};
	}

	void Project::PersistGameAtomically(
		const PersistenceSnapshot &snapshot,
		const std::function<void()> &beforeReplace
	) const {
		if (snapshot.Revision == 0) throw std::invalid_argument("Cannot persist revision zero");
		if (!Identity.IsValid()) throw std::invalid_argument("Cannot persist an invalid project identity");
		if (InstanceFileFormat == InstanceFormat::Binary)
			throw std::runtime_error("Binary instance formats are not yet implemented");
		PersistStringAtomically(RootConfiguration / ProjectMetadataFilename, EncodeProjectMetadata(Identity));
		for (const auto &Artifact : snapshot.Assets.Artifacts) {
			if (!Artifact.Bytes || Artifact.RelativePath.empty())
				throw std::runtime_error("Asset persistence snapshot contains an invalid artifact");
			PersistBytesAtomically(Root / std::filesystem::path(Artifact.RelativePath), *Artifact.Bytes);
		}
		if (!snapshot.Assets.CatalogJson.empty())
			PersistStringAtomically(Root / ".gargantuan" / "assets" / "catalog.json", snapshot.Assets.CatalogJson);
		if (beforeReplace) beforeReplace();
		PersistStringAtomically(InstanceFilePath, snapshot.Contents);
	}
}
