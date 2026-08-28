// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/assets/AssetTypes.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	struct AssetImportContext {
		AssetKind RequestedKind = AssetKind::Image;
		std::string SourceExtension;
		AssetCancellationToken Cancellation;
		std::chrono::steady_clock::time_point Deadline;
		std::shared_ptr<const std::unordered_map<std::string,
			std::shared_ptr<const std::vector<std::uint8_t>>>> ExternalResources;
	};

	struct AssetImportCandidate {
		ImportedAsset Asset;
		std::shared_ptr<const std::vector<std::uint8_t>> Artifact;
		AssetContentId ContentId;
	};

	enum class AssetImportBindingKind : std::uint8_t {
		MeshPrimitiveMaterial,
		MaterialBaseColorTexture,
		MaterialNormalTexture,
		AnimationSkeletonMesh,
	};

	struct AssetImportBinding {
		AssetImportBindingKind Kind = AssetImportBindingKind::MeshPrimitiveMaterial;
		std::size_t TargetIndex = 0;
		std::string LogicalKey;
	};

	struct AssetImportNodeCandidate {
		std::string LogicalKey;
		AssetKind Kind = AssetKind::Image;
		std::string Name;
		ImportedAsset Asset;
		std::shared_ptr<const std::vector<std::uint8_t>> Artifact;
		AssetContentId ContentId;
		std::vector<AssetImportBinding> Bindings;
	};

	struct AssetImportGraphCandidate {
		std::vector<AssetImportNodeCandidate> Nodes;
		std::string PrimaryLogicalKey;
		std::vector<AssetDiagnostic> Diagnostics;
	};

	class IAssetImporter {
	  public:
		virtual ~IAssetImporter() = default;
		[[nodiscard]] virtual AssetKind GetKind() const = 0;
		[[nodiscard]] virtual bool SupportsExtension(std::string_view Extension) const = 0;
		[[nodiscard]] virtual bool IsCompound() const { return false; }
		[[nodiscard]] virtual std::expected<std::vector<std::string>, AssetDiagnostic> DiscoverExternalResources(
			std::span<const std::uint8_t> Source,
			const AssetImportContext &Context
		) const;
		[[nodiscard]] virtual std::expected<AssetImportCandidate, AssetDiagnostic> Import(
			std::span<const std::uint8_t> Source,
			const AssetImportContext &Context
		) const = 0;
		[[nodiscard]] virtual std::expected<AssetImportGraphCandidate, AssetDiagnostic> ImportGraph(
			std::span<const std::uint8_t> Source,
			const AssetImportContext &Context
		) const;
	};

	[[nodiscard]] std::vector<std::unique_ptr<IAssetImporter>> CreateFoundationAssetImporters();
	[[nodiscard]] std::unique_ptr<IAssetImporter> CreateGltfImporter(AssetKind Kind = AssetKind::Mesh);
	[[nodiscard]] std::expected<std::shared_ptr<const std::vector<std::uint8_t>>, AssetDiagnostic> EncodeAssetArtifact(
		const ImportedAsset &Asset,
		AssetKind Kind
	);
	[[nodiscard]] std::expected<AssetImportCandidate, AssetDiagnostic> DecodeAssetArtifact(
		std::span<const std::uint8_t> Artifact,
		AssetKind ExpectedKind,
		const AssetContentId &ExpectedContentId
	);
}
