#pragma once

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/serialization/GasDocument.hpp"
#include "gargantuan/terrain/Persistence.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <cstdint>

namespace gargantuan {
	class Project {
	  public:
		struct PersistenceSnapshot {
			std::uint64_t Revision = 0;
			std::string Contents;
			AssetProjectSnapshot Assets;
			std::optional<terrain::TerrainCapture> Terrain;
			terrain::DomainTerrainCaptures Domains;
			blockgrid::SegmentMap GridSegments;
		};

		std::filesystem::path Root;
		std::filesystem::path RootConfiguration;
		std::filesystem::path InstanceFilePath;
		InstanceSerialization::InstanceFormat InstanceFileFormat;
		ProjectId Identity;

		static Project fromInit(
			BaseFilesystem *fs,
			std::string projectName = "Untitled",
			std::shared_ptr<Instance> instance = nullptr,
			InstanceSerialization::InstanceFormat format = InstanceSerialization::InstanceFormat ::Json
		);

		static Project fromExisting(BaseFilesystem *fs);
		static Project forDestination(BaseFilesystem *fs, InstanceSerialization::InstanceFormat format);
		// Read a trusted GAS origin without migrating metadata or opening an Instance file.
		[[nodiscard]] static Project FromGasOrigin(BaseFilesystem *Filesystem);

		class PreparedGame {
		  public:
			PreparedGame(PreparedGame &&) noexcept = default;
			PreparedGame &operator=(PreparedGame &&) noexcept = default;
			[[nodiscard]] const terrain::Field *GetTerrain() const noexcept { return Terrain.get(); }
		  private:
			friend class Project;
			PreparedGame() = default;
			const BaseFilesystem *Source{};
			ProjectId Identity;
			std::string Contents;
			std::optional<AssetContentId> Manifest;
			std::unique_ptr<terrain::Field> Terrain;
			terrain::DomainTerrainBindings Bindings;
			std::map<std::uint32_t, std::unique_ptr<terrain::Field>> DomainFields;
			blockgrid::SegmentMap GridSegments;
		};

		[[nodiscard]] PreparedGame PrepareGameLoad() const;
		std::shared_ptr<DataModel> DeserializeGame();
		[[nodiscard]] std::shared_ptr<DataModel> DeserializeGame(PreparedGame Prepared);
		// Capture a place from a world owned by this project's filesystem.
		[[nodiscard]] SerializationResult<Gas::Document> CaptureGasPlace(const std::shared_ptr<DataModel> &World,
			std::optional<terrain::TerrainCapture> *CapturedTerrain = nullptr,
			terrain::DomainTerrainCaptures *CapturedDomains = nullptr) const;
		void PersistTerrainArtifacts(const terrain::TerrainCapture &Capture,
			std::optional<std::uint32_t> DomainNode = std::nullopt) const;
		[[nodiscard]] std::optional<AssetContentId> GetPersistedTerrainManifest(terrain::DomainTerrainBindings *Domains = nullptr) const;
		void ValidatePersistenceDestination() const;
		void ExportGasPlace(const std::shared_ptr<DataModel> &World, const std::filesystem::path &Destination,
			const std::optional<std::string> &ExpectedPreRunSource) const;
		// The caller establishes this origin's trusted schema before loading.
		// As with DeserializeGame, the caller establishes the loaded revision before publication.
		[[nodiscard]] std::shared_ptr<DataModel> LoadGasPlace(const Gas::Document &Document) const;
		// Publish JSON into an origin whose Instance file is absent.
		void RestoreGasPlace(const std::shared_ptr<DataModel> &World,
			const std::optional<std::string> &ExpectedPreRunSource) const;
		[[nodiscard]] PersistenceSnapshot CaptureGame(
			const std::shared_ptr<DataModel> &game,
			std::uint64_t revision,
			bool ReuseDurableTerrain = false
		) const;
		void PersistGameAtomically(
			const PersistenceSnapshot &snapshot,
			const std::function<void()> &beforeReplace = {}
		) const;
		[[nodiscard]] bool ReplacePreRunSource(const std::optional<std::string> &ExpectedSource,
			std::string_view Source, const std::function<void()> &BeforeReplace = {}) const;

	  private:
		struct DurableTerrainState;
		void PersistGridSegments(std::string_view Scene,const blockgrid::SegmentMap& Segments) const;
		BaseFilesystem *Filesystem;
		std::shared_ptr<DurableTerrainState> DurableTerrain;
		Project(BaseFilesystem *fs);
	};
}
