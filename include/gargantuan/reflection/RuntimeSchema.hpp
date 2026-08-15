// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/runtime/WireValue.hpp"
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

	enum class SchemaDefinitionKind : std::uint8_t { Class, Enum, Extension };
	enum class SchemaExtensionPropertyType : std::uint8_t { Boolean, Integer, Number, String };
	enum class SchemaClassConstructionKind : std::uint8_t { Native, CustomData };
	enum class CustomSubclassPolicy : std::uint8_t { Forbidden, DataOnly };

	inline constexpr std::size_t MaximumSchemaNamespaceBytes = 100;
	inline constexpr std::size_t MaximumSchemaDefinitionNameBytes = 100;
	inline constexpr std::size_t MaximumCustomEnumDefinitions = 64;
	inline constexpr std::size_t MaximumCustomEnumItems = 256;
	inline constexpr std::size_t MaximumCustomEnumItemNameBytes = 100;
	inline constexpr std::size_t MaximumCustomExtensionDefinitions = 64;
	inline constexpr std::size_t MaximumCustomClassDefinitions = 64;
	inline constexpr std::size_t MaximumCustomClassProperties = 64;
	inline constexpr std::size_t MaximumCustomClassInheritanceDepth = 16;
	inline constexpr std::size_t MaximumCustomPropertyOverridesPerInstance = 128;
	inline constexpr std::size_t MaximumCustomPropertyOverrideBytesPerInstance = 32 * 1024;
	inline constexpr std::size_t MaximumExtensionProperties = 64;
	inline constexpr std::size_t MaximumExtensionPropertyNameBytes = 100;
	inline constexpr std::size_t MaximumExtensionDefaultValueBytes = 4 * 1024;
	inline constexpr std::size_t MaximumExtensionOverridesPerInstance = 128;
	inline constexpr std::size_t MaximumExtensionOverrideBytesPerInstance = 32 * 1024;
	inline constexpr std::size_t MaximumCustomSchemaPayloadBytes = 64 * 1024;

	struct SchemaClassProperty {
		std::string Name;
		std::string CanonicalName;
		SchemaExtensionPropertyType Type = SchemaExtensionPropertyType::Boolean;
		WireValue DefaultValue = false;
		bool Editable = true;
		auto operator<=>(const SchemaClassProperty &) const = default;
	};

	struct SchemaClassDefinition {
		SchemaId Id{};
		std::string Namespace = "Engine";
		std::string ClassName = "Instance";
		std::uint32_t DefinitionVersion = 1;
		SchemaProvenance Provenance = SchemaProvenance::NativeEngine;
		SchemaClassConstructionKind ConstructionKind = SchemaClassConstructionKind::Native;
		CustomSubclassPolicy ProjectSubclassPolicy = CustomSubclassPolicy::Forbidden;
		std::shared_ptr<Instance> (*Constructor)() = nullptr;
		std::string Description = "(No description provided.)";
		std::optional<std::string> Superclass = "Instance";
		std::optional<SchemaId> BaseSchemaId = SchemaId::FromNativeName("Engine", "Instance");
		SchemaId NativeHostClassId{};
		std::optional<std::string> PendingBaseCanonicalName{};
		std::vector<SchemaClassProperty> DeclaredCustomProperties{};
		std::unordered_map<std::string, InstanceProperty> Properties{};
		std::unordered_map<std::string, UserdataMethod<Instance>> Methods{};
		bool EditorVisible = true;
		std::string OriginDetail{};

		// Populated and owned by RuntimeSchemaRegistry. These are compatibility
		// views over the canonical definition, not a second metadata source.
		std::string CanonicalName{};
		bool Flattened = false;
		std::unordered_set<std::string> InheritedClasses{};
		std::unordered_set<SchemaId, SchemaIdHash> InheritedClassIds{};
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

	struct SchemaExtensionProperty {
		std::string Name;
		std::string CanonicalName;
		SchemaExtensionPropertyType Type = SchemaExtensionPropertyType::Boolean;
		WireValue DefaultValue = false;
		bool Editable = true;
		auto operator<=>(const SchemaExtensionProperty &) const = default;
	};

	struct SchemaExtensionDefinition {
		SchemaId Id{};
		std::string Namespace;
		std::string Name;
		std::uint32_t DefinitionVersion = 1;
		SchemaProvenance Provenance = SchemaProvenance::Game;
		std::string OriginDetail{};
		std::string CanonicalName{};
		SchemaId TargetClassId{};
		std::vector<SchemaExtensionProperty> Properties;
	};

	using SchemaDefinition = std::variant<SchemaClassDefinition, SchemaEnumDefinition, SchemaExtensionDefinition>;

	[[nodiscard]] SchemaDefinitionKind GetSchemaDefinitionKind(const SchemaDefinition &definition);
	[[nodiscard]] const SchemaId &GetSchemaDefinitionId(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaDefinitionNamespace(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaDefinitionName(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaDefinitionCanonicalName(const SchemaDefinition &definition);
	[[nodiscard]] std::uint32_t GetSchemaDefinitionVersion(const SchemaDefinition &definition);
	[[nodiscard]] SchemaProvenance GetSchemaDefinitionProvenance(const SchemaDefinition &definition);
	[[nodiscard]] std::string_view GetSchemaExtensionPropertyTypeName(SchemaExtensionPropertyType type);
	[[nodiscard]] std::optional<SchemaExtensionPropertyType> ParseSchemaExtensionPropertyType(std::string_view type);
	[[nodiscard]] std::size_t ValidateSchemaExtensionPropertyValue(
		SchemaExtensionPropertyType type,
		const WireValue &value
	);

	enum class RuntimeSchemaRegistryState : std::uint8_t {
		Building,
		Validated,
		Frozen,
	};

	class RuntimeSchemaRegistry {
	  public:
		void RegisterNative(std::type_index nativeType, SchemaClassDefinition definition);
		void RegisterClass(SchemaClassDefinition definition, std::string_view baseCanonicalName);
		void RegisterEnum(SchemaEnumDefinition definition);
		void RegisterExtension(SchemaExtensionDefinition definition, std::string_view targetCanonicalName);

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
		[[nodiscard]] const SchemaClassProperty *FindCustomClassProperty(
			SchemaId declaringClassId,
			std::string_view propertyName
		) const;
		[[nodiscard]] bool IsClassDerivedFrom(SchemaId classId, SchemaId baseClassId) const;
		[[nodiscard]] bool IsClassConstructible(const SchemaClassDefinition &definition) const;
		[[nodiscard]] const SchemaEnumDefinition *FindEnumById(SchemaId id) const;
		[[nodiscard]] const SchemaEnumDefinition *FindEnumByName(std::string_view name) const;
		[[nodiscard]] const SchemaExtensionDefinition *FindExtensionById(SchemaId id) const;
		[[nodiscard]] const SchemaExtensionDefinition *FindExtensionByName(std::string_view name) const;
		[[nodiscard]] const SchemaExtensionProperty *FindExtensionProperty(
			SchemaId extensionId,
			std::string_view propertyName
		) const;
		[[nodiscard]] bool IsExtensionApplicableToClass(SchemaId extensionId, SchemaId classId) const;
		[[nodiscard]] std::vector<const SchemaExtensionDefinition *> FindApplicableExtensions(SchemaId classId) const;
		[[nodiscard]] std::vector<const SchemaDefinition *> EnumerateDefinitions() const;
		[[nodiscard]] std::vector<const SchemaClassDefinition *> EnumerateClasses() const;
		[[nodiscard]] std::vector<const SchemaExtensionDefinition *> EnumerateExtensions() const;

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
		std::size_t CustomSchemaPayloadBytes = 0;
		RuntimeSchemaRegistryState State = RuntimeSchemaRegistryState::Building;

		void RequireBuilding(std::string_view operation) const;
		void RequireFrozen(std::string_view operation) const;
	};

	[[nodiscard]] std::string_view GetRuntimeSchemaRegistryStateName(RuntimeSchemaRegistryState state);
	[[nodiscard]] std::string_view GetSchemaProvenanceName(SchemaProvenance provenance);
}
