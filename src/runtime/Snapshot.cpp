#include "gargantuan/runtime/Snapshot.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <algorithm>
#include <any>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <set>
#include <unordered_set>

namespace gargantuan {
	namespace {
		using Json = nlohmann::ordered_json;

		Json EncodeId(WireObjectId id) {
			return EncodeWireObjectId(id);
		}

		std::optional<WireObjectId> DecodeId(const Json &value) {
			return DecodeWireObjectId(value);
		}

		std::optional<WireValue> EncodeNativeValue(const std::any &value) {
			return EncodeNativeWireValue(value);
		}

		std::optional<std::any> DecodeNativeValue(const WireValue &value) {
			return DecodeNativeWireValue(value);
		}

		Json EncodeValue(const WireValue &value) {
			return EncodeWireValue(value);
		}

		std::optional<WireValue> DecodeValue(const Json &encoded) {
			return DecodeWireValue(encoded);
		}

		void CollectObjects(
			const std::shared_ptr<Instance> &instance,
			std::vector<std::shared_ptr<Instance>> &objects,
			std::unordered_set<Instance *> &visited
		) {
			if (!instance || !visited.insert(instance.get()).second) throw std::runtime_error("Snapshot hierarchy is invalid");
			instance->AssertIsAlive();
			objects.push_back(instance);
			for (const auto &child : instance->Children) CollectObjects(child, objects, visited);
		}
	}

	std::shared_ptr<Instance> SnapshotLoadResult::Resolve(WireObjectId id) const {
		auto found = Objects.find(id);
		return found == Objects.end() || !found->second || found->second->GetDestroyed() ? nullptr : found->second;
	}

	Snapshot CaptureSnapshot(const std::shared_ptr<Instance> &root) {
		AssertAuthoritativeMutation("CaptureSnapshot");
		std::vector<std::shared_ptr<Instance>> instances;
		std::unordered_set<Instance *> visited;
		CollectObjects(root, instances, visited);

		std::unordered_map<Instance *, WireObjectId> ids;
		for (const auto &instance : instances)
			ids.emplace(instance.get(), WireObjectId::FromObjectId(instance->GetObjectId()));

		auto dataModel = std::dynamic_pointer_cast<DataModel>(root);
		if (!dataModel) throw std::runtime_error("Snapshot root must be a DataModel");
		Snapshot snapshot;
		for (const auto &instance : instances) {
			auto *definition = InstanceClassRegistry::GetDefinition(instance.get());
			if (!definition) throw std::runtime_error("Snapshot object has no class definition");
			SnapshotObject object{.Id = ids.at(instance.get()), .ClassName = definition->ClassName, .Name = instance->GetName()};
			if (auto parent = instance->GetParent()) {
				auto parentId = ids.find(parent->get());
				if (parentId == ids.end()) throw std::runtime_error("Snapshot contains an external parent");
				object.Parent = parentId->second;
			}

			for (const auto &[name, property] : definition->AllProperties) {
				if (name == "Name") continue;
				if (property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
					!property->Read || !property->Write)
					continue;
				if (property->ReadObjectReference) {
					auto referenced = property->ReadObjectReference(instance.get());
					if (!referenced) object.Properties.emplace(name, std::monostate{});
					else {
						if (referenced->GetDestroyed()) throw std::runtime_error("Snapshot property references a destroyed object");
						auto referencedId = ids.find(referenced.get());
						if (referencedId == ids.end()) throw std::runtime_error("Snapshot property references an external object");
						object.Properties.emplace(name, WireObjectReference{referencedId->second});
					}
				} else if (property->ReadEnumValue) {
					auto [enumName, enumValue] = property->ReadEnumValue(instance.get());
					auto enumType = Enums::GetEnums().find(enumName);
					if (enumType == Enums::GetEnums().end()) throw std::runtime_error("Snapshot enum type is not registered");
					auto item = enumType->second->FromValue(enumValue);
					if (!item) throw std::runtime_error("Snapshot enum value is not registered");
					object.Properties.emplace(name, WireEnumItem{enumName, std::string(item->Name)});
				} else {
					auto encoded = EncodeNativeValue(property->Read(instance.get()));
					if (!encoded) throw std::runtime_error("Snapshot property has no wire encoding: " + name);
					object.Properties.emplace(name, std::move(*encoded));
				}
			}
			object.Attributes = instance->GetAttributeValues(ScriptSecurityContext::CoreTrusted());
			(void)ValidateAttributeCollection(object.Attributes);
			for (const auto &[extensionId, properties] : instance->GetExtensionPropertyOverrides(
				ScriptSecurityContext::CoreTrusted()
			)) {
				auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(extensionId);
				if (!extension) throw std::runtime_error("Snapshot extension definition is missing");
				object.Extensions.push_back({extensionId, extension->DefinitionVersion, properties});
			}
			object.Tags = dataModel->Tags.GetTags(dataModel->GetObjectId(), instance->GetObjectId(), ScriptSecurityContext::CoreTrusted());
			snapshot.Objects.push_back(std::move(object));
		}
		const auto scope = root->GetReplicationScopeId();
		if (!scope.IsValid() || scope != root->GetObjectId())
			throw std::runtime_error("Snapshot root must be the DataModel that owns the replication scope");
		snapshot.Cursor = ChangeJournal::Get().CreateCursor(scope);
		return snapshot;
	}

