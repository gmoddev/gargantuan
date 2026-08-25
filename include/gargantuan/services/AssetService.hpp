// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/services/generated/AssetService.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	class BaseFilesystem;
	class SourceMount;

	class AssetService : public Instance {
		I_AssetService;

	  public:
		AssetService();
		~AssetService() override;
		AssetService(const AssetService &) = delete;
		AssetService &operator=(const AssetService &) = delete;

		[[nodiscard]] AssetOperationResult ImportProjectAsset(
			SourceMount &Mount,
			std::string Source,
			std::optional<AssetKind> RequestedKind = std::nullopt,
			std::string Name = {},
			const AssetCancellationToken &Cancellation = {}
		);
		[[nodiscard]] AssetOperationResult ReimportProjectAsset(
			SourceMount &Mount,
			std::string_view Reference,
			const AssetCancellationToken &Cancellation = {}
		);
		[[nodiscard]] AssetOperationResult DeleteProjectAsset(std::string_view Reference);

		[[nodiscard]] std::optional<AssetRecord> GetAsset(std::string_view Reference) const;
		[[nodiscard]] std::vector<AssetRecord> GetCatalog(bool IncludeBuiltIns = true) const;
		[[nodiscard]] bool IsAvailable(std::string_view Reference) const;
		[[nodiscard]] std::optional<AssetImageResource> ResolveImage(std::string_view Reference);
		[[nodiscard]] std::optional<AssetFontResource> ResolveFont(std::string_view Reference) const;
		[[nodiscard]] std::optional<ImportedMesh> ResolveMesh(std::string_view Reference) const;
		[[nodiscard]] std::optional<AssetMeshResource> ResolveMeshResource(std::string_view Reference) const;
		[[nodiscard]] std::optional<AssetMaterialResource> ResolveMaterial(std::string_view Reference);
		[[nodiscard]] AssetTextureChanges DrainTextureChanges();
		[[nodiscard]] AssetMeshChanges DrainMeshChanges();
		[[nodiscard]] AssetChangeBatch ReadChanges(std::uint64_t Sequence) const;

		void ConfigureBuiltInFont(const std::filesystem::path &Path);
		[[nodiscard]] std::string RegisterMemoryImage(
			std::string Name,
			std::uint32_t Width,
			std::uint32_t Height,
			std::span<const std::uint8_t> Rgba8
		);

		void LoadProjectAssets(BaseFilesystem &Filesystem);
		void LoadProjectAssetSnapshot(const AssetProjectSnapshot &Snapshot);
		[[nodiscard]] AssetProjectSnapshot CaptureProjectAssets() const;
		void LoadRuntimeAssetSnapshot(const AssetRuntimeSnapshot &Snapshot);
		[[nodiscard]] AssetRuntimeSnapshot CaptureRuntimeAssets() const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
