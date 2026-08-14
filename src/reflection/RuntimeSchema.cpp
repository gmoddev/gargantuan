// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/reflection/RuntimeSchema.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gargantuan {
	namespace {
		bool IsValidUtf8(std::string_view value) {
			for (std::size_t index = 0; index < value.size();) {
				const auto lead = static_cast<unsigned char>(value[index]);
				std::size_t length = 0;
				std::uint32_t codepoint = 0;
				if (lead <= 0x7f) { length = 1; codepoint = lead; }
				else if (lead >= 0xc2 && lead <= 0xdf) { length = 2; codepoint = lead & 0x1f; }
				else if (lead >= 0xe0 && lead <= 0xef) { length = 3; codepoint = lead & 0x0f; }
				else if (lead >= 0xf0 && lead <= 0xf4) { length = 4; codepoint = lead & 0x07; }
				else return false;
				if (index + length > value.size()) return false;
				for (std::size_t offset = 1; offset < length; ++offset) {
					const auto continuation = static_cast<unsigned char>(value[index + offset]);
					if ((continuation & 0xc0) != 0x80) return false;
					codepoint = (codepoint << 6) | (continuation & 0x3f);
				}
				if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
					(length == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
					(codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
				index += length;
			}
			return true;
		}

		std::string ClassCanonicalName(const SchemaClassDefinition &definition) {
			return definition.Namespace + "." + definition.ClassName;
		}

		std::string EnumCanonicalName(const SchemaEnumDefinition &definition) {
			return definition.Namespace + "." + definition.Name;
		}

		template <typename Definition>
		[[noreturn]] void InvalidDefinition(const Definition &definition, std::string_view reason) {
			const auto canonical = [&]() {
				if constexpr (std::is_same_v<Definition, SchemaClassDefinition>) return ClassCanonicalName(definition);
				else return EnumCanonicalName(definition);
			}();
			throw std::invalid_argument("Invalid schema definition '" + canonical + "': " + std::string(reason));
		}

		void ValidateIdentity(
			SchemaId id,
			std::string_view schemaNamespace,
			std::string_view name,
			std::uint32_t version,
			const auto &definition
		) {
			if (!id.IsValid()) InvalidDefinition(definition, "SchemaId is invalid");
			if (schemaNamespace.empty()) InvalidDefinition(definition, "namespace is empty");
			if (name.empty()) InvalidDefinition(definition, "definition name is empty");
			if (schemaNamespace.size() > MaximumSchemaNamespaceBytes)
				InvalidDefinition(definition, "namespace exceeds its byte limit");
			if (name.size() > MaximumSchemaDefinitionNameBytes)
				InvalidDefinition(definition, "definition name exceeds its byte limit");
			if (schemaNamespace.find('\0') != std::string_view::npos || name.find('\0') != std::string_view::npos)
				InvalidDefinition(definition, "identity contains an embedded null");
			if (!IsValidUtf8(schemaNamespace) || !IsValidUtf8(name))
				InvalidDefinition(definition, "identity is not valid UTF-8");
			if (version == 0) InvalidDefinition(definition, "definition version is zero");
		}
	}

	SchemaDefinitionKind GetSchemaDefinitionKind(const SchemaDefinition &definition) {
		return std::holds_alternative<SchemaClassDefinition>(definition)
			? SchemaDefinitionKind::Class : SchemaDefinitionKind::Enum;
	}

	const SchemaId &GetSchemaDefinitionId(const SchemaDefinition &definition) {
		return std::visit([](const auto &typed) -> const SchemaId & { return typed.Id; }, definition);
	}

	std::string_view GetSchemaDefinitionNamespace(const SchemaDefinition &definition) {
		return std::visit([](const auto &typed) -> std::string_view { return typed.Namespace; }, definition);
	}

	std::string_view GetSchemaDefinitionName(const SchemaDefinition &definition) {
		return std::visit([](const auto &typed) -> std::string_view {
			using Definition = std::decay_t<decltype(typed)>;
			if constexpr (std::is_same_v<Definition, SchemaClassDefinition>) return typed.ClassName;
			else return typed.Name;
		}, definition);
	}

	std::string_view GetSchemaDefinitionCanonicalName(const SchemaDefinition &definition) {
		return std::visit([](const auto &typed) -> std::string_view { return typed.CanonicalName; }, definition);
	}

	std::uint32_t GetSchemaDefinitionVersion(const SchemaDefinition &definition) {
		return std::visit([](const auto &typed) { return typed.DefinitionVersion; }, definition);
	}

	SchemaProvenance GetSchemaDefinitionProvenance(const SchemaDefinition &definition) {
		return std::visit([](const auto &typed) { return typed.Provenance; }, definition);
	}

	void RuntimeSchemaRegistry::RequireBuilding(std::string_view operation) const {
		if (State != RuntimeSchemaRegistryState::Building)
			throw std::logic_error("Runtime schema registry cannot " + std::string(operation) + " while " +
				std::string(GetRuntimeSchemaRegistryStateName(State)));
	}

	void RuntimeSchemaRegistry::RequireFrozen(std::string_view operation) const {
		if (State != RuntimeSchemaRegistryState::Frozen)
			throw std::logic_error("Runtime schema registry cannot " + std::string(operation) + " while " +
				std::string(GetRuntimeSchemaRegistryStateName(State)));
	}

	void RuntimeSchemaRegistry::RegisterNative(std::type_index nativeType, SchemaClassDefinition definition) {
		RequireBuilding("register a native definition");
		ValidateIdentity(definition.Id, definition.Namespace, definition.ClassName, definition.DefinitionVersion, definition);
		if (definition.Provenance != SchemaProvenance::NativeEngine)
			InvalidDefinition(definition, "RegisterNative requires NativeEngine provenance");
		if (definition.Superclass.has_value() != definition.BaseSchemaId.has_value())
			InvalidDefinition(definition, "base name and base SchemaId must either both exist or both be absent");
		definition.CanonicalName = ClassCanonicalName(definition);
		definition.Flattened = false;
		definition.InheritedClasses.clear();
		definition.AllProperties.clear();
		definition.AllMethods.clear();
		if (ClassIdsByType.contains(nativeType))
			throw std::invalid_argument("Native type is already registered in the runtime schema");
		if (DefinitionsById.contains(definition.Id))
			throw std::invalid_argument("Duplicate SchemaId " + definition.Id.ToString());
		if (IdsByCanonicalName.contains(definition.CanonicalName))
			throw std::invalid_argument("Duplicate canonical schema name " + definition.CanonicalName);

		for (auto &[name, property] : definition.Properties) {
			if (name.empty() || property.Name != name) InvalidDefinition(definition, "property name/entry mismatch");
			if (property.ReflectedTypedef.empty()) InvalidDefinition(definition, "property has no reflected value type");
			if (!property.Read && !property.Write) InvalidDefinition(definition, "property has no native access path");
			if (property.ReplicationPolicy != InstanceProperty::Replication::None && (!property.Read || !property.Write))
				InvalidDefinition(definition, "replicated property is not readable and writable");
			if (property.Editable && (!property.Write || property.WritePermission == Enums::Permission::Never))
				InvalidDefinition(definition, "editable property has no write path");
			if (property.DeclaringSchemaId.IsValid() && property.DeclaringSchemaId != definition.Id)
				InvalidDefinition(definition, "property declares a different owner");
			property.DeclaringSchemaId = definition.Id;
		}

		for (auto &[name, method] : definition.Methods) {
			if (name.empty() || !method.Call) InvalidDefinition(definition, "method is unnamed or has no native call path");
			if (definition.Properties.contains(name)) InvalidDefinition(definition, "property and method names collide");
			if (method.DeclaringSchemaId.IsValid() && method.DeclaringSchemaId != definition.Id)
				InvalidDefinition(definition, "method declares a different owner");
			method.DeclaringSchemaId = definition.Id;
		}

		const auto id = definition.Id;
		const auto canonical = definition.CanonicalName;
		DefinitionsById.emplace(id, std::move(definition));
		try {
			ClassIdsByType.emplace(nativeType, id);
			IdsByCanonicalName.emplace(canonical, id);
		} catch (...) {
			ClassIdsByType.erase(nativeType);
			IdsByCanonicalName.erase(canonical);
			DefinitionsById.erase(id);
			throw;
		}
	}

	void RuntimeSchemaRegistry::RegisterEnum(SchemaEnumDefinition definition) {
		RequireBuilding("register an enum definition");
		ValidateIdentity(definition.Id, definition.Namespace, definition.Name, definition.DefinitionVersion, definition);
		if (definition.Provenance != SchemaProvenance::Game)
			InvalidDefinition(definition, "custom enum registration requires Game provenance");
		if (definition.Id != SchemaId::FromEnumName(definition.Namespace, definition.Name))
			InvalidDefinition(definition, "SchemaId does not match deterministic enum identity");
		if (definition.Items.empty()) InvalidDefinition(definition, "enum has no items");
		if (definition.Items.size() > MaximumCustomEnumItems) InvalidDefinition(definition, "enum exceeds its item limit");
		std::sort(definition.Items.begin(), definition.Items.end(), [](const auto &left, const auto &right) {
			return left.Name != right.Name ? left.Name < right.Name : left.Value < right.Value;
		});
		std::unordered_set<std::string> names;
		std::unordered_set<std::int32_t> values;
		std::size_t payloadBytes = definition.Namespace.size() + definition.Name.size();
		for (const auto &item : definition.Items) {
			if (item.Name.empty()) InvalidDefinition(definition, "enum item name is empty");
			if (item.Name.size() > MaximumCustomEnumItemNameBytes)
				InvalidDefinition(definition, "enum item name exceeds its byte limit");
			if (item.Name.find('\0') != std::string::npos || !IsValidUtf8(item.Name))
				InvalidDefinition(definition, "enum item name is invalid UTF-8 or contains null");
			if (!names.insert(item.Name).second) InvalidDefinition(definition, "duplicate enum item name " + item.Name);
			if (!values.insert(item.Value).second) InvalidDefinition(definition, "duplicate enum item value");
			payloadBytes += item.Name.size() + sizeof(item.Value);
		}
		if (payloadBytes > MaximumCustomSchemaPayloadBytes)
			InvalidDefinition(definition, "enum definition exceeds its payload byte limit");
		if (payloadBytes > MaximumCustomSchemaPayloadBytes -
			std::min(CustomSchemaPayloadBytes, MaximumCustomSchemaPayloadBytes))
			InvalidDefinition(definition, "candidate exceeds its aggregate custom schema payload byte limit");
		const auto enumCount = std::count_if(DefinitionsById.begin(), DefinitionsById.end(), [](const auto &entry) {
			return std::holds_alternative<SchemaEnumDefinition>(entry.second);
		});
		if (enumCount >= MaximumCustomEnumDefinitions)
			InvalidDefinition(definition, "candidate exceeds its custom enum definition limit");
		definition.CanonicalName = EnumCanonicalName(definition);
		if (DefinitionsById.contains(definition.Id))
			throw std::invalid_argument("Duplicate SchemaId " + definition.Id.ToString());
		if (IdsByCanonicalName.contains(definition.CanonicalName))
			throw std::invalid_argument("Duplicate canonical schema name " + definition.CanonicalName);

		const auto id = definition.Id;
		const auto canonical = definition.CanonicalName;
		DefinitionsById.emplace(id, std::move(definition));
		try { IdsByCanonicalName.emplace(canonical, id); }
		catch (...) { DefinitionsById.erase(id); throw; }
		CustomSchemaPayloadBytes += payloadBytes;
	}

	void RuntimeSchemaRegistry::Validate() {
		RequireBuilding("validate");
		struct FlattenedDefinition {
			std::unordered_set<std::string> InheritedClasses;
			std::unordered_map<std::string, const InstanceProperty *> Properties;
			std::unordered_map<std::string, const UserdataMethod<Instance> *> Methods;
		};
		std::vector<SchemaClassDefinition *> ordered;
		for (auto &[id, definition] : DefinitionsById)
			if (auto *typed = std::get_if<SchemaClassDefinition>(&definition)) ordered.push_back(typed);
		std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
			return left->CanonicalName < right->CanonicalName;
		});
		for (const auto *definition : ordered) {
			if (!definition->Superclass) continue;
			auto base = DefinitionsById.find(*definition->BaseSchemaId);
			if (base == DefinitionsById.end()) InvalidDefinition(*definition, "base SchemaId is not registered");
			auto *baseClass = std::get_if<SchemaClassDefinition>(&base->second);
			if (!baseClass || baseClass->ClassName != *definition->Superclass)
				InvalidDefinition(*definition, "base name does not match a class base SchemaId");
		}
		std::unordered_map<SchemaId, unsigned char, SchemaIdHash> visitState;
		std::unordered_map<SchemaId, FlattenedDefinition, SchemaIdHash> flattened;
		std::function<void(SchemaClassDefinition &)> visit = [&](SchemaClassDefinition &definition) {
			auto &state = visitState[definition.Id];
			if (state == 1) InvalidDefinition(definition, "inheritance cycle detected");
			if (state == 2) return;
			state = 1;
			FlattenedDefinition result;
			if (definition.BaseSchemaId) {
				auto &base = std::get<SchemaClassDefinition>(DefinitionsById.at(*definition.BaseSchemaId));
				visit(base);
				result = flattened.at(base.Id);
				result.InheritedClasses.insert(base.ClassName);
			}
			for (auto &[name, property] : definition.Properties) result.Properties[name] = &property;
			for (auto &[name, method] : definition.Methods) result.Methods[name] = &method;
			flattened.emplace(definition.Id, std::move(result));
			state = 2;
		};
		for (auto *definition : ordered) visit(*definition);
		for (auto *definition : ordered) {
			auto &result = flattened.at(definition->Id);
			definition->InheritedClasses = std::move(result.InheritedClasses);
			definition->AllProperties = std::move(result.Properties);
			definition->AllMethods = std::move(result.Methods);
			definition->Flattened = true;
		}
		State = RuntimeSchemaRegistryState::Validated;
	}

	void RuntimeSchemaRegistry::Freeze() {
		if (State != RuntimeSchemaRegistryState::Validated)
			throw std::logic_error("Runtime schema registry cannot freeze while " +
				std::string(GetRuntimeSchemaRegistryStateName(State)));
		State = RuntimeSchemaRegistryState::Frozen;
	}

	const SchemaDefinition *RuntimeSchemaRegistry::FindDefinitionById(SchemaId id) const {
		RequireFrozen("look up a definition");
		auto definition = DefinitionsById.find(id);
		return definition == DefinitionsById.end() ? nullptr : &definition->second;
	}

	const SchemaDefinition *RuntimeSchemaRegistry::FindDefinitionByName(std::string_view name) const {
		RequireFrozen("look up a definition");
		if (name.find('.') != std::string_view::npos) {
			auto id = IdsByCanonicalName.find(std::string(name));
			return id == IdsByCanonicalName.end() ? nullptr : &DefinitionsById.at(id->second);
		}
		const SchemaDefinition *match = nullptr;
		for (const auto &[id, definition] : DefinitionsById) {
			if (GetSchemaDefinitionName(definition) != name) continue;
			if (match) return nullptr;
			match = &definition;
		}
		return match;
	}

	const SchemaClassDefinition *RuntimeSchemaRegistry::FindClassById(SchemaId id) const {
		auto definition = FindDefinitionById(id);
		return definition ? std::get_if<SchemaClassDefinition>(definition) : nullptr;
	}

	const SchemaClassDefinition *RuntimeSchemaRegistry::FindClassByType(std::type_index nativeType) const {
		RequireFrozen("look up a class definition");
		auto id = ClassIdsByType.find(nativeType);
		return id == ClassIdsByType.end() ? nullptr : FindClassById(id->second);
	}

	const SchemaClassDefinition *RuntimeSchemaRegistry::FindClassByName(std::string_view name) const {
		RequireFrozen("look up a class definition");
		if (name.find('.') != std::string_view::npos) {
			auto id = IdsByCanonicalName.find(std::string(name));
			return id == IdsByCanonicalName.end() ? nullptr : FindClassById(id->second);
		}
		const SchemaClassDefinition *match = nullptr;
		for (const auto &[id, definition] : DefinitionsById) {
			const auto *typed = std::get_if<SchemaClassDefinition>(&definition);
			if (!typed || typed->ClassName != name) continue;
			if (match) return nullptr;
			match = typed;
		}
		return match;
	}

	const SchemaEnumDefinition *RuntimeSchemaRegistry::FindEnumById(SchemaId id) const {
		auto definition = FindDefinitionById(id);
		return definition ? std::get_if<SchemaEnumDefinition>(definition) : nullptr;
	}

	const SchemaEnumDefinition *RuntimeSchemaRegistry::FindEnumByName(std::string_view name) const {
		RequireFrozen("look up an enum definition");
		if (name.find('.') != std::string_view::npos) {
			auto id = IdsByCanonicalName.find(std::string(name));
			return id == IdsByCanonicalName.end() ? nullptr : FindEnumById(id->second);
		}
		const SchemaEnumDefinition *match = nullptr;
		for (const auto &[id, definition] : DefinitionsById) {
			const auto *typed = std::get_if<SchemaEnumDefinition>(&definition);
			if (!typed || typed->Name != name) continue;
			if (match) return nullptr;
			match = typed;
		}
		return match;
	}

	std::vector<const SchemaDefinition *> RuntimeSchemaRegistry::EnumerateDefinitions() const {
		RequireFrozen("enumerate definitions");
		std::vector<const SchemaDefinition *> result;
		result.reserve(DefinitionsById.size());
		for (const auto &[id, definition] : DefinitionsById) result.push_back(&definition);
		std::sort(result.begin(), result.end(), [](const auto *left, const auto *right) {
			const auto leftName = GetSchemaDefinitionCanonicalName(*left);
			const auto rightName = GetSchemaDefinitionCanonicalName(*right);
			if (leftName != rightName) return leftName < rightName;
			return GetSchemaDefinitionKind(*left) < GetSchemaDefinitionKind(*right);
		});
		return result;
	}

	std::vector<const SchemaClassDefinition *> RuntimeSchemaRegistry::EnumerateClasses() const {
		std::vector<const SchemaClassDefinition *> result;
		for (const auto *definition : EnumerateDefinitions())
			if (const auto *typed = std::get_if<SchemaClassDefinition>(definition)) result.push_back(typed);
		return result;
	}

	std::string_view GetRuntimeSchemaRegistryStateName(RuntimeSchemaRegistryState state) {
		switch (state) {
			case RuntimeSchemaRegistryState::Building: return "Building";
			case RuntimeSchemaRegistryState::Validated: return "Validated";
			case RuntimeSchemaRegistryState::Frozen: return "Frozen";
		}
		throw std::invalid_argument("Unknown runtime schema registry state");
	}

	std::string_view GetSchemaProvenanceName(SchemaProvenance provenance) {
		switch (provenance) {
			case SchemaProvenance::NativeEngine: return "NativeEngine";
			case SchemaProvenance::CoreLuau: return "CoreLuau";
			case SchemaProvenance::Game: return "Game";
			case SchemaProvenance::Package: return "Package";
			case SchemaProvenance::Plugin: return "Plugin";
			case SchemaProvenance::Tooling: return "Tooling";
		}
		throw std::invalid_argument("Unknown schema provenance");
	}
}
