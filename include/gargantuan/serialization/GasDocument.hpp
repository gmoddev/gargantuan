// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/runtime/WireValue.hpp"
#include "gargantuan/terrain/DomainTerrainBindings.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace gargantuan::Gas {
	enum class DocumentKind : std::uint8_t { Model = 1, Place = 2 };

	inline constexpr std::uint16_t FormatVersion = 1;
	inline constexpr std::uint16_t TerrainPlaceFormatVersion = 2;
	inline constexpr std::uint16_t DomainTerrainPlaceFormatVersion = 3;
	inline constexpr std::uint32_t SchemaVersion = 1;
	inline constexpr std::size_t MaximumDocumentBytes = 8 * 1024 * 1024;
	inline constexpr std::size_t MaximumInstances = 64 * 1024;
	inline constexpr std::size_t MaximumDepth = 64;
	inline constexpr std::size_t MaximumStringBytes = 64 * 1024;
	inline constexpr std::size_t MaximumBinaryBytes = 1024 * 1024;
	inline constexpr std::size_t MaximumProperties = 1024;
	inline constexpr std::size_t MaximumValues = 1024 * 1024;
	inline constexpr std::size_t MaximumReferences = 1024 * 1024;
	inline constexpr std::size_t MaximumDependencies = 16 * 1024;
	inline constexpr std::size_t MaximumPreRunBytes = 256 * 1024;

	// One-based preorder index. Zero denotes a null reference or a root parent.
	struct LocalReference {
		std::uint32_t Target = 0;
		bool operator==(const LocalReference &) const = default;
	};

	// WireObjectReference is invalid in GAS. The format accepts only LocalReference.
	using Binary = std::vector<std::uint8_t>;
	using Value = std::variant<WireValue, LocalReference, Binary>;
	using Properties = std::map<std::string, Value>;

	struct PropertyGroup {
		SchemaId Owner;
		std::uint32_t DefinitionVersion = 0;
		Properties Values;
		bool operator==(const PropertyGroup &) const = default;
	};

	struct Node {
		std::uint32_t Parent = 0;
		SchemaId Class;
		std::uint32_t DefinitionVersion = 0;
		std::string Name;
		Properties SavedProperties;
		Properties Attributes;
		std::vector<PropertyGroup> Extensions;
		std::vector<PropertyGroup> CustomProperties;
		std::vector<std::string> Tags;
		bool operator==(const Node &) const = default;
	};

	struct Dependency {
		AssetId Id;
		AssetContentId Content;
		bool operator==(const Dependency &) const = default;
	};

	struct PlaceMetadata {
		ProjectId Identity;
		std::optional<std::string> PreRunSource;
		std::optional<AssetContentId> TerrainManifest;
		terrain::DomainTerrainBindings Domains;
		bool operator==(const PlaceMetadata &) const = default;
	};

	struct Document {
		DocumentKind Kind = DocumentKind::Model;
		std::vector<Node> Nodes;
		std::vector<Dependency> Dependencies;
		std::optional<PlaceMetadata> Place;
		bool operator==(const Document &) const = default;
	};

	[[nodiscard]] inline std::uint16_t GetFormatVersion(const Document &Value) noexcept {
		if (Value.Place && !Value.Place->Domains.empty()) return DomainTerrainPlaceFormatVersion;
		return Value.Place && Value.Place->TerrainManifest ? TerrainPlaceFormatVersion : FormatVersion;
	}

	// These operations handle inert values only. Callers still require frozen-schema,
	// dependency and lifecycle validation before constructing or publishing Instances.
	[[nodiscard]] SerializationResult<Binary> Encode(const Document &DocumentValue);
	[[nodiscard]] SerializationResult<Document> Decode(std::span<const std::uint8_t> Bytes);
}
