// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/scripting/UserdataMethod.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace gargantuan {
	class Instance;

	enum class SchemaProvenance : std::uint8_t {
		NativeEngine,
		CoreLuau,
		Game,
		Package,
		Plugin,
		Tooling,
	};

	enum class SchemaDefinitionKind : std::uint8_t { Class, Enum };

	inline constexpr std::size_t MaximumSchemaNamespaceBytes = 100;
	inline constexpr std::size_t MaximumSchemaDefinitionNameBytes = 100;
	inline constexpr std::size_t MaximumCustomEnumDefinitions = 64;
	inline constexpr std::size_t MaximumCustomEnumItems = 256;
	inline constexpr std::size_t MaximumCustomEnumItemNameBytes = 100;
	inline constexpr std::size_t MaximumCustomSchemaPayloadBytes = 64 * 1024;

	struct SchemaClassDefinition {
		SchemaId Id{};
		std::string Namespace = "Engine";
		std::string ClassName = "Instance";
		std::uint32_t DefinitionVersion = 1;
		SchemaProvenance Provenance = SchemaProvenance::NativeEngine;
		std::shared_ptr<Instance> (*Constructor)() = nullptr;
		std::string Description = "(No description provided.)";
		std::optional<std::string> Superclass = "Instance";
		std::optional<SchemaId> BaseSchemaId = SchemaId::FromNativeName("Engine", "Instance");
		std::unordered_map<std::string, InstanceProperty> Properties{};
		std::unordered_map<std::string, UserdataMethod<Instance>> Methods{};
		bool EditorVisible = true;
		std::string OriginDetail{};

		// Populated and owned by RuntimeSchemaRegistry. These are compatibility
		// views over the canonical definition, not a second metadata source.
		std::string CanonicalName{};
		bool Flattened = false;
		std::unordered_set<std::string> InheritedClasses{};
		std::unordered_map<std::string, const InstanceProperty *> AllProperties{};
		std::unordered_map<std::string, const UserdataMethod<Instance> *> AllMethods{};
	};

	struct SchemaEnumItem {
		std::string Name;
		std::int32_t Value = 0;
		auto operator<=>(const SchemaEnumItem &) const = default;
	};

	struct SchemaEnumDefinition {
		SchemaId Id{};
		std::string Namespace;
		std::string Name;
		std::uint32_t DefinitionVersion = 1;
		SchemaProvenance Provenance = SchemaProvenance::Game;
		std::string OriginDetail{};
		std::string CanonicalName{};
		std::vector<SchemaEnumItem> Items;
	};

	using SchemaDefinition = std::variant<SchemaClassDefinition, SchemaEnumDefinition>;

	[[nodiscard]] SchemaDefinitionKind GetSchemaDefinitionKind(const SchemaDefinition &definition);
	[[nodiscard]] const SchemaId &GetSchemaDefinitionId(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaDefinitionNamespace(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaDefinitionName(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaDefinitionCanonicalName(const SchemaDefinition &definition);
	[[nodiscard]] std::uint32_t GetSchemaDefinitionVersion(const SchemaDefinition &definition);
	[[nodiscard]] SchemaProvenance GetSchemaDefinitionProvenance(const SchemaDefinition &definition);

	enum class RuntimeSchemaRegistryState : std::uint8_t {
		Building,
		Validated,
		Frozen,
	};

	class RuntimeSchemaRegistry {
	  public:
		void RegisterNative(std::type_index nativeType, SchemaClassDefinition definition);
		void RegisterEnum(SchemaEnumDefinition definition);

		template <typename T> void RegisterNative(SchemaClassDefinition definition) {
			RegisterNative(std::type_index(typeid(T)), std::move(definition));
		}

		void Validate();
		void Freeze();

		[[nodiscard]] const SchemaDefinition *FindDefinitionById(SchemaId id) const;
		[[nodiscard]] const SchemaDefinition *FindDefinitionByName(std::string_view name) const;
		[[nodiscard]] const SchemaClassDefinition *FindClassById(SchemaId id) const;
		[[nodiscard]] const SchemaClassDefinition *FindClassByType(std::type_index nativeType) const;
		[[nodiscard]] const SchemaClassDefinition *FindClassByName(std::string_view name) const;
		[[nodiscard]] const SchemaEnumDefinition *FindEnumById(SchemaId id) const;
		[[nodiscard]] const SchemaEnumDefinition *FindEnumByName(std::string_view name) const;
		[[nodiscard]] std::vector<const SchemaDefinition *> EnumerateDefinitions() const;
		[[nodiscard]] std::vector<const SchemaClassDefinition *> EnumerateClasses() const;

		// Class-only compatibility surface retained for existing reflection code.
		[[nodiscard]] const SchemaClassDefinition *FindById(SchemaId id) const { return FindClassById(id); }
		[[nodiscard]] const SchemaClassDefinition *FindByType(std::type_index nativeType) const { return FindClassByType(nativeType); }
		[[nodiscard]] const SchemaClassDefinition *FindByName(std::string_view name) const { return FindClassByName(name); }
		[[nodiscard]] std::vector<const SchemaClassDefinition *> Enumerate() const { return EnumerateClasses(); }
		[[nodiscard]] std::size_t Size() const { return DefinitionsById.size(); }
		[[nodiscard]] RuntimeSchemaRegistryState GetState() const { return State; }
		[[nodiscard]] bool IsValidated() const { return State != RuntimeSchemaRegistryState::Building; }
		[[nodiscard]] bool IsFrozen() const { return State == RuntimeSchemaRegistryState::Frozen; }

	  private:
		std::unordered_map<SchemaId, SchemaDefinition, SchemaIdHash> DefinitionsById;
		std::unordered_map<std::type_index, SchemaId> ClassIdsByType;
		std::unordered_map<std::string, SchemaId> IdsByCanonicalName;
		RuntimeSchemaRegistryState State = RuntimeSchemaRegistryState::Building;

		void RequireBuilding(std::string_view operation) const;
		void RequireFrozen(std::string_view operation) const;
	};

	[[nodiscard]] std::string_view GetRuntimeSchemaRegistryStateName(RuntimeSchemaRegistryState state);
	[[nodiscard]] std::string_view GetSchemaProvenanceName(SchemaProvenance provenance);
}
