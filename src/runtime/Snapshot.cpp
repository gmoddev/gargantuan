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
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "serialization/JsonCodec.hpp"
#include "runtime/SnapshotValidation.hpp"

#include <algorithm>
#include <any>
#include <stdexcept>
#include <set>
#include <unordered_set>

namespace gargantuan {
	namespace {
		using Json = JsonCodec::Json;

		Json EncodeId(WireObjectId id) {
			return JsonCodec::EncodeObjectId(id);
		}

		std::optional<WireObjectId> DecodeId(const Json &value) {
			return JsonCodec::DecodeObjectId(value);
		}

		std::optional<WireValue> EncodeNativeValue(const std::any &value) {
			return EncodeNativeWireValue(value);
		}

		std::optional<std::any> DecodeNativeValue(const WireValue &value) {
			return DecodeNativeWireValue(value);
		}

		Json EncodeValue(const WireValue &value) {
			return JsonCodec::EncodeWireValue(value);
		}

		std::optional<WireValue> DecodeValue(const Json &encoded) {
			return JsonCodec::DecodeWireValue(encoded);
		}

		bool HasOnlyFields(const Json &Value, std::initializer_list<std::string_view> Allowed) {
			if (!Value.is_object()) return false;
			for (const auto &[Name, Child] : Value.items()) {
				(void)Child;
				if (std::find(Allowed.begin(), Allowed.end(), Name) == Allowed.end()) return false;
			}
			return true;
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

		void ValidateSnapshotForLoad(const Snapshot &SnapshotValue) {
			if (SnapshotValue.Version != SnapshotFormatVersion || !SnapshotValue.Cursor.Scope.IsValid() ||
				SnapshotValue.Cursor.NextSequence == 0 || SnapshotValue.Objects.empty() ||
				SnapshotValue.Objects.size() > MaximumSnapshotObjects)
				throw std::invalid_argument("Snapshot envelope is invalid or oversized");

			std::unordered_map<WireObjectId, const SnapshotObject *> Objects;
			std::size_t RootCount = 0;
			for (const auto &Object : SnapshotValue.Objects) {
				if (!Object.Id.IsValid() || !Objects.emplace(Object.Id, &Object).second)
					throw std::invalid_argument("Snapshot contains an invalid or duplicate ObjectId");
				if (!Object.Parent) ++RootCount;
			}
			if (RootCount != 1)
				throw std::invalid_argument("Snapshot must contain exactly one root");

			const auto &Schema = GetActiveRuntimeSchemaRegistry();
			for (const auto &Object : SnapshotValue.Objects) {
				ValidateProtocolString(Object.ClassName, MaximumProtocolIdentifierBytes, "Snapshot class name");
				ValidateProtocolString(Object.Name, MaximumProtocolStringBytes, "Snapshot object name");
				auto *Definition = InstanceClassRegistry::GetDefinitionBySchemaId(Object.ClassSchemaId);
				const auto ExpectedName = Definition && Definition->ConstructionKind == SchemaClassConstructionKind::CustomData
					? Definition->CanonicalName : Definition ? Definition->ClassName : std::string{};
				if (!Definition || Definition->DefinitionVersion != Object.ClassDefinitionVersion ||
					!InstanceClassRegistry::IsConstructible(*Definition) || Object.ClassName != ExpectedName)
					throw std::invalid_argument("Snapshot class identity/version is incompatible");
				if (Object.Parent && !Objects.contains(*Object.Parent))
					throw std::invalid_argument("Snapshot parent is outside the receiving scope");
				if (Object.Properties.size() > MaximumSnapshotPropertiesPerObject)
					throw std::invalid_argument("Snapshot property count exceeds its limit");

				for (const auto &[Name, Value] : Object.Properties) {
					ValidateProtocolString(Name, MaximumProtocolIdentifierBytes, "Snapshot property name");
					ValidateProtocolWireValue(Value);
					auto PropertyPosition = Definition->AllProperties.find(Name);
					auto *Property = PropertyPosition == Definition->AllProperties.end() ? nullptr : PropertyPosition->second;
					if (!Property || Property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
						!Property->Write)
						throw std::invalid_argument("Snapshot contains an unknown or non-replicated property");
					if (std::holds_alternative<std::monostate>(Value) ||
						std::holds_alternative<WireObjectReference>(Value)) {
						if (!Property->WriteObjectReference)
							throw std::invalid_argument("Snapshot uses a reference for a value property");
						if (const auto *Reference = std::get_if<WireObjectReference>(&Value);
							Reference && !Objects.contains(Reference->Object))
							throw std::invalid_argument("Snapshot reference is outside the receiving scope");
					} else if (const auto *EnumValue = std::get_if<WireEnumItem>(&Value)) {
						if (!Property->WriteEnumValue || Property->ReflectedTypedef != "Enum." + EnumValue->EnumType)
							throw std::invalid_argument("Snapshot enum does not match its reflected property");
						auto EnumType = Enums::GetEnums().find(EnumValue->EnumType);
						if (EnumType == Enums::GetEnums().end() || !EnumType->second->FromName(EnumValue->Item))
							throw std::invalid_argument("Snapshot enum identity is unknown");
					} else {
						auto Native = DecodeNativeValue(Value);
						if (!Native || !Property->IsValueValid(*Native))
							throw std::invalid_argument("Snapshot property value is invalid");
					}
				}

				for (const auto &[Name, Value] : Object.Attributes) {
					(void)Name;
					ValidateProtocolWireValue(Value);
				}
				(void)ValidateAttributeCollection(Object.Attributes);
				if (Object.Extensions.size() > MaximumCustomExtensionDefinitions)
					throw std::invalid_argument("Snapshot extension state exceeds its definition limit");
				std::size_t ExtensionCount = 0;
				std::size_t ExtensionBytes = 0;
				std::optional<SchemaId> PreviousExtension;
				for (const auto &State : Object.Extensions) {
					auto *Extension = Schema.FindExtensionById(State.ExtensionSchemaId);
					if (!Extension || Extension->DefinitionVersion != State.DefinitionVersion ||
						(PreviousExtension && !(*PreviousExtension < State.ExtensionSchemaId)) ||
						!Schema.IsExtensionApplicableToClass(State.ExtensionSchemaId, Definition->Id) ||
						State.Properties.empty() || State.Properties.size() > MaximumExtensionProperties)
						throw std::invalid_argument("Snapshot extension state is incompatible");
					PreviousExtension = State.ExtensionSchemaId;
					for (const auto &[Name, Value] : State.Properties) {
						ValidateProtocolWireValue(Value);
						auto *Property = Schema.FindExtensionProperty(State.ExtensionSchemaId, Name);
						if (!Property || Value == Property->DefaultValue)
							throw std::invalid_argument("Snapshot extension property state is non-canonical");
						++ExtensionCount;
						ExtensionBytes += sizeof(SchemaId) + sizeof(State.DefinitionVersion) + Name.size() +
							ValidateSchemaExtensionPropertyValue(Property->Type, Value);
						if (ExtensionCount > MaximumExtensionOverridesPerInstance ||
							ExtensionBytes > MaximumExtensionOverrideBytesPerInstance)
							throw std::invalid_argument("Snapshot extension state exceeds its sparse-state limit");
					}
				}

				if (Object.CustomProperties.size() > MaximumCustomClassInheritanceDepth)
					throw std::invalid_argument("Snapshot custom property state exceeds its owner limit");
				std::size_t CustomCount = 0;
				std::size_t CustomBytes = 0;
				std::optional<SchemaId> PreviousOwner;
				for (const auto &State : Object.CustomProperties) {
					auto *Owner = Schema.FindClassById(State.DeclaringClassSchemaId);
					if (!Owner || Owner->ConstructionKind != SchemaClassConstructionKind::CustomData ||
						Owner->DefinitionVersion != State.DefinitionVersion ||
						(PreviousOwner && !(*PreviousOwner < State.DeclaringClassSchemaId)) ||
						!Schema.IsClassDerivedFrom(Definition->Id, State.DeclaringClassSchemaId) ||
						State.Properties.empty() || State.Properties.size() > MaximumCustomClassProperties)
						throw std::invalid_argument("Snapshot custom property state is incompatible");
					PreviousOwner = State.DeclaringClassSchemaId;
					for (const auto &[Name, Value] : State.Properties) {
						ValidateProtocolWireValue(Value);
						auto *Property = Schema.FindCustomClassProperty(State.DeclaringClassSchemaId, Name);
						if (!Property || Value == Property->DefaultValue)
							throw std::invalid_argument("Snapshot custom property state is non-canonical");
						++CustomCount;
						CustomBytes += sizeof(SchemaId) + sizeof(State.DefinitionVersion) + Name.size() +
							ValidateSchemaExtensionPropertyValue(Property->Type, Value);
						if (CustomCount > MaximumCustomPropertyOverridesPerInstance ||
							CustomBytes > MaximumCustomPropertyOverrideBytesPerInstance)
							throw std::invalid_argument("Snapshot custom property state exceeds its sparse-state limit");
					}
				}

				if (Object.Tags.size() > MaximumTagsPerInstance)
					throw std::invalid_argument("Snapshot tag state exceeds its limit");
				std::set<std::string> Tags;
				for (const auto &Tag : Object.Tags) {
					ValidateTagName(Tag);
					if (!Tags.insert(Tag).second) throw std::invalid_argument("Snapshot contains duplicate tags");
				}
			}

			std::unordered_map<WireObjectId, std::uint8_t> VisitState;
			VisitState.reserve(Objects.size());
			for (const auto &[Start, Object] : Objects) {
				(void)Object;
				if (VisitState[Start] == 2) continue;
				std::vector<WireObjectId> Path;
				auto Current = Start;
				while (true) {
					auto &State = VisitState[Current];
					if (State == 1) throw std::invalid_argument("Snapshot hierarchy contains a cycle");
					if (State == 2) break;
					State = 1;
					Path.push_back(Current);
					auto Position = Objects.find(Current);
					if (Position == Objects.end() || !Position->second->Parent) break;
					Current = *Position->second->Parent;
				}
				for (const auto Id : Path) VisitState[Id] = 2;
			}
		}
	}

