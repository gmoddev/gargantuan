#pragma once

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>

namespace gargantuan {
	class Project {
	  public:
		struct PersistenceSnapshot {
			std::uint64_t Revision = 0;
			std::string Contents;
			AssetProjectSnapshot Assets;
		};

		std::filesystem::path Root;
		std::filesystem::path RootConfiguration;
		std::filesystem::path InstanceFilePath;
		InstanceSerialization::InstanceFormat InstanceFileFormat;

		static Project fromInit(
			BaseFilesystem *fs,
			std::string projectName = "Untitled",
			std::shared_ptr<Instance> instance = nullptr,
			InstanceSerialization::InstanceFormat format = InstanceSerialization::InstanceFormat ::Json
		);

		static Project fromExisting(BaseFilesystem *fs);
		static Project forDestination(BaseFilesystem *fs, InstanceSerialization::InstanceFormat format);

		std::shared_ptr<DataModel> DeserializeGame();
		[[nodiscard]] PersistenceSnapshot CaptureGame(
			const std::shared_ptr<DataModel> &game,
			std::uint64_t revision
		) const;
		void PersistGameAtomically(
			const PersistenceSnapshot &snapshot,
			const std::function<void()> &beforeReplace = {}
		) const;

	  private:
		BaseFilesystem *Filesystem;
		Project(BaseFilesystem *fs);
	};
}