	std::string SerializeSnapshot(const Snapshot &snapshot) {
		Json document;
		document["Version"] = snapshot.Version;
		document["Cursor"] = Json{
			{"Scope", EncodeId(WireObjectId::FromObjectId(snapshot.Cursor.Scope))},
			{"NextSequence", snapshot.Cursor.NextSequence},
		};
		document["Objects"] = Json::array();
		for (const auto &object : snapshot.Objects) {
			Json encoded;
			encoded["Id"] = EncodeId(object.Id);
			encoded["ClassName"] = object.ClassName;
			encoded["Name"] = object.Name;
			encoded["Parent"] = object.Parent ? EncodeId(*object.Parent) : Json(nullptr);
			encoded["Properties"] = Json::object();
			for (const auto &[name, value] : object.Properties) encoded["Properties"][name] = EncodeValue(value);
			encoded["Attributes"] = Json::object();
			for (const auto &[name, value] : object.Attributes) encoded["Attributes"][name] = EncodeValue(value);
			encoded["Extensions"] = Json::array();
			for (const auto &extension : object.Extensions) {
				Json extensionValue{
					{"ExtensionSchemaId", extension.ExtensionSchemaId.ToString()},
					{"DefinitionVersion", extension.DefinitionVersion},
					{"Properties", Json::object()},
				};
				for (const auto &[name, value] : extension.Properties)
					extensionValue["Properties"][name] = EncodeValue(value);
				encoded["Extensions"].push_back(std::move(extensionValue));
			}
			encoded["Tags"] = object.Tags;
			document["Objects"].push_back(std::move(encoded));
		}
		return document.dump();
	}