	void ValidateSnapshotSemantic(const Snapshot &SnapshotValue) {
		ValidateSnapshotForLoad(SnapshotValue);
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
			SnapshotObject object{
				.Id = ids.at(instance.get()),
				.ClassSchemaId = definition->Id,
				.ClassDefinitionVersion = definition->DefinitionVersion,
				.ClassName = definition->ConstructionKind == SchemaClassConstructionKind::CustomData
					? definition->CanonicalName : definition->ClassName,
				.Name = instance->GetName(),
			};
			if (auto parent = instance->GetParent()) {
				auto parentId = ids.find(parent->get());
				if (parentId == ids.end()) throw std::runtime_error("Snapshot contains an external parent");
				object.Parent = parentId->second;
			}

			for (const auto &[name, property] : definition->AllProperties) {
				if (property->CustomSchemaPropertyType) continue;
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
			for (const auto &[declaringClassId, properties] : instance->GetCustomClassPropertyOverrides(
				ScriptSecurityContext::CoreTrusted()
			)) {
				auto *declaringClass = GetActiveRuntimeSchemaRegistry().FindClassById(declaringClassId);
				if (!declaringClass) throw std::runtime_error("Snapshot custom class definition is missing");
				object.CustomProperties.push_back({declaringClassId, declaringClass->DefinitionVersion, properties});
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
			encoded["ClassSchemaId"] = object.ClassSchemaId.ToString();
			encoded["ClassDefinitionVersion"] = object.ClassDefinitionVersion;
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
			encoded["CustomProperties"] = Json::array();
			for (const auto &state : object.CustomProperties) {
				Json encodedState{
					{"DeclaringClassSchemaId", state.DeclaringClassSchemaId.ToString()},
					{"DefinitionVersion", state.DefinitionVersion},
					{"Properties", Json::object()},
				};
				for (const auto &[name, value] : state.Properties)
					encodedState["Properties"][name] = EncodeValue(value);
				encoded["CustomProperties"].push_back(std::move(encodedState));
			}
			encoded["Tags"] = object.Tags;
			document["Objects"].push_back(std::move(encoded));
		}
		auto Encoded = JsonCodec::Encode(document, "Snapshot");
		if (!Encoded) throw std::runtime_error(Encoded.error().Format());
		return std::move(*Encoded);
	}

	SnapshotParseResult DeserializeSnapshot(std::string_view serialized) {
		SnapshotParseResult result;
		auto Parsed = JsonCodec::Parse(serialized, MaximumProtocolDocumentBytes, "Snapshot");
		if (!Parsed) {
			result.Errors.push_back(Parsed.error().Format());
			return result;
		}
		auto document = std::move(*Parsed);
		try {
			if (!HasOnlyFields(document, {"Version", "Cursor", "Objects"}) || document.size() != 3 ||
				document.value("Version", 0u) != SnapshotFormatVersion ||
				!document.contains("Cursor") || !document["Cursor"].is_object() ||
				!HasOnlyFields(document["Cursor"], {"Scope", "NextSequence"}) ||
				document["Cursor"].size() != 2 ||
				!document["Cursor"].contains("Scope") || !document["Cursor"].contains("NextSequence") ||
				!document["Cursor"]["NextSequence"].is_number_unsigned() ||
				!document.contains("Objects") || !document["Objects"].is_array()) {
				result.Errors.push_back("Invalid or unsupported snapshot envelope");
				return result;
			}
			if (document["Objects"].empty() || document["Objects"].size() > MaximumSnapshotObjects)
				throw std::invalid_argument("Snapshot object count is outside its supported range");

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
				if (!HasOnlyFields(encoded, {
						"Id", "ClassSchemaId", "ClassDefinitionVersion", "ClassName", "Name", "Parent",
						"Properties", "Attributes", "Extensions", "CustomProperties", "Tags"
					}) || encoded.size() != 11 || !encoded.contains("Id") || !encoded.contains("ClassSchemaId") ||
					!encoded["ClassSchemaId"].is_string() || !encoded.contains("ClassDefinitionVersion") ||
					!encoded["ClassDefinitionVersion"].is_number_unsigned() || !encoded.contains("ClassName") ||
					!encoded["ClassName"].is_string() || !encoded.contains("Name") || !encoded["Name"].is_string() ||
					!encoded.contains("Parent") || !encoded.contains("Properties") || !encoded["Properties"].is_object() ||
					!encoded.contains("Attributes") || !encoded["Attributes"].is_object() ||
					!encoded.contains("Extensions") || !encoded["Extensions"].is_array() ||
					!encoded.contains("CustomProperties") || !encoded["CustomProperties"].is_array() ||
					!encoded.contains("Tags") || !encoded["Tags"].is_array()) {
					result.Errors.push_back("Invalid snapshot object");
					return result;
				}
				ValidateProtocolString(
					encoded["ClassName"].get_ref<const std::string &>(), MaximumProtocolIdentifierBytes, "Snapshot class name"
				);
				ValidateProtocolString(
					encoded["Name"].get_ref<const std::string &>(), MaximumProtocolStringBytes, "Snapshot object name"
				);
				if (encoded["Properties"].size() > MaximumSnapshotPropertiesPerObject)
					throw std::invalid_argument("Snapshot property count exceeds its limit");

				auto id = DecodeId(encoded["Id"]);
				if (!id) {
					result.Errors.push_back("Invalid snapshot ObjectId");
					return result;
				}
				auto classId = SchemaId::Parse(encoded["ClassSchemaId"].get<std::string>());
				auto decodedClassVersion = JsonCodec::DecodeUnsigned32(encoded["ClassDefinitionVersion"]);
				const auto classVersion = decodedClassVersion.value_or(0);
				auto *classDefinition = classId ? GetActiveRuntimeSchemaRegistry().FindClassById(*classId) : nullptr;
				if (!classDefinition || classVersion == 0 || classDefinition->DefinitionVersion != classVersion)
					throw std::invalid_argument("Snapshot class identity/version is missing or incompatible");
				const auto expectedClassName = classDefinition->ConstructionKind == SchemaClassConstructionKind::CustomData
					? classDefinition->CanonicalName : classDefinition->ClassName;
				if (encoded["ClassName"].get<std::string>() != expectedClassName)
					throw std::invalid_argument("Snapshot class display identity is inconsistent");

				SnapshotObject object{
					.Id = *id,
					.ClassSchemaId = *classId,
					.ClassDefinitionVersion = classVersion,
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
					ValidateProtocolString(name, MaximumProtocolIdentifierBytes, "Snapshot property name");
					auto value = DecodeValue(encodedValue);
					if (!value) {
						result.Errors.push_back("Invalid wire value for property " + name);
						return result;
					}
					object.Properties.emplace(name, std::move(*value));
				}
				if (encoded["Attributes"].size() > MaximumAttributesPerInstance)
					throw std::invalid_argument("Snapshot attribute count exceeds its limit");
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
				std::optional<SchemaId> previousExtensionId;
				std::size_t ExtensionOverrideCount = 0;
				std::size_t ExtensionOverrideBytes = 0;
				for (const auto &encodedExtension : encoded["Extensions"]) {
					if (!HasOnlyFields(encodedExtension, {"ExtensionSchemaId", "DefinitionVersion", "Properties"}) ||
						encodedExtension.size() != 3 || !encodedExtension.contains("ExtensionSchemaId") ||
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
					auto decodedVersion = JsonCodec::DecodeUnsigned32(encodedExtension["DefinitionVersion"]);
					const auto version = decodedVersion.value_or(0);
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
						++ExtensionOverrideCount;
						ExtensionOverrideBytes += sizeof(SchemaId) + sizeof(extension->DefinitionVersion) + name.size() +
							ValidateSchemaExtensionPropertyValue(property->Type, *value);
						if (ExtensionOverrideCount > MaximumExtensionOverridesPerInstance ||
							ExtensionOverrideBytes > MaximumExtensionOverrideBytesPerInstance)
							throw std::invalid_argument("Snapshot exceeds its per-Instance extension override limits");
						if (*value == property->DefaultValue)
							throw std::invalid_argument("Snapshot redundantly stores an extension default");
						state.Properties.emplace(name, std::move(*value));
					}
					if (state.Properties.empty()) throw std::invalid_argument("Snapshot extension state has no overrides");
					object.Extensions.push_back(std::move(state));
				}
				if (encoded["CustomProperties"].size() > MaximumCustomClassInheritanceDepth)
					throw std::invalid_argument("Snapshot exceeds its custom property declaring-class state limit");
				std::optional<SchemaId> previousDeclaringClassId;
				std::size_t CustomOverrideCount = 0;
				std::size_t CustomOverrideBytes = 0;
				for (const auto &encodedState : encoded["CustomProperties"]) {
					if (!HasOnlyFields(encodedState, {"DeclaringClassSchemaId", "DefinitionVersion", "Properties"}) ||
						encodedState.size() != 3 || !encodedState.contains("DeclaringClassSchemaId") ||
						!encodedState["DeclaringClassSchemaId"].is_string() ||
						!encodedState.contains("DefinitionVersion") || !encodedState["DefinitionVersion"].is_number_unsigned() ||
						!encodedState.contains("Properties") || !encodedState["Properties"].is_object())
						throw std::invalid_argument("Snapshot custom property state is malformed");
					auto declaringId = SchemaId::Parse(encodedState["DeclaringClassSchemaId"].get<std::string>());
					if (!declaringId || (previousDeclaringClassId && !(*previousDeclaringClassId < *declaringId)))
						throw std::invalid_argument("Snapshot custom property declaring identities are invalid or unordered");
					previousDeclaringClassId = *declaringId;
					auto *declaringClass = GetActiveRuntimeSchemaRegistry().FindClassById(*declaringId);
					auto decodedVersion = JsonCodec::DecodeUnsigned32(encodedState["DefinitionVersion"]);
					const auto version = decodedVersion.value_or(0);
					if (!declaringClass || declaringClass->ConstructionKind != SchemaClassConstructionKind::CustomData ||
						declaringClass->DefinitionVersion != version ||
						!GetActiveRuntimeSchemaRegistry().IsClassDerivedFrom(classDefinition->Id, *declaringId))
						throw std::invalid_argument("Snapshot custom property owner/version is incompatible");
					if (encodedState["Properties"].empty() ||
						encodedState["Properties"].size() > MaximumCustomClassProperties)
						throw std::invalid_argument("Snapshot custom property state is empty or oversized");
					SnapshotCustomClassState state{*declaringId, version, {}};
					for (const auto &[name, encodedValue] : encodedState["Properties"].items()) {
						auto *property = GetActiveRuntimeSchemaRegistry().FindCustomClassProperty(*declaringId, name);
						auto value = DecodeValue(encodedValue);
						if (!property || !value) throw std::invalid_argument("Snapshot custom property is malformed or unknown");
						++CustomOverrideCount;
						CustomOverrideBytes += sizeof(SchemaId) + sizeof(version) + name.size() +
							ValidateSchemaExtensionPropertyValue(property->Type, *value);
						if (CustomOverrideCount > MaximumCustomPropertyOverridesPerInstance ||
							CustomOverrideBytes > MaximumCustomPropertyOverrideBytesPerInstance)
							throw std::invalid_argument("Snapshot exceeds its custom property override limits");
						if (*value == property->DefaultValue)
							throw std::invalid_argument("Snapshot redundantly stores a custom property default");
						state.Properties.emplace(name, std::move(*value));
					}
					object.CustomProperties.push_back(std::move(state));
				}
				std::set<std::string> uniqueTags;
				if (encoded["Tags"].size() > MaximumTagsPerInstance)
					throw std::invalid_argument("Snapshot tag count exceeds its limit");
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
		try {
			ValidateSnapshotForLoad(snapshot);
		} catch (const std::exception &Error) {
			result.Errors.push_back(Error.what());
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
			auto *definition = InstanceClassRegistry::GetDefinitionBySchemaId(object.ClassSchemaId);
			if (!definition || definition->DefinitionVersion != object.ClassDefinitionVersion ||
				!InstanceClassRegistry::IsConstructible(*definition)) {
				result.Errors.push_back("Unknown or non-constructible snapshot class: " + object.ClassName);
				return result;
			}
			const auto expectedName = definition->ConstructionKind == SchemaClassConstructionKind::CustomData
				? definition->CanonicalName : definition->ClassName;
			if (object.ClassName != expectedName) {
				result.Errors.push_back("Snapshot class identity is inconsistent: " + object.ClassName);
				return result;
			}
			auto instance = InstanceClassRegistry::Construct(*definition);
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
				for (const auto &state : object.CustomProperties) {
					for (const auto &[name, value] : state.Properties) {
						if (instance->ApplyCustomClassPropertyMutation(
							state.DeclaringClassSchemaId,
							state.DefinitionVersion,
							name,
							value,
							ScriptSecurityContext::CoreTrusted()
						) != MutationStatus::Success)
							throw std::runtime_error("Snapshot custom property value was rejected");
					}
				}
				for (const auto &tag : object.Tags)
					(void)dataModel->Tags.Add(dataModel->GetObjectId(), instance->GetObjectId(), tag, ScriptSecurityContext::CoreTrusted());
			}
		} catch (const std::exception &error) {
			result.Errors.push_back(error.what());
			result.Root.reset();
			result.Objects.clear();
		}
		return result;
	}
}
