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

	struct SchemaDefinition {
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

	class RuntimeSchemaRegistry {
	  public:
		void RegisterNative(std::type_index nativeType, SchemaDefinition definition);

		template <typename T> void RegisterNative(SchemaDefinition definition) {
			RegisterNative(std::type_index(typeid(T)), std::move(definition));
		}

		void Validate();
		void InvalidateCaches();

		[[nodiscard]] SchemaDefinition *FindById(SchemaId id);
		[[nodiscard]] SchemaDefinition *FindByType(std::type_index nativeType);
		[[nodiscard]] SchemaDefinition *FindByName(std::string_view name);
		[[nodiscard]] std::vector<const SchemaDefinition *> Enumerate();
		[[nodiscard]] std::size_t Size() const { return DefinitionsByType.size(); }

	  private:
		std::unordered_map<std::type_index, SchemaDefinition> DefinitionsByType;
		std::unordered_map<SchemaId, std::type_index, SchemaIdHash> TypesById;
		std::unordered_map<std::string, std::type_index> TypesByCanonicalName;
		bool Validated = false;

		void EnsureValidated();
	};

	[[nodiscard]] RuntimeSchemaRegistry &GetRuntimeSchemaRegistry();
	[[nodiscard]] std::string_view GetSchemaProvenanceName(SchemaProvenance provenance);
}
