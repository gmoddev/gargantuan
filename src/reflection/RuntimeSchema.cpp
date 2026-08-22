// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <algorithm>
#include <cmath>
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

		std::string ExtensionCanonicalName(const SchemaExtensionDefinition &definition) {
			return definition.Namespace + "." + definition.Name;
		}

		template <typename Definition>
		[[noreturn]] void InvalidDefinition(const Definition &definition, std::string_view reason) {
			const auto canonical = [&]() {
				if constexpr (std::is_same_v<Definition, SchemaClassDefinition>) return ClassCanonicalName(definition);
				else if constexpr (std::is_same_v<Definition, SchemaEnumDefinition>) return EnumCanonicalName(definition);
				else return ExtensionCanonicalName(definition);
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
		if (std::holds_alternative<SchemaClassDefinition>(definition)) return SchemaDefinitionKind::Class;
		if (std::holds_alternative<SchemaEnumDefinition>(definition)) return SchemaDefinitionKind::Enum;
		return SchemaDefinitionKind::Extension;
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

	std::string_view GetSchemaExtensionPropertyTypeName(SchemaExtensionPropertyType type) {
		switch (type) {
			case SchemaExtensionPropertyType::Boolean: return "Boolean";
			case SchemaExtensionPropertyType::Integer: return "Integer";
			case SchemaExtensionPropertyType::Number: return "Number";
			case SchemaExtensionPropertyType::String: return "String";
		}
		throw std::invalid_argument("Unknown extension property type");
	}

	std::optional<SchemaExtensionPropertyType> ParseSchemaExtensionPropertyType(std::string_view type) {
		if (type == "Boolean") return SchemaExtensionPropertyType::Boolean;
		if (type == "Integer") return SchemaExtensionPropertyType::Integer;
		if (type == "Number") return SchemaExtensionPropertyType::Number;
		if (type == "String") return SchemaExtensionPropertyType::String;
		return std::nullopt;
	}

	std::size_t ValidateSchemaExtensionPropertyValue(
		SchemaExtensionPropertyType type,
		const WireValue &value
	) {
		bool valid = false;
		switch (type) {
			case SchemaExtensionPropertyType::Boolean:
				valid = std::holds_alternative<bool>(value);
				break;
			case SchemaExtensionPropertyType::Integer:
				valid = std::holds_alternative<int>(value);
				break;
			case SchemaExtensionPropertyType::Number:
				valid = std::holds_alternative<double>(value) && std::isfinite(std::get<double>(value));
				break;
			case SchemaExtensionPropertyType::String:
				if (const auto *string = std::get_if<std::string>(&value))
					valid = IsValidUtf8(*string) && string->find('\0') == std::string::npos;
				break;
		}
		if (!valid) throw std::invalid_argument(
			"Extension property value does not match declared " + std::string(GetSchemaExtensionPropertyTypeName(type)) + " type"
		);
		const auto bytes = MeasureWireValueJsonBytes(value);
		if (bytes > MaximumExtensionDefaultValueBytes)
			throw std::invalid_argument("Extension property value exceeds its encoded byte limit");
		return bytes;
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
		if (definition.ConstructionKind != SchemaClassConstructionKind::Native)
			InvalidDefinition(definition, "RegisterNative requires native construction metadata");
		if (definition.Superclass.has_value() != definition.BaseSchemaId.has_value())
			InvalidDefinition(definition, "base name and base SchemaId must either both exist or both be absent");
		definition.CanonicalName = ClassCanonicalName(definition);
		definition.Flattened = false;
		definition.InheritedClasses.clear();
		definition.AllProperties.clear();
		definition.AllMethods.clear();
		definition.InheritedClassIds.clear();
		definition.NativeHostClassId = definition.Id;
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
			if (property.SemanticType != InstanceProperty::DataType::Unsupported && property.WireType.empty())
				InvalidDefinition(definition, "semantic property has no wire type");
			if (property.Range &&
				((property.Range->Minimum && !std::isfinite(*property.Range->Minimum)) ||
				 (property.Range->Maximum && !std::isfinite(*property.Range->Maximum)) ||
				 (property.Range->Minimum && property.Range->Maximum &&
				  *property.Range->Minimum > *property.Range->Maximum)))
				InvalidDefinition(definition, "property numeric range is invalid");
			if (property.SemanticType == InstanceProperty::DataType::NativeEnum &&
				(!property.NativeEnumType || !property.ReadEnumValue || !property.WriteEnumValue))
				InvalidDefinition(definition, "native enum property has incomplete identity or access metadata");
			if (property.SemanticType == InstanceProperty::DataType::ObjectReference &&
				(!property.ObjectReferenceClassSchemaId || !property.ObjectReferenceClassSchemaId->IsValid() ||
				 !property.ReadObjectReference))
				InvalidDefinition(definition, "object-reference property has incomplete constraint metadata");
			if (property.DeclaringSchemaId.IsValid() && property.DeclaringSchemaId != definition.Id)
				InvalidDefinition(definition, "property declares a different owner");
			property.DeclaringSchemaId = definition.Id;
			property.DeclaringDefinitionVersion = definition.DefinitionVersion;
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

	void RuntimeSchemaRegistry::RegisterClass(
		SchemaClassDefinition definition,
		std::string_view baseCanonicalName
	) {
		RequireBuilding("register a custom class definition");
		ValidateIdentity(definition.Id, definition.Namespace, definition.ClassName, definition.DefinitionVersion, definition);
		if (definition.Provenance != SchemaProvenance::Game)
			InvalidDefinition(definition, "custom class registration requires Game provenance");
		if (definition.Namespace == "Engine" || definition.Namespace.starts_with("Engine."))
			InvalidDefinition(definition, "project classes cannot use the protected Engine namespace");
		if (definition.Id != SchemaId::FromCustomClassName(definition.Namespace, definition.ClassName))
			InvalidDefinition(definition, "SchemaId does not match deterministic custom class identity");
		if (definition.ConstructionKind != SchemaClassConstructionKind::CustomData || definition.Constructor)
			InvalidDefinition(definition, "custom class must use bounded data-only construction");
		if (!definition.Properties.empty() || !definition.Methods.empty())
			InvalidDefinition(definition, "custom class cannot supply native members or callbacks");
		if (baseCanonicalName.empty() || baseCanonicalName.find('.') == std::string_view::npos ||
			baseCanonicalName.size() > MaximumSchemaNamespaceBytes + MaximumSchemaDefinitionNameBytes + 1 ||
			baseCanonicalName.find('\0') != std::string_view::npos || !IsValidUtf8(baseCanonicalName))
			InvalidDefinition(definition, "base must be a valid canonical class name");
		if (definition.DeclaredCustomProperties.empty()) InvalidDefinition(definition, "custom class has no properties");
		if (definition.DeclaredCustomProperties.size() > MaximumCustomClassProperties)
			InvalidDefinition(definition, "custom class exceeds its property limit");

		definition.CanonicalName = ClassCanonicalName(definition);
		definition.PendingBaseCanonicalName = std::string(baseCanonicalName);
		definition.BaseSchemaId.reset();
		definition.Superclass.reset();
		definition.NativeHostClassId = {};
		definition.ProjectSubclassPolicy = CustomSubclassPolicy::DataOnly;
		definition.Flattened = false;
		definition.InheritedClasses.clear();
		definition.InheritedClassIds.clear();
		definition.AllProperties.clear();
		definition.AllMethods.clear();
		std::sort(definition.DeclaredCustomProperties.begin(), definition.DeclaredCustomProperties.end(),
			[](const auto &left, const auto &right) { return left.Name < right.Name; });
		std::unordered_set<std::string> names;
		std::size_t payloadBytes = definition.Namespace.size() + definition.ClassName.size() + baseCanonicalName.size();
		for (auto &property : definition.DeclaredCustomProperties) {
			if (property.Name.empty()) InvalidDefinition(definition, "custom property name is empty");
			if (property.Name.size() > MaximumExtensionPropertyNameBytes)
				InvalidDefinition(definition, "custom property name exceeds its byte limit");
			if (property.Name.find('\0') != std::string::npos || !IsValidUtf8(property.Name))
				InvalidDefinition(definition, "custom property name is invalid UTF-8 or contains null");
			if (!names.insert(property.Name).second)
				InvalidDefinition(definition, "duplicate custom property name " + property.Name);
			property.CanonicalName = definition.CanonicalName + "." + property.Name;
			try {
				payloadBytes += property.Name.size() + GetSchemaExtensionPropertyTypeName(property.Type).size() +
					ValidateSchemaExtensionPropertyValue(property.Type, property.DefaultValue);
			} catch (const std::exception &error) {
				InvalidDefinition(definition, "invalid default for property " + property.Name + ": " + error.what());
			}
			definition.Properties.emplace(property.Name, MakeCustomClassInstanceProperty(
				definition.Id, definition.DefinitionVersion, property
			));
		}
		if (payloadBytes > MaximumCustomSchemaPayloadBytes)
			InvalidDefinition(definition, "custom class definition exceeds its payload byte limit");
		if (payloadBytes > MaximumCustomSchemaPayloadBytes -
			std::min(CustomSchemaPayloadBytes, MaximumCustomSchemaPayloadBytes))
			InvalidDefinition(definition, "candidate exceeds its aggregate custom schema payload byte limit");
		const auto classCount = std::count_if(DefinitionsById.begin(), DefinitionsById.end(), [](const auto &entry) {
			const auto *candidate = std::get_if<SchemaClassDefinition>(&entry.second);
			return candidate && candidate->Provenance == SchemaProvenance::Game;
		});
		if (classCount >= MaximumCustomClassDefinitions)
			InvalidDefinition(definition, "candidate exceeds its custom class definition limit");
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

	void RuntimeSchemaRegistry::RegisterExtension(
		SchemaExtensionDefinition definition,
		std::string_view targetCanonicalName
	) {
		RequireBuilding("register an extension definition");
		ValidateIdentity(definition.Id, definition.Namespace, definition.Name, definition.DefinitionVersion, definition);
		if (definition.Provenance != SchemaProvenance::Game)
			InvalidDefinition(definition, "custom extension registration requires Game provenance");
		if (definition.Id != SchemaId::FromExtensionName(definition.Namespace, definition.Name))
			InvalidDefinition(definition, "SchemaId does not match deterministic extension identity");
		if (definition.TargetClassId.IsValid())
			InvalidDefinition(definition, "target SchemaId must be resolved by the canonical registry");
		if (targetCanonicalName.empty() || targetCanonicalName.find('.') == std::string_view::npos ||
			targetCanonicalName.size() > MaximumSchemaNamespaceBytes + MaximumSchemaDefinitionNameBytes + 1 ||
			targetCanonicalName.find('\0') != std::string_view::npos || !IsValidUtf8(targetCanonicalName))
			InvalidDefinition(definition, "target must be a valid canonical class name");
		auto targetIdentity = IdsByCanonicalName.find(std::string(targetCanonicalName));
		if (targetIdentity == IdsByCanonicalName.end()) InvalidDefinition(definition, "target definition is not registered");
		auto target = DefinitionsById.find(targetIdentity->second);
		if (target == DefinitionsById.end() || !std::holds_alternative<SchemaClassDefinition>(target->second))
			InvalidDefinition(definition, "target definition is not a class");
		definition.TargetClassId = targetIdentity->second;
		if (definition.Properties.empty()) InvalidDefinition(definition, "extension has no properties");
		if (definition.Properties.size() > MaximumExtensionProperties)
			InvalidDefinition(definition, "extension exceeds its property limit");
		definition.CanonicalName = ExtensionCanonicalName(definition);
		std::sort(definition.Properties.begin(), definition.Properties.end(), [](const auto &left, const auto &right) {
			return left.Name < right.Name;
		});
		std::unordered_set<std::string> names;
		std::size_t payloadBytes = definition.Namespace.size() + definition.Name.size() + targetCanonicalName.size();
		for (auto &property : definition.Properties) {
			if (property.Name.empty()) InvalidDefinition(definition, "extension property name is empty");
			if (property.Name.size() > MaximumExtensionPropertyNameBytes)
				InvalidDefinition(definition, "extension property name exceeds its byte limit");
			if (property.Name.find('\0') != std::string::npos || !IsValidUtf8(property.Name))
				InvalidDefinition(definition, "extension property name is invalid UTF-8 or contains null");
			if (!names.insert(property.Name).second)
				InvalidDefinition(definition, "duplicate extension property name " + property.Name);
			property.CanonicalName = definition.CanonicalName + "." + property.Name;
			try {
				payloadBytes += property.Name.size() + GetSchemaExtensionPropertyTypeName(property.Type).size() +
					ValidateSchemaExtensionPropertyValue(property.Type, property.DefaultValue);
			} catch (const std::exception &error) {
				InvalidDefinition(definition, "invalid default for property " + property.Name + ": " + error.what());
			}
		}
		if (payloadBytes > MaximumCustomSchemaPayloadBytes)
			InvalidDefinition(definition, "extension definition exceeds its payload byte limit");
		if (payloadBytes > MaximumCustomSchemaPayloadBytes -
			std::min(CustomSchemaPayloadBytes, MaximumCustomSchemaPayloadBytes))
			InvalidDefinition(definition, "candidate exceeds its aggregate custom schema payload byte limit");
		const auto extensionCount = std::count_if(DefinitionsById.begin(), DefinitionsById.end(), [](const auto &entry) {
			return std::holds_alternative<SchemaExtensionDefinition>(entry.second);
		});
		if (extensionCount >= MaximumCustomExtensionDefinitions)
			InvalidDefinition(definition, "candidate exceeds its custom extension definition limit");
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
			std::size_t Depth = 1;
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
		for (auto *definition : ordered) {
			if (definition->ConstructionKind == SchemaClassConstructionKind::CustomData) {
				if (!definition->PendingBaseCanonicalName)
					InvalidDefinition(*definition, "custom class has no pending canonical base");
				auto baseIdentity = IdsByCanonicalName.find(*definition->PendingBaseCanonicalName);
				if (baseIdentity == IdsByCanonicalName.end()) InvalidDefinition(*definition, "base definition is not registered");
				auto base = DefinitionsById.find(baseIdentity->second);
				auto *baseClass = base == DefinitionsById.end() ? nullptr : std::get_if<SchemaClassDefinition>(&base->second);
				if (!baseClass) InvalidDefinition(*definition, "base definition is not a class");
				if (baseClass->ProjectSubclassPolicy != CustomSubclassPolicy::DataOnly)
					InvalidDefinition(*definition, "base class does not permit project data-only subclasses");
				definition->BaseSchemaId = baseClass->Id;
				definition->Superclass = baseClass->ConstructionKind == SchemaClassConstructionKind::CustomData
					? baseClass->CanonicalName : baseClass->ClassName;
				definition->NativeHostClassId = baseClass->ConstructionKind == SchemaClassConstructionKind::CustomData
					? baseClass->NativeHostClassId : baseClass->Id;
			}
		}
		for (const auto *definition : ordered) {
			if (!definition->Superclass) continue;
			auto base = DefinitionsById.find(*definition->BaseSchemaId);
			if (base == DefinitionsById.end()) InvalidDefinition(*definition, "base SchemaId is not registered");
			auto *baseClass = std::get_if<SchemaClassDefinition>(&base->second);
			if (!baseClass || (baseClass->ClassName != *definition->Superclass &&
				baseClass->CanonicalName != *definition->Superclass))
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
				if (definition.ConstructionKind == SchemaClassConstructionKind::CustomData) {
					definition.NativeHostClassId = base.ConstructionKind == SchemaClassConstructionKind::CustomData
						? base.NativeHostClassId : base.Id;
					auto host = DefinitionsById.find(definition.NativeHostClassId);
					auto *hostClass = host == DefinitionsById.end()
						? nullptr : std::get_if<SchemaClassDefinition>(&host->second);
					if (!hostClass || hostClass->ConstructionKind != SchemaClassConstructionKind::Native ||
						!hostClass->Constructor || hostClass->ProjectSubclassPolicy != CustomSubclassPolicy::DataOnly)
						InvalidDefinition(definition, "custom class native host is not an approved data-only constructor");
				}
				result = flattened.at(base.Id);
				++result.Depth;
				result.InheritedClasses.insert(base.ClassName);
				result.InheritedClasses.insert(base.CanonicalName);
			}
			if (result.Depth > MaximumCustomClassInheritanceDepth)
				InvalidDefinition(definition, "inheritance exceeds its depth limit");
			for (auto &[name, property] : definition.Properties) {
				if (result.Properties.contains(name) || result.Methods.contains(name))
					InvalidDefinition(definition, "member " + name + " collides with an inherited member");
				result.Properties[name] = &property;
			}
			for (auto &[name, method] : definition.Methods) {
				if (result.Properties.contains(name) || result.Methods.contains(name))
					InvalidDefinition(definition, "member " + name + " collides with an inherited member");
				result.Methods[name] = &method;
			}
			flattened.emplace(definition.Id, std::move(result));
			state = 2;
		};
		for (auto *definition : ordered) visit(*definition);
		for (auto *definition : ordered) {
			auto &result = flattened.at(definition->Id);
			definition->InheritedClasses = std::move(result.InheritedClasses);
			definition->InheritedClasses.insert(definition->ClassName);
			definition->InheritedClasses.insert(definition->CanonicalName);
			definition->InheritedClassIds.clear();
			for (auto current = definition; current; current = current->BaseSchemaId
				? std::get_if<SchemaClassDefinition>(&DefinitionsById.at(*current->BaseSchemaId)) : nullptr)
				definition->InheritedClassIds.insert(current->Id);
			definition->AllProperties = std::move(result.Properties);
			definition->AllMethods = std::move(result.Methods);
			definition->Flattened = true;
		}
		for (auto &[id, candidate] : DefinitionsById) {
			(void)id;
			auto *extension = std::get_if<SchemaExtensionDefinition>(&candidate);
			if (!extension) continue;
			auto target = DefinitionsById.find(extension->TargetClassId);
			if (target == DefinitionsById.end()) InvalidDefinition(*extension, "target SchemaId is not registered");
			auto *targetClass = std::get_if<SchemaClassDefinition>(&target->second);
			if (!targetClass) InvalidDefinition(*extension, "target SchemaId does not identify a class");
			for (const auto &property : extension->Properties) {
				if (targetClass->AllProperties.contains(property.Name) || targetClass->AllMethods.contains(property.Name))
					InvalidDefinition(*extension, "property " + property.Name + " collides with a protected native member");
			}
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

	const SchemaClassProperty *RuntimeSchemaRegistry::FindCustomClassProperty(
		SchemaId declaringClassId,
		std::string_view propertyName
	) const {
		auto *definition = FindClassById(declaringClassId);
		if (!definition || definition->ConstructionKind != SchemaClassConstructionKind::CustomData) return nullptr;
		auto found = std::lower_bound(definition->DeclaredCustomProperties.begin(), definition->DeclaredCustomProperties.end(),
			propertyName, [](const SchemaClassProperty &property, std::string_view name) { return property.Name < name; });
		return found == definition->DeclaredCustomProperties.end() || found->Name != propertyName ? nullptr : &*found;
	}

	bool RuntimeSchemaRegistry::IsClassDerivedFrom(SchemaId classId, SchemaId baseClassId) const {
		auto *definition = FindClassById(classId);
		return definition && definition->InheritedClassIds.contains(baseClassId);
	}

	bool RuntimeSchemaRegistry::IsClassConstructible(const SchemaClassDefinition &definition) const {
		if (definition.ConstructionKind == SchemaClassConstructionKind::Native) return definition.Constructor != nullptr;
		if (!definition.NativeHostClassId.IsValid()) return false;
		auto *host = FindClassById(definition.NativeHostClassId);
		return host && host->ConstructionKind == SchemaClassConstructionKind::Native && host->Constructor &&
			host->ProjectSubclassPolicy == CustomSubclassPolicy::DataOnly;
	}

	const SchemaExtensionDefinition *RuntimeSchemaRegistry::FindExtensionById(SchemaId id) const {
		auto definition = FindDefinitionById(id);
		return definition ? std::get_if<SchemaExtensionDefinition>(definition) : nullptr;
	}

	const SchemaExtensionDefinition *RuntimeSchemaRegistry::FindExtensionByName(std::string_view name) const {
		RequireFrozen("look up an extension definition");
		if (name.find('.') != std::string_view::npos) {
			auto id = IdsByCanonicalName.find(std::string(name));
			return id == IdsByCanonicalName.end() ? nullptr : FindExtensionById(id->second);
		}
		const SchemaExtensionDefinition *match = nullptr;
		for (const auto &[id, definition] : DefinitionsById) {
			(void)id;
			const auto *typed = std::get_if<SchemaExtensionDefinition>(&definition);
			if (!typed || typed->Name != name) continue;
			if (match) return nullptr;
			match = typed;
		}
		return match;
	}

	const SchemaExtensionProperty *RuntimeSchemaRegistry::FindExtensionProperty(
		SchemaId extensionId,
		std::string_view propertyName
	) const {
		auto *extension = FindExtensionById(extensionId);
		if (!extension) return nullptr;
		auto found = std::lower_bound(extension->Properties.begin(), extension->Properties.end(), propertyName,
			[](const SchemaExtensionProperty &property, std::string_view name) { return property.Name < name; });
		return found == extension->Properties.end() || found->Name != propertyName ? nullptr : &*found;
	}

	bool RuntimeSchemaRegistry::IsExtensionApplicableToClass(SchemaId extensionId, SchemaId classId) const {
		auto *extension = FindExtensionById(extensionId);
		auto *definition = FindClassById(classId);
		if (!extension || !definition) return false;
		while (definition) {
			if (definition->Id == extension->TargetClassId) return true;
			definition = definition->BaseSchemaId ? FindClassById(*definition->BaseSchemaId) : nullptr;
		}
		return false;
	}

	std::vector<const SchemaExtensionDefinition *> RuntimeSchemaRegistry::FindApplicableExtensions(SchemaId classId) const {
		std::vector<const SchemaExtensionDefinition *> result;
		for (const auto *extension : EnumerateExtensions())
			if (IsExtensionApplicableToClass(extension->Id, classId)) result.push_back(extension);
		return result;
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

	std::vector<const SchemaExtensionDefinition *> RuntimeSchemaRegistry::EnumerateExtensions() const {
		std::vector<const SchemaExtensionDefinition *> result;
		for (const auto *definition : EnumerateDefinitions())
			if (const auto *typed = std::get_if<SchemaExtensionDefinition>(definition)) result.push_back(typed);
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
