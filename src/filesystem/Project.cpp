#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/Paths.hpp"

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

		return self;
	}

	Project Project::forDestination(BaseFilesystem *fs, InstanceFormat format) {
		Project self(fs);
		self.InstanceFileFormat = format;
		self.InstanceFilePath = self.RootConfiguration / GetProjectInstanceFilename(format);
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
			game->Root = Root;
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
		return {revision, InstanceSerialization::Serialize(InstanceFileFormat, root)};
	}

	void Project::PersistGameAtomically(
		const PersistenceSnapshot &snapshot,
		const std::function<void()> &beforeReplace
	) const {
		if (snapshot.Revision == 0) throw std::invalid_argument("Cannot persist revision zero");
		if (InstanceFileFormat == InstanceFormat::Binary)
			throw std::runtime_error("Binary instance formats are not yet implemented");
		std::filesystem::create_directories(RootConfiguration);
		static std::atomic_uint64_t Counter = 1;
		const auto suffix = std::format(
			".saving-{}-{}",
			std::chrono::steady_clock::now().time_since_epoch().count(),
			Counter.fetch_add(1, std::memory_order_relaxed)
		);
		const auto temporary = std::filesystem::path(InstanceFilePath.string() + suffix);
		try {
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				if (!output) throw std::runtime_error("Could not open the temporary project file");
				output.write(snapshot.Contents.data(), static_cast<std::streamsize>(snapshot.Contents.size()));
				output.flush();
				if (!output) throw std::runtime_error("Could not write the temporary project file");
				output.close();
				if (!output) throw std::runtime_error("Could not close the temporary project file");
			}
			if (beforeReplace) beforeReplace();
#if defined(_WIN32)
			if (!MoveFileExW(
				temporary.c_str(), InstanceFilePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
			)) throw std::runtime_error(std::format("Atomic project replacement failed ({})", GetLastError()));
#else
			std::filesystem::rename(temporary, InstanceFilePath);
#endif
		} catch (...) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			throw;
		}
	}
}
