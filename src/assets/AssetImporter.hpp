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
#include <vector>

namespace gargantuan {
	struct AssetImportContext {
		AssetKind RequestedKind = AssetKind::Image;
		std::string SourceExtension;
		AssetCancellationToken Cancellation;
		std::chrono::steady_clock::time_point Deadline;
	};

	struct AssetImportCandidate {
		ImportedAsset Asset;
		std::shared_ptr<const std::vector<std::uint8_t>> Artifact;
		AssetContentId ContentId;
	};

	class IAssetImporter {
	  public:
		virtual ~IAssetImporter() = default;
		[[nodiscard]] virtual AssetKind GetKind() const = 0;
		[[nodiscard]] virtual bool SupportsExtension(std::string_view Extension) const = 0;
		[[nodiscard]] virtual std::expected<AssetImportCandidate, AssetDiagnostic> Import(
			std::span<const std::uint8_t> Source,
			const AssetImportContext &Context
		) const = 0;
	};

	[[nodiscard]] std::vector<std::unique_ptr<IAssetImporter>> CreateFoundationAssetImporters();
	[[nodiscard]] std::expected<AssetImportCandidate, AssetDiagnostic> DecodeAssetArtifact(
		std::span<const std::uint8_t> Artifact,
		AssetKind ExpectedKind,
		const AssetContentId &ExpectedContentId
	);
}