	SnapshotParseResult DeserializeSnapshot(std::string_view serialized) {
		SnapshotParseResult result;
		Json document;
		try {
			document = Json::parse(serialized);
		} catch (const std::exception &error) {
			result.Errors.push_back(error.what());
			return result;
		}
		try {
			if (!document.is_object() || document.value("Version", 0u) != SnapshotFormatVersion ||
				!document.contains("Cursor") || !document["Cursor"].is_object() ||
				!document["Cursor"].contains("Scope") || !document["Cursor"].contains("NextSequence") ||
				!document["Cursor"]["NextSequence"].is_number_unsigned() ||
				!document.contains("Objects") || !document["Objects"].is_array()) {
				result.Errors.push_back("Invalid or unsupported snapshot envelope");
				return result;
			}

			Snapshot snapshot;
			auto cursorScope = DecodeId(document["Cursor"]["Scope"]);
			if (!cursorScope) {
				result.Errors.push_back("Invalid snapshot cursor scope");
				return result;
			}
			snapshot.Cursor.Scope = cursorScope->ToObjectId();
			snapshot.Cursor.NextSequence = document["Cursor"]["NextSequence"].get<std::uint64_t>();
			if (!snapshot.Cursor.Scope.IsValid() || snapshot.Cursor.NextSequence == 0) {
				result.Errors.push_back("Invalid zero snapshot cursor");
				return result;
			}

			for (const auto &encoded : document["Objects"]) {
				if (!encoded.is_object() || !encoded.contains("Id") || !encoded.contains("ClassName") ||
					!encoded["ClassName"].is_string() || !encoded.contains("Name") || !encoded["Name"].is_string() ||
					!encoded.contains("Parent") || !encoded.contains("Properties") || !encoded["Properties"].is_object() ||
					!encoded.contains("Attributes") || !encoded["Attributes"].is_object() ||
					!encoded.contains("Extensions") || !encoded["Extensions"].is_array() ||
					!encoded.contains("Tags") || !encoded["Tags"].is_array()) {
					result.Errors.push_back("Invalid snapshot object");
					return result;
				}

				auto id = DecodeId(encoded["Id"]);
				if (!id) {
					result.Errors.push_back("Invalid snapshot ObjectId");
					return result;
				}

				SnapshotObject object{
					.Id = *id,
					.ClassName = encoded["ClassName"].get<std::string>(),
					.Name = encoded["Name"].get<std::string>()
				};
				if (!encoded["Parent"].is_null()) {
					auto parent = DecodeId(encoded["Parent"]);
					if (!parent) {
						result.Errors.push_back("Invalid snapshot parent ObjectId");
						return result;
					}
					object.Parent = *parent;
				}

				for (const auto &[name, encodedValue] : encoded["Properties"].items()) {
					auto value = DecodeValue(encodedValue);
					if (!value) {
						result.Errors.push_back("Invalid wire value for property " + name);
						return result;
					}
					object.Properties.emplace(name, std::move(*value));
				}
				for (const auto &[name, encodedValue] : encoded["Attributes"].items()) {
					auto value = DecodeValue(encodedValue);
					if (!value) {
						result.Errors.push_back("Invalid wire value for attribute " + name);
						return result;
					}
					object.Attributes.emplace(name, std::move(*value));
				}
				(void)ValidateAttributeCollection(object.Attributes);
				if (encoded["Extensions"].size() > MaximumCustomExtensionDefinitions)
					throw std::invalid_argument("Snapshot exceeds its extension definition state limit");
				auto *classDefinition = InstanceClassRegistry::GetDefinitionByName(object.ClassName);
				if (!classDefinition) throw std::invalid_argument("Snapshot class definition is missing");
				std::optional<SchemaId> previousExtensionId;
				for (const auto &encodedExtension : encoded["Extensions"]) {
					if (!encodedExtension.is_object() || !encodedExtension.contains("ExtensionSchemaId") ||
						!encodedExtension["ExtensionSchemaId"].is_string() ||
						!encodedExtension.contains("DefinitionVersion") ||
						!encodedExtension["DefinitionVersion"].is_number_unsigned() ||
						!encodedExtension.contains("Properties") || !encodedExtension["Properties"].is_object())
						throw std::invalid_argument("Snapshot extension state is malformed");
					auto extensionId = SchemaId::Parse(encodedExtension["ExtensionSchemaId"].get<std::string>());
					if (!extensionId || (previousExtensionId && !(*previousExtensionId < *extensionId)))
						throw std::invalid_argument("Snapshot extension identities are invalid or unordered");
					previousExtensionId = *extensionId;
					auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(*extensionId);
					const auto version = encodedExtension["DefinitionVersion"].get<std::uint32_t>();
					if (!extension || extension->DefinitionVersion != version)
						throw std::invalid_argument("Snapshot extension version is missing or incompatible");
					if (!GetActiveRuntimeSchemaRegistry().IsExtensionApplicableToClass(*extensionId, classDefinition->Id))
						throw std::invalid_argument("Snapshot extension does not apply to its object class");
					SnapshotExtensionState state{*extensionId, version, {}};
					if (encodedExtension["Properties"].empty() ||
						encodedExtension["Properties"].size() > MaximumExtensionProperties)
						throw std::invalid_argument("Snapshot extension property state is empty or oversized");
					for (const auto &[name, encodedValue] : encodedExtension["Properties"].items()) {
						auto *property = GetActiveRuntimeSchemaRegistry().FindExtensionProperty(*extensionId, name);
						auto value = DecodeValue(encodedValue);
						if (!property || !value) throw std::invalid_argument("Snapshot extension property is malformed or unknown");
						(void)ValidateSchemaExtensionPropertyValue(property->Type, *value);
						if (*value == property->DefaultValue)
							throw std::invalid_argument("Snapshot redundantly stores an extension default");
						state.Properties.emplace(name, std::move(*value));
					}
					if (state.Properties.empty()) throw std::invalid_argument("Snapshot extension state has no overrides");
					object.Extensions.push_back(std::move(state));
				}
				std::set<std::string> uniqueTags;
				for (const auto &tag : encoded["Tags"]) {
					if (!tag.is_string()) throw std::invalid_argument("Snapshot tag is not a string");
					auto name = tag.get<std::string>();
					ValidateTagName(name);
					if (!uniqueTags.insert(name).second) throw std::invalid_argument("Snapshot contains duplicate tags");
					if (uniqueTags.size() > MaximumTagsPerInstance) throw std::invalid_argument("Snapshot exceeds the per-Instance tag limit");
				}
				object.Tags.assign(uniqueTags.begin(), uniqueTags.end());
				snapshot.Objects.push_back(std::move(object));
			}
			result.Value = std::move(snapshot);
		} catch (const std::exception &error) {
			result.Errors.push_back(std::string("Invalid snapshot value: ") + error.what());
		}
		return result;
	}

