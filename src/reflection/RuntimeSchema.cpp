// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/reflection/RuntimeSchema.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		std::string CanonicalName(const SchemaDefinition &definition) {
			return definition.Namespace + "." + definition.ClassName;
		}

		[[noreturn]] void InvalidDefinition(const SchemaDefinition &definition, std::string_view reason) {
			throw std::invalid_argument(
				"Invalid schema definition '" + CanonicalName(definition) + "': " + std::string(reason)
			);
		}
	}

	void RuntimeSchemaRegistry::RequireBuilding(std::string_view operation) const {
		if (State != RuntimeSchemaRegistryState::Building)
			throw std::logic_error(
				"Runtime schema registry cannot " + std::string(operation) + " while " +
				std::string(GetRuntimeSchemaRegistryStateName(State))
			);
	}

	void RuntimeSchemaRegistry::RequireFrozen(std::string_view operation) const {
		if (State != RuntimeSchemaRegistryState::Frozen)
			throw std::logic_error(
				"Runtime schema registry cannot " + std::string(operation) + " while " +
				std::string(GetRuntimeSchemaRegistryStateName(State))
			);
	}

	void RuntimeSchemaRegistry::RegisterNative(std::type_index nativeType, SchemaDefinition definition) {
		RequireBuilding("register a native definition");
		if (!definition.Id.IsValid()) InvalidDefinition(definition, "SchemaId is invalid");
		if (definition.Namespace.empty()) InvalidDefinition(definition, "namespace is empty");
		if (definition.ClassName.empty()) InvalidDefinition(definition, "class name is empty");
		if (definition.DefinitionVersion == 0) InvalidDefinition(definition, "definition version is zero");
		if (definition.Provenance != SchemaProvenance::NativeEngine)
			InvalidDefinition(definition, "RegisterNative requires NativeEngine provenance");
		if (definition.Superclass.has_value() != definition.BaseSchemaId.has_value())
			InvalidDefinition(definition, "base name and base SchemaId must either both exist or both be absent");

		definition.CanonicalName = CanonicalName(definition);
		if (DefinitionsByType.contains(nativeType))
			throw std::invalid_argument("Native type is already registered in the runtime schema");
		if (TypesById.contains(definition.Id))
			throw std::invalid_argument("Duplicate SchemaId " + definition.Id.ToString());
		if (TypesByCanonicalName.contains(definition.CanonicalName))
			throw std::invalid_argument("Duplicate canonical schema name " + definition.CanonicalName);

		for (auto &[name, property] : definition.Properties) {
			if (name.empty() || property.Name != name) InvalidDefinition(definition, "property name/entry mismatch");
			if (property.ReflectedTypedef.empty()) InvalidDefinition(definition, "property has no reflected value type");
			if (!property.Read && !property.Write) InvalidDefinition(definition, "property has no native access path");
			if (property.ReplicationPolicy != InstanceProperty::Replication::None &&
				(!property.Read || !property.Write))
				InvalidDefinition(definition, "replicated property is not readable and writable");
			if (property.Editable && (!property.Write || property.WritePermission == Enums::Permission::Never))
				InvalidDefinition(definition, "editable property has no write path");
			if (property.DeclaringSchemaId.IsValid() && property.DeclaringSchemaId != definition.Id)
				InvalidDefinition(definition, "property declares a different owner");
			property.DeclaringSchemaId = definition.Id;
		}

		for (auto &[name, method] : definition.Methods) {
			if (name.empty() || !method.Call) InvalidDefinition(definition, "method is unnamed or has no native call path");
			if (definition.Properties.contains(name))
				InvalidDefinition(definition, "property and method names collide");
			if (method.DeclaringSchemaId.IsValid() && method.DeclaringSchemaId != definition.Id)
				InvalidDefinition(definition, "method declares a different owner");
			method.DeclaringSchemaId = definition.Id;
		}
		auto [definitionIterator, inserted] = DefinitionsByType.emplace(nativeType, std::move(definition));
		if (!inserted) throw std::logic_error("Schema native type collision changed during registration");
		try {
			TypesById.emplace(definitionIterator->second.Id, nativeType);
			TypesByCanonicalName.emplace(definitionIterator->second.CanonicalName, nativeType);
		} catch (...) {
			TypesById.erase(definitionIterator->second.Id);
			TypesByCanonicalName.erase(definitionIterator->second.CanonicalName);
			DefinitionsByType.erase(definitionIterator);
			throw;
		}
	}

	void RuntimeSchemaRegistry::Validate() {
		RequireBuilding("validate");
		struct FlattenedDefinition {
			std::unordered_set<std::string> InheritedClasses;
			std::unordered_map<std::string, const InstanceProperty *> Properties;
			std::unordered_map<std::string, const UserdataMethod<Instance> *> Methods;
		};

		std::vector<SchemaDefinition *> ordered;
		ordered.reserve(DefinitionsByType.size());
		for (auto &[type, definition] : DefinitionsByType) ordered.push_back(&definition);
		std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
			return left->CanonicalName < right->CanonicalName;
		});

		for (const auto *definition : ordered) {
			if (definition->Superclass) {
				auto baseType = TypesById.find(*definition->BaseSchemaId);
				if (baseType == TypesById.end()) InvalidDefinition(*definition, "base SchemaId is not registered");
				auto base = DefinitionsByType.find(baseType->second);
				if (base == DefinitionsByType.end() || base->second.ClassName != *definition->Superclass)
					InvalidDefinition(*definition, "base name does not match base SchemaId");
			}
		}

		std::unordered_map<SchemaId, unsigned char, SchemaIdHash> visitState;
		std::unordered_map<SchemaId, FlattenedDefinition, SchemaIdHash> flattened;
		std::function<void(SchemaDefinition &)> visit = [&](SchemaDefinition &definition) {
			auto &state = visitState[definition.Id];
			if (state == 1) InvalidDefinition(definition, "inheritance cycle detected");
			if (state == 2) return;
			state = 1;

			FlattenedDefinition result;
			if (definition.BaseSchemaId) {
				auto baseType = TypesById.find(*definition.BaseSchemaId);
				auto &base = DefinitionsByType.at(baseType->second);
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
			throw std::logic_error(
				"Runtime schema registry cannot freeze while " +
				std::string(GetRuntimeSchemaRegistryStateName(State))
			);
		State = RuntimeSchemaRegistryState::Frozen;
	}

	const SchemaDefinition *RuntimeSchemaRegistry::FindById(SchemaId id) const {
		RequireFrozen("look up a definition");
		auto type = TypesById.find(id);
		if (type == TypesById.end()) return nullptr;
		auto definition = DefinitionsByType.find(type->second);
		return definition == DefinitionsByType.end() ? nullptr : &definition->second;
	}

	const SchemaDefinition *RuntimeSchemaRegistry::FindByType(std::type_index nativeType) const {
		RequireFrozen("look up a definition");
		auto definition = DefinitionsByType.find(nativeType);
		return definition == DefinitionsByType.end() ? nullptr : &definition->second;
	}

	const SchemaDefinition *RuntimeSchemaRegistry::FindByName(std::string_view name) const {
		RequireFrozen("look up a definition");
		if (name.find('.') != std::string_view::npos) {
			auto type = TypesByCanonicalName.find(std::string(name));
			if (type == TypesByCanonicalName.end()) return nullptr;
			return &DefinitionsByType.at(type->second);
		}

		const SchemaDefinition *match = nullptr;
		for (const auto &[type, definition] : DefinitionsByType) {
			if (definition.ClassName != name) continue;
			if (match) return nullptr;
			match = &definition;
		}
		return match;
	}

	std::vector<const SchemaDefinition *> RuntimeSchemaRegistry::Enumerate() const {
		RequireFrozen("enumerate definitions");
		std::vector<const SchemaDefinition *> result;
		result.reserve(DefinitionsByType.size());
		for (const auto &[type, definition] : DefinitionsByType) result.push_back(&definition);
		std::sort(result.begin(), result.end(), [](const auto *left, const auto *right) {
			return left->CanonicalName < right->CanonicalName;
		});
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