	SnapshotLoadResult LoadSnapshot(const Snapshot &snapshot) {
		AssertAuthoritativeMutation("LoadSnapshot");
		SnapshotLoadResult result;
		if (snapshot.Version != SnapshotFormatVersion || !snapshot.Cursor.Scope.IsValid() ||
			snapshot.Cursor.NextSequence == 0) {
			result.Errors.push_back("Unsupported snapshot version");
			return result;
		}
		const auto rootObject = std::find_if(
			snapshot.Objects.begin(),
			snapshot.Objects.end(),
			[](const SnapshotObject &object) { return !object.Parent.has_value(); }
		);
		if (rootObject == snapshot.Objects.end() || rootObject->Id.ToObjectId() != snapshot.Cursor.Scope) {
			result.Errors.push_back("Snapshot root does not match its replication scope");
			return result;
		}

		for (const auto &object : snapshot.Objects) {
			if (!object.Id.IsValid() || result.Objects.contains(object.Id)) {
				result.Errors.push_back("Invalid or duplicate snapshot ObjectId");
				return result;
			}
			auto *definition = InstanceClassRegistry::GetDefinitionByName(object.ClassName);
			if (!definition || !definition->Constructor) {
				result.Errors.push_back("Unknown or non-constructible snapshot class: " + object.ClassName);
				return result;
			}
			auto instance = definition->Constructor();
			if (instance->ApplyPropertyMutation("Name", object.Name, Enums::Permission::Engine) != MutationStatus::Success) {
				result.Errors.push_back("Snapshot object name was rejected");
				return result;
			}
			result.Objects.emplace(object.Id, std::move(instance));
		}

		std::size_t rootCount = 0;
		try {
			for (const auto &object : snapshot.Objects) {
				auto instance = result.Resolve(object.Id);
				if (!object.Parent) {
					result.Root = instance;
					++rootCount;
					continue;
				}
				auto parent = result.Resolve(*object.Parent);
				if (!parent) throw std::runtime_error("Snapshot parent is stale or missing");
				instance->SetParent(parent);
			}
			if (rootCount != 1) throw std::runtime_error("Snapshot must contain exactly one root");
			auto dataModel = std::dynamic_pointer_cast<DataModel>(result.Root);
			if (!dataModel) throw std::runtime_error("Snapshot root is not a DataModel");

			for (const auto &object : snapshot.Objects) {
				auto instance = result.Resolve(object.Id);
				for (const auto &[name, value] : object.Properties) {
					auto *property = instance->FindProperty(name);
					if (!property || property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
						!property->Write)
						throw std::runtime_error("Snapshot contains an unknown or non-replicated property");
					if (std::holds_alternative<std::monostate>(value) ||
						std::holds_alternative<WireObjectReference>(value)) {
						if (!property->WriteObjectReference) throw std::runtime_error("Wire reference used for a value property");
						std::shared_ptr<Instance> referenced;
						if (auto *reference = std::get_if<WireObjectReference>(&value)) {
							referenced = result.Resolve(reference->Object);
							if (!referenced) throw std::runtime_error("Snapshot reference is stale or missing");
						}
						property->WriteObjectReference(instance.get(), referenced);
					} else if (auto *wireEnum = std::get_if<WireEnumItem>(&value)) {
						if (!property->WriteEnumValue || property->ReflectedTypedef != "Enum." + wireEnum->EnumType)
							throw std::runtime_error("Wire enum does not match the reflected property type");
						auto enumType = Enums::GetEnums().find(wireEnum->EnumType);
						if (enumType == Enums::GetEnums().end()) throw std::runtime_error("Wire enum type is unknown");
						auto item = enumType->second->FromName(wireEnum->Item);
						if (!item) throw std::runtime_error("Wire enum item is unknown");
						property->WriteEnumValue(instance.get(), item->Value);
					} else {
						auto native = DecodeNativeValue(value);
						if (!native || instance->ApplyPropertyMutation(name, *native, Enums::Permission::Engine) !=
							MutationStatus::Success)
							throw std::runtime_error("Snapshot property value was rejected");
					}
				}
				for (const auto &[name, value] : object.Attributes) {
					if (instance->ApplyAttributeMutation(name, value, ScriptSecurityContext::CoreTrusted()) !=
						MutationStatus::Success)
						throw std::runtime_error("Snapshot attribute value was rejected");
				}
				for (const auto &extension : object.Extensions) {
					for (const auto &[name, value] : extension.Properties) {
						if (instance->ApplyExtensionPropertyMutation(
							extension.ExtensionSchemaId,
							extension.DefinitionVersion,
							name,
							value,
							ScriptSecurityContext::CoreTrusted()
						) != MutationStatus::Success)
							throw std::runtime_error("Snapshot extension property value was rejected");
					}
				}
				for (const auto &tag : object.Tags)
					(void)dataModel->Tags.Add(dataModel->GetObjectId(), instance->GetObjectId(), tag, ScriptSecurityContext::CoreTrusted());
			}
		} catch (const std::exception &error) {
			result.Errors.push_back(error.what());
		}
		return result;
	}
}
