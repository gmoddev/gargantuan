#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/TagIndex.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <SDL3/SDL_log.h>
#include <array>
#include <cmath>
#include <cstring>
#include <istream>
#include <memory>
#include <limits>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace gargantuan::InstanceSerialization {
	// Serialization

	using Serializable =
		std::variant<bool, CFrame, Color3, double, EnumItem, float, glm::vec3, int, std::string_view, UDim, Vector2>;

	namespace Json {
		// NOTE: InstanceFormat is based off Rojo's model format with a few exceptions:
		// - All properties are assumed to be explicit for forward compatibility
		// - Serializables are not restricted to just instances, ie. it can also be used
		//   for configuration files
		// - Because of this, removed Enum's explicit format to implement a new EnumItem
		//   explicit format that also specifies the EnumType
		// - Int32 and Int64 are merged into Int as those are irrelevant to Gargantuan

		using json = nlohmann::ordered_json;

		struct SerializationState {
			std::unordered_map<std::shared_ptr<Instance>, json> InstanceMap;
		};

		using SerializedPair = std::pair<const char *, json>;
		std::optional<SerializedPair> TrySerializeValue(std::any unknown) {
			if (!unknown.has_value()) return std::nullopt;

			if (auto *value = std::any_cast<bool>(&unknown)) {
				return SerializedPair{"Bool", *value};
			} else if (auto *value = std::any_cast<CFrame>(&unknown)) {
				glm::vec3 position = value->Position;
				glm::mat3 rotation = value->Rotation;
				std::vector<float> components{
					position.x,
					position.y,
					position.z,
					rotation[0][0],
					rotation[0][1],
					rotation[0][2],
					rotation[1][0],
					rotation[1][1],
					rotation[1][2],
					rotation[2][0],
					rotation[2][1],
					rotation[2][2]
				};
				return SerializedPair{"CFrame", components};
			} else if (auto *value = std::any_cast<Color3>(&unknown)) {
				return SerializedPair{"Color3", {value->R, value->G, value->B}};
			} else if (auto *value = std::any_cast<double>(&unknown)) {
				return SerializedPair{"Double", *value};
			} else if (auto *value = std::any_cast<EnumItem>(&unknown)) {
				return SerializedPair{"EnumItem", {value->EnumType->Name, value->Name}};
			} else if (auto *value = std::any_cast<float>(&unknown)) {
				return SerializedPair{"Float", *value};
			} else if (auto *value = std::any_cast<glm::vec3>(&unknown)) {
				return SerializedPair{"Vector3", {value->x, value->y, value->z}};
			} else if (auto *value = std::any_cast<int>(&unknown)) {
				return SerializedPair{"Int", *value};
			} else if (auto *value = std::any_cast<std::string_view>(&unknown)) {
				return SerializedPair{"String", std::string(value->data(), value->size())};
			} else if (auto *value = std::any_cast<UDim>(&unknown)) {
				return SerializedPair{"UDim", {value->Scale, value->Offset}};
			} else if (auto *value = std::any_cast<Vector2>(&unknown)) {
				return SerializedPair{"Vector2", {value->GetX(), value->GetY()}};
			} else {
				return std::nullopt;
			}
		}

		void
		SerializeProperties(
			const InstanceClassDefinition *definition,
			std::shared_ptr<Instance> instance,
			json &properties
		) {
			for (auto &[key, property] : definition->Properties) {
				if (key == "Parent" || property.CustomSchemaPropertyType ||
					property.PersistencePolicy != InstanceProperty::Persistence::Saved ||
					!property.Read || !property.Write)
					continue;

				auto value = property.Read(instance.get());
				if (auto serialized = TrySerializeValue(value); serialized.has_value()) {
					properties[key] = json::object({serialized.value()});
				}
			}

			if (definition->BaseSchemaId) {
				auto *superclass = InstanceClassRegistry::GetDefinitionBySchemaId(*definition->BaseSchemaId);
				if (!superclass) throw std::runtime_error("Published class has no registered base SchemaId");
				SerializeProperties(superclass, instance, properties);
			}
		}

		nlohmann::ordered_json SerializeInstance(std::shared_ptr<Instance> instance, SerializationState &state) {
			if (state.InstanceMap.contains(instance)) {
				return state.InstanceMap.at(instance);
			}

			std::vector<json> children;
			children.reserve(instance->Children.size());
			for (auto &child : instance->Children) {
				if (instance->GetArchivable()) children.emplace_back(SerializeInstance(child, state));
			}

			auto *definition = InstanceClassRegistry::GetDefinition(instance.get());
			auto properties = json::object();
			SerializeProperties(definition, instance, properties);

			nlohmann::ordered_json serialized;
			serialized["Name"] = instance->GetName();
			serialized["ClassName"] = definition->ConstructionKind == SchemaClassConstructionKind::CustomData
				? definition->CanonicalName : definition->ClassName;
			serialized["ClassSchemaId"] = definition->Id.ToString();
			serialized["ClassDefinitionVersion"] = definition->DefinitionVersion;
			serialized["Properties"] = properties;
			serialized["Attributes"] = json::object();
			for (const auto &[name, value] : instance->GetAttributeValues(ScriptSecurityContext::CoreTrusted()))
				serialized["Attributes"][name] = EncodeWireValue(value);
			serialized["Extensions"] = json::array();
			for (const auto &[extensionId, properties] : instance->GetExtensionPropertyOverrides(
				ScriptSecurityContext::CoreTrusted()
			)) {
				auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(extensionId);
				if (!extension) throw std::runtime_error("Serialized extension definition is missing");
				json encoded{
					{"ExtensionSchemaId", extensionId.ToString()},
					{"DefinitionVersion", extension->DefinitionVersion},
					{"Properties", json::object()},
				};
				for (const auto &[name, value] : properties)
					encoded["Properties"][name] = EncodeWireValue(value);
				serialized["Extensions"].push_back(std::move(encoded));
			}
			serialized["CustomProperties"] = json::array();
			for (const auto &[declaringClassId, properties] : instance->GetCustomClassPropertyOverrides(
				ScriptSecurityContext::CoreTrusted()
			)) {
				auto *declaringClass = GetActiveRuntimeSchemaRegistry().FindClassById(declaringClassId);
				if (!declaringClass) throw std::runtime_error("Serialized custom class definition is missing");
				json encoded{
					{"DeclaringClassSchemaId", declaringClassId.ToString()},
					{"DefinitionVersion", declaringClass->DefinitionVersion},
					{"Properties", json::object()},
				};
				for (const auto &[name, value] : properties)
					encoded["Properties"][name] = EncodeWireValue(value);
				serialized["CustomProperties"].push_back(std::move(encoded));
			}
			serialized["Tags"] = json::array();
			if (auto dataModel = instance->GetDataModel())
				for (const auto &tag : dataModel->Tags.GetTags(dataModel->GetObjectId(), instance->GetObjectId(), ScriptSecurityContext::CoreTrusted()))
					serialized["Tags"].push_back(tag);
			serialized["Children"] = children;

			state.InstanceMap.emplace(instance, serialized);
			return serialized;
		}
	}

	std::string Serialize(InstanceFormat format, std::shared_ptr<Instance> &instance) {
		switch (format) {
		case InstanceFormat::Json: {
			Json::SerializationState state;
			auto serialized = Json::SerializeInstance(instance, state);
			serialized["Version"] = 4;
			return serialized.dump();
		}
		default: {
			return "";
		}
		}
	};

	// Deserialization
	// FIXME: Current path refers to the parent when it really should refer to
	// the current child
	namespace {
		std::optional<std::string> ReadBoundedDocument(std::istream &Input) {
			std::string Result;
			std::array<char, 4096> Buffer{};
			while (Input) {
				Input.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
				const auto Count = static_cast<std::size_t>(Input.gcount());
				if (Count > MaximumProtocolDocumentBytes - Result.size()) return std::nullopt;
				Result.append(Buffer.data(), Count);
			}
			return Result;
		}
	}

	std::string DeserializationState::FormatCurrentPath() {
		std::ostringstream stream;
		int segmentCount = CurrentPath.size();
		for (int idx = 0; idx < segmentCount; idx++) {
			auto segment = CurrentPath[idx];
			stream << segment;
			if (idx < segmentCount - 1) stream << " -> ";
		}
		return stream.str();
	}

	std::optional<std::any> TryDeserializeProperty(const json &unknown, DeserializationState &state) {
		auto DecodeFloat = [](const json &Value) -> std::optional<float> {
			if (!Value.is_number()) return std::nullopt;
			const auto Decoded = Value.get<double>();
			if (!std::isfinite(Decoded) || Decoded < -std::numeric_limits<float>::max() ||
				Decoded > std::numeric_limits<float>::max()) return std::nullopt;
			return static_cast<float>(Decoded);
		};
		auto DecodeInt = [](const json &Value) -> std::optional<int> {
			if (Value.is_number_unsigned()) {
				const auto Decoded = Value.get<std::uint64_t>();
				if (Decoded > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return std::nullopt;
				return static_cast<int>(Decoded);
			}
			if (!Value.is_number_integer()) return std::nullopt;
			const auto Decoded = Value.get<std::int64_t>();
			if (Decoded < std::numeric_limits<int>::min() || Decoded > std::numeric_limits<int>::max()) return std::nullopt;
			return static_cast<int>(Decoded);
		};
		if (unknown.size() == 0) return state.ReturnError("Missing explicit property to infer from");
		if (unknown.size() > 1) return state.ReturnError("Got too many possible property types");

		if (unknown.contains("Bool")) {
			auto value = unknown["Bool"];
			if (!value.is_boolean()) return state.ReturnError("Expected a boolean");
			return value.get<bool>();
		} else if (unknown.contains("CFrame")) {
			auto value = unknown["CFrame"];
			if (!value.is_array() || value.size() != 12) {
				return state.ReturnError("Expected CFrame to be an array of 12 components");
			};

			auto x = value[0];
			if (!x.is_number()) return state.ReturnError("Expected X component to be a float");

			auto y = value[1];
			if (!y.is_number()) return state.ReturnError("Expected Y component to be a float");

			auto z = value[2];
			if (!z.is_number()) return state.ReturnError("Expected Z component to be a float");

			auto r00 = value[3];
			if (!r00.is_number()) return state.ReturnError("Expected R00 component to be a float");

			auto r01 = value[4];
			if (!r01.is_number()) return state.ReturnError("Expected R01 component to be a float");

			auto r02 = value[5];
			if (!r02.is_number()) return state.ReturnError("Expected R02 component to be a float");

			auto r10 = value[6];
			if (!r10.is_number()) return state.ReturnError("Expected R10 component to be a float");

			auto r11 = value[7];
			if (!r11.is_number()) return state.ReturnError("Expected R11 component to be a float");

			auto r12 = value[8];
			if (!r12.is_number()) return state.ReturnError("Expected R12 component to be a float");

			auto r20 = value[9];
			if (!r20.is_number()) return state.ReturnError("Expected R20 component to be a float");

			auto r21 = value[10];
			if (!r21.is_number()) return state.ReturnError("Expected R21 component to be a float");

			auto r22 = value[11];
			if (!r22.is_number()) return state.ReturnError("Expected R22 component to be a float");

			auto X = DecodeFloat(x); auto Y = DecodeFloat(y); auto Z = DecodeFloat(z);
			auto R00 = DecodeFloat(r00); auto R01 = DecodeFloat(r01); auto R02 = DecodeFloat(r02);
			auto R10 = DecodeFloat(r10); auto R11 = DecodeFloat(r11); auto R12 = DecodeFloat(r12);
			auto R20 = DecodeFloat(r20); auto R21 = DecodeFloat(r21); auto R22 = DecodeFloat(r22);
			if (!X || !Y || !Z || !R00 || !R01 || !R02 || !R10 || !R11 || !R12 || !R20 || !R21 || !R22)
				return state.ReturnError("CFrame contains a non-finite or out-of-range component");
			return CFrame(
				glm::vec3(*X, *Y, *Z),
				glm::mat3(
					glm::vec3(*R00, *R10, *R20),
					glm::vec3(*R01, *R11, *R21),
					glm::vec3(*R02, *R12, *R22)
				)
			);
		} else if (unknown.contains("Color3")) {
			auto value = unknown["Color3"];
			if (!value.is_array() || value.size() != 3) {
				return state.ReturnError("Expected Color3 to be an array of RGB components");
			};

			auto r = value[0];
			if (!r.is_number()) return state.ReturnError("Expected red component to be a float");

			auto g = value[1];
			if (!g.is_number()) return state.ReturnError("Expected green component to be a float");

			auto b = value[2];
			if (!b.is_number()) return state.ReturnError("Expected blue component to be a float");

			auto R = DecodeFloat(r); auto G = DecodeFloat(g); auto B = DecodeFloat(b);
			if (!R || !G || !B) return state.ReturnError("Color3 contains a non-finite or out-of-range component");
			return Color3(*R, *G, *B);
		} else if (unknown.contains("Double")) {
			auto value = unknown["Double"];
			if (!value.is_number()) return state.ReturnError("Expected double");
			const auto Decoded = value.get<double>();
			if (!std::isfinite(Decoded)) return state.ReturnError("Expected a finite double");
			return Decoded;
		} else if (unknown.contains("EnumItem")) {
			auto value = unknown["EnumItem"];
			if (!value.is_array() || value.size() != 2) return state.ReturnError("Expected EnumItem to be an array of 2 strings");

			auto enumType = value[0];
			if (!enumType.is_string()) return state.ReturnError("Expected EnumType to be a string");

			auto enumName = value[1];
			if (!enumName.is_string()) return state.ReturnError("Expected EnumName to be a string");
			try {
				ValidateProtocolString(
					enumType.get_ref<const std::string &>(), MaximumProtocolIdentifierBytes, "Persisted enum type"
				);
				ValidateProtocolString(
					enumName.get_ref<const std::string &>(), MaximumProtocolIdentifierBytes, "Persisted enum item"
				);
			} catch (const std::exception &Error) {
				return state.ReturnError("Invalid persisted enum identity: {}", Error.what());
			}

			auto enumTypeString = enumType.get<std::string>();
			auto &enums = Enums::GetEnums();
			if (!enums.contains(enumTypeString)) return state.ReturnError("Unknown EnumType {}", enumTypeString);

			auto enumNameString = enumName.get<std::string>();
			for (auto &item : enums.at(enumTypeString)->Items) {
				if (item.Name == enumNameString) {
					return item;
				}
			}

			return state.ReturnError("Unknown EnumItem named {} of Enum {}", enumNameString, enumTypeString);
		} else if (unknown.contains("Float")) {
			auto value = unknown["Float"];
			if (!value.is_number()) return state.ReturnError("Expected float");
			auto Decoded = DecodeFloat(value);
			if (!Decoded) return state.ReturnError("Expected a finite in-range float");
			return *Decoded;
		} else if (unknown.contains("Vector3")) {
			auto value = unknown["Vector3"];
			if (!value.is_array() || value.size() != 3) return state.ReturnError("Expected Vector3 to be an array of 3 components");

			auto x = value[0];
			if (!x.is_number()) return state.ReturnError("Expected X component to be a float");

			auto y = value[1];
			if (!y.is_number()) return state.ReturnError("Expected Y component to be a float");

			auto z = value[2];
			if (!z.is_number()) return state.ReturnError("Expected Z component to be a float");

			auto X = DecodeFloat(x); auto Y = DecodeFloat(y); auto Z = DecodeFloat(z);
			if (!X || !Y || !Z) return state.ReturnError("Vector3 contains a non-finite or out-of-range component");
			return glm::vec3(*X, *Y, *Z);
		} else if (unknown.contains("Int")) {
			auto value = unknown["Int"];
			auto Decoded = DecodeInt(value);
			if (!Decoded) return state.ReturnError("Expected a signed 32-bit int");
			return *Decoded;
		} else if (unknown.contains("String")) {
			auto value = unknown["String"];
			if (!value.is_string()) return state.ReturnError("Expected string");
			const auto &Decoded = value.get_ref<const std::string &>();
			try { ValidateProtocolString(Decoded, MaximumProtocolStringBytes, "Persisted string"); }
			catch (const std::exception &Error) { return state.ReturnError("Invalid persisted string: {}", Error.what()); }
			return Decoded;
		} else if (unknown.contains("UDim")) {
			auto value = unknown["UDim"];
			if (!value.is_array() || value.size() != 2) return state.ReturnError("Expected UDim to be an array of 2 components");

			auto scale = value[0];
			if (!scale.is_number()) return state.ReturnError("Expected Scale component to be a float");

			auto offset = value[1];
			if (!offset.is_number_integer()) return state.ReturnError("Expected Offset component to be an integer");

			auto Scale = DecodeFloat(scale); auto Offset = DecodeInt(offset);
			if (!Scale || !Offset) return state.ReturnError("UDim contains a non-finite or out-of-range component");
			return UDim(*Scale, *Offset);
		} else if (unknown.contains("Vector2")) {
			auto value = unknown["Vector2"];
			if (!value.is_array() || value.size() != 2) return state.ReturnError("Expected Vector2 to be an array of 2 components");

			auto x = value[0];
			if (!x.is_number()) return state.ReturnError("Expected X component to be a float");

			auto y = value[1];
			if (!y.is_number()) return state.ReturnError("Expected Y component to be a float");

			auto X = DecodeFloat(x); auto Y = DecodeFloat(y);
			if (!X || !Y) return state.ReturnError("Vector2 contains a non-finite or out-of-range component");
			return Vector2(*X, *Y);
		}

		return state.ReturnError("Unsupported property value: %s", unknown.dump());
	};

	std::optional<std::shared_ptr<Instance>> TryDeserializeInstance(
		const json &contents,
		DeserializationState &state,
		bool requireAttributes,
		bool requireTags,
		bool requireExtensions,
		bool requireClassIdentity,
		bool requireCustomProperties
	) {
		if (state.CurrentPath.size() > MaximumProtocolJsonDepth)
			return state.ReturnError("Instance hierarchy exceeds its depth limit at {}", state.FormatCurrentPath());
		if (state.ObjectsDecoded == MaximumPersistenceObjects)
			return state.ReturnError("Instance document exceeds its object-count limit");
		++state.ObjectsDecoded;
		if (!contents.is_object())
			return state.ReturnError("Instance {} is not an object", state.FormatCurrentPath());
		if (!contents.contains("Name") || !contents["Name"].is_string()) {
			state.PushError("Child under {}has an invalid Name field", state.FormatCurrentPath());
			return std::nullopt;
		}
		const auto &name = contents["Name"];
		try { ValidateProtocolString(name.get_ref<const std::string &>(), MaximumProtocolStringBytes, "Instance name"); }
		catch (const std::exception &Error) { return state.ReturnError("Invalid Instance name: {}", Error.what()); }

		state.CurrentPath.push_back(name.get<std::string>());

		if (!contents.contains("Properties") || !contents["Properties"].is_object()) {
			state.PushError("Instance {} has an invalid Properties field", state.FormatCurrentPath());
			return std::nullopt;
		}
		const auto &properties = contents["Properties"];
		if (properties.size() > MaximumSnapshotPropertiesPerObject)
			return state.ReturnError("Instance {} exceeds its property-count limit", state.FormatCurrentPath());
		if (requireAttributes && (!contents.contains("Attributes") || !contents["Attributes"].is_object())) {
			state.PushError("Instance {} has an invalid Attributes field", state.FormatCurrentPath());
			return std::nullopt;
		}
		const auto attributes = contents.contains("Attributes") ? contents["Attributes"] : json::object();
		if (!attributes.is_object()) {
			state.PushError("Instance {} has an invalid Attributes field", state.FormatCurrentPath());
			return std::nullopt;
		}
		if (requireTags && (!contents.contains("Tags") || !contents["Tags"].is_array())) {
			state.PushError("Instance {} has an invalid Tags field", state.FormatCurrentPath());
			return std::nullopt;
		}
		auto tags = contents.contains("Tags") ? contents["Tags"] : json::array();
		if (!tags.is_array()) {
			state.PushError("Instance {} has an invalid Tags field", state.FormatCurrentPath());
			return std::nullopt;
		}
		if (requireExtensions && (!contents.contains("Extensions") || !contents["Extensions"].is_array())) {
			state.PushError("Instance {} has an invalid Extensions field", state.FormatCurrentPath());
			return std::nullopt;
		}
		auto extensions = contents.contains("Extensions") ? contents["Extensions"] : json::array();
		if (!extensions.is_array()) {
			state.PushError("Instance {} has an invalid Extensions field", state.FormatCurrentPath());
			return std::nullopt;
		}
		if (requireCustomProperties && (!contents.contains("CustomProperties") ||
			!contents["CustomProperties"].is_array())) {
			state.PushError("Instance {} has an invalid CustomProperties field", state.FormatCurrentPath());
			return std::nullopt;
		}
		auto customProperties = contents.contains("CustomProperties") ? contents["CustomProperties"] : json::array();
		if (!customProperties.is_array()) {
			state.PushError("Instance {} has an invalid CustomProperties field", state.FormatCurrentPath());
			return std::nullopt;
		}

		if (!contents.contains("Children") || !contents["Children"].is_array()) {
			state.PushError("Instance {} has an invalid Children field", state.FormatCurrentPath());
			return std::nullopt;
		}
		const auto &children = contents["Children"];
		if (children.size() > MaximumPersistenceObjects)
			return state.ReturnError("Instance {} exceeds its child-count limit", state.FormatCurrentPath());

		auto maybeClassName = contents["ClassName"];
		if (!maybeClassName.is_string()) {
			state.PushError("Instance {} has an invalid ClassName field", state.FormatCurrentPath());
			return std::nullopt;
		}

		auto className = maybeClassName.get<std::string>();
		try { ValidateProtocolString(className, MaximumProtocolIdentifierBytes, "Persisted class name"); }
		catch (const std::exception &Error) { return state.ReturnError("Invalid class name: {}", Error.what()); }
		const InstanceClassDefinition *definition = nullptr;
		if (requireClassIdentity) {
			if (!contents.contains("ClassSchemaId") || !contents["ClassSchemaId"].is_string() ||
				!contents.contains("ClassDefinitionVersion") || !contents["ClassDefinitionVersion"].is_number_unsigned()) {
				state.PushError("Instance {} has invalid stable class identity fields", state.FormatCurrentPath());
				return std::nullopt;
			}
			auto classId = SchemaId::Parse(contents["ClassSchemaId"].get<std::string>());
			auto decodedVersion = DecodeWireUnsigned32(contents["ClassDefinitionVersion"]);
			const auto version = decodedVersion.value_or(0);
			definition = classId ? InstanceClassRegistry::GetDefinitionBySchemaId(*classId) : nullptr;
			const auto expectedName = definition && definition->ConstructionKind == SchemaClassConstructionKind::CustomData
				? definition->CanonicalName : definition ? definition->ClassName : std::string{};
			if (!definition || version == 0 || definition->DefinitionVersion != version || className != expectedName) {
				state.PushError("Instance {} has missing or incompatible class identity/version", state.FormatCurrentPath());
				return std::nullopt;
			}
		} else {
			definition = InstanceClassRegistry::GetDefinitionByName(className);
			if (definition && definition->ConstructionKind != SchemaClassConstructionKind::Native) definition = nullptr;
		}
		if (!definition) {
			state.PushError("Instance {} has unknown ClassName '{}'", state.FormatCurrentPath(), className);
			return std::nullopt;
		} else if (!InstanceClassRegistry::IsConstructible(*definition)) {
			state.PushError("Cannot deserialize instance {} of class {}", state.FormatCurrentPath(), className);
			return std::nullopt;
		}

		LOG_INFO(App, "Registered property count for %s: %zu", className.c_str(), definition->AllProperties.size());
		auto instance = InstanceClassRegistry::Construct(*definition);
		if (instance->ApplyPropertyMutation("Name", name.get<std::string>(), Enums::Permission::Engine) !=
			MutationStatus::Success) {
			instance->Destroy();
			return state.ReturnError("Failed to apply Name for {}", state.FormatCurrentPath());
		}
		for (auto &[key, property] : definition->AllProperties) {
			if (property->CustomSchemaPropertyType) continue;
			LOG_INFO(App, "Trying to deserialize %s of %s", key.data(), state.FormatCurrentPath().data());
			if (key == "Parent" || !properties.contains(key) ||
				property->PersistencePolicy != InstanceProperty::Persistence::Saved || !property->Write)
				continue;
			LOG_INFO(App, "Deserializing %s of %s", key.data(), state.FormatCurrentPath().data());

			auto value = properties[key];
			if (!value.is_object()) {
				state.PushError("Invalid value of property '{}' in {}", key, state.FormatCurrentPath());
				instance->Destroy();
				return std::nullopt;
			}

			auto maybeDeserialized = TryDeserializeProperty(value, state);
			if (!maybeDeserialized.has_value()) {
				state.PushError("Failed to deserialize value of property '{}' in {}", key, state.FormatCurrentPath());
				instance->Destroy();
				return std::nullopt;
			}

			auto deserialized = maybeDeserialized.value();

			try {
				if (property->Validate && !property->Validate(deserialized)) {
					instance->Destroy();
					return state.ReturnError("Validation failed for property '{}' in {}", key, state.FormatCurrentPath());
				}
				const auto status = instance->ApplyPropertyMutation(key, deserialized, Enums::Permission::Engine);
				if (status != MutationStatus::Success) throw std::runtime_error("Serialized property mutation rejected");
			} catch (const std::bad_any_cast &e) {
				instance->Destroy();
				return state.ReturnError(
					"Type mismatch on property '{}' in {}, expected {}, got approximately {}",
					key,
					state.FormatCurrentPath(),
					property->ReflectedTypedef,
					typeid(deserialized).name()
				);
			} catch (const std::exception &e) {
				instance->Destroy();
				return state.ReturnError(
					"Failed to set value of property '{}' in {}: {}", key, state.FormatCurrentPath(), e.what()
				);
			} catch (...) {
				instance->Destroy();
				return state.ReturnError("Unknown error setting property '{}' in {}", key, state.FormatCurrentPath());
			}
		}
		try {
			std::map<std::string, WireValue> decodedAttributes;
			for (const auto &[key, encoded] : attributes.items()) {
				auto value = DecodeWireValue(encoded);
				if (!value) throw std::invalid_argument("Malformed attribute WireValue");
				ValidateAttributeName(key);
				(void)ValidateAttributeValue(*value);
				decodedAttributes.emplace(key, std::move(*value));
			}
			(void)ValidateAttributeCollection(decodedAttributes);
			for (const auto &[key, value] : decodedAttributes) {
				if (instance->ApplyAttributeMutation(key, value, ScriptSecurityContext::CoreTrusted()) !=
					MutationStatus::Success)
					throw std::runtime_error("Serialized attribute mutation rejected");
			}
		} catch (const std::exception &error) {
			instance->Destroy();
			return state.ReturnError("Failed to deserialize attributes in {}: {}", state.FormatCurrentPath(), error.what());
		}
		try {
			if (customProperties.size() > MaximumCustomClassInheritanceDepth)
				throw std::invalid_argument("Instance exceeds its custom property declaring-class state limit");
			std::optional<SchemaId> previousDeclaringClassId;
			for (const auto &encodedState : customProperties) {
				if (!encodedState.is_object() || !encodedState.contains("DeclaringClassSchemaId") ||
					!encodedState["DeclaringClassSchemaId"].is_string() ||
					!encodedState.contains("DefinitionVersion") || !encodedState["DefinitionVersion"].is_number_unsigned() ||
					!encodedState.contains("Properties") || !encodedState["Properties"].is_object())
					throw std::invalid_argument("Malformed custom property state");
				auto declaringId = SchemaId::Parse(encodedState["DeclaringClassSchemaId"].get<std::string>());
				if (!declaringId || (previousDeclaringClassId && !(*previousDeclaringClassId < *declaringId)))
					throw std::invalid_argument("Invalid, duplicate, or unordered custom declaring class SchemaId");
				previousDeclaringClassId = *declaringId;
				auto decodedVersion = DecodeWireUnsigned32(encodedState["DefinitionVersion"]);
				if (!decodedVersion || *decodedVersion == 0)
					throw std::invalid_argument("Invalid custom property declaring class version");
				const auto version = *decodedVersion;
				auto *declaringClass = GetActiveRuntimeSchemaRegistry().FindClassById(*declaringId);
				if (!declaringClass || declaringClass->ConstructionKind != SchemaClassConstructionKind::CustomData ||
					declaringClass->DefinitionVersion != version ||
					!GetActiveRuntimeSchemaRegistry().IsClassDerivedFrom(definition->Id, *declaringId))
					throw std::invalid_argument("Missing or incompatible custom property declaring class version");
				const auto &encodedProperties = encodedState["Properties"];
				if (encodedProperties.empty() || encodedProperties.size() > MaximumCustomClassProperties)
					throw std::invalid_argument("Custom property state is empty or oversized");
				for (const auto &[propertyName, encodedValue] : encodedProperties.items()) {
					auto *property = GetActiveRuntimeSchemaRegistry().FindCustomClassProperty(*declaringId, propertyName);
					auto value = DecodeWireValue(encodedValue);
					if (!property || !value) throw std::invalid_argument("Unknown or malformed custom property");
					(void)ValidateSchemaExtensionPropertyValue(property->Type, *value);
					if (*value == property->DefaultValue)
						throw std::invalid_argument("Persisted custom property state redundantly stores its default");
					if (instance->ApplyCustomClassPropertyMutation(
						*declaringId, version, propertyName, std::move(*value), ScriptSecurityContext::CoreTrusted()
					) != MutationStatus::Success)
						throw std::runtime_error("Serialized custom property mutation rejected");
				}
			}
		} catch (const std::exception &error) {
			instance->Destroy();
			return state.ReturnError(
				"Failed to deserialize custom property state in {}: {}", state.FormatCurrentPath(), error.what()
			);
		}
		try {
			if (extensions.size() > MaximumCustomExtensionDefinitions)
				throw std::invalid_argument("Instance exceeds its extension definition state limit");
			std::optional<SchemaId> previousExtensionId;
			for (const auto &encodedExtension : extensions) {
				if (!encodedExtension.is_object() || !encodedExtension.contains("ExtensionSchemaId") ||
					!encodedExtension["ExtensionSchemaId"].is_string() ||
					!encodedExtension.contains("DefinitionVersion") ||
					!encodedExtension["DefinitionVersion"].is_number_unsigned() ||
					!encodedExtension.contains("Properties") || !encodedExtension["Properties"].is_object())
					throw std::invalid_argument("Malformed extension state");
				const auto encodedId = encodedExtension["ExtensionSchemaId"].get<std::string>();
				auto extensionId = SchemaId::Parse(encodedId);
				if (!extensionId || extensionId->ToString() != encodedId ||
					(previousExtensionId && !(*previousExtensionId < *extensionId)))
					throw std::invalid_argument("Invalid, duplicate, or unordered extension SchemaId");
				previousExtensionId = *extensionId;
				auto decodedVersion = DecodeWireUnsigned32(encodedExtension["DefinitionVersion"]);
				if (!decodedVersion || *decodedVersion == 0)
					throw std::invalid_argument("Invalid extension definition version");
				const auto version = *decodedVersion;
				auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(*extensionId);
				if (!extension || extension->DefinitionVersion != version)
					throw std::invalid_argument("Missing or incompatible extension definition version");
				const auto &encodedProperties = encodedExtension["Properties"];
				if (encodedProperties.empty() || encodedProperties.size() > MaximumExtensionProperties)
					throw std::invalid_argument("Extension property state is empty or oversized");
				for (const auto &[propertyName, encodedValue] : encodedProperties.items()) {
					auto *property = GetActiveRuntimeSchemaRegistry().FindExtensionProperty(*extensionId, propertyName);
					auto value = DecodeWireValue(encodedValue);
					if (!property || !value) throw std::invalid_argument("Unknown or malformed extension property");
					(void)ValidateSchemaExtensionPropertyValue(property->Type, *value);
					if (*value == property->DefaultValue)
						throw std::invalid_argument("Persisted extension state redundantly stores its default");
					if (instance->ApplyExtensionPropertyMutation(
						*extensionId, version, propertyName, std::move(*value), ScriptSecurityContext::CoreTrusted()
					) != MutationStatus::Success)
						throw std::runtime_error("Serialized extension property mutation rejected");
				}
			}
		} catch (const std::exception &error) {
			instance->Destroy();
			return state.ReturnError(
				"Failed to deserialize extension state in {}: {}", state.FormatCurrentPath(), error.what()
			);
		}
		try {
			std::set<std::string> decodedTags;
			for (const auto &encoded : tags) {
				if (!encoded.is_string()) throw std::invalid_argument("Tag is not a string");
				auto tag = encoded.get<std::string>();
				ValidateTagName(tag);
				if (!decodedTags.insert(std::move(tag)).second) throw std::invalid_argument("Duplicate tag membership");
				if (decodedTags.size() > MaximumTagsPerInstance) throw std::invalid_argument("Instance exceeds its tag count limit");
			}
			state.PendingTags.emplace(instance, std::vector<std::string>(decodedTags.begin(), decodedTags.end()));
		} catch (const std::exception &error) {
			instance->Destroy();
			return state.ReturnError("Failed to deserialize tags in {}: {}", state.FormatCurrentPath(), error.what());
		}

		for (auto &child : children) {
			auto maybeChild = TryDeserializeInstance(
				child, state, requireAttributes, requireTags, requireExtensions, requireClassIdentity, requireCustomProperties
			);
			if (maybeChild.has_value()) {
				maybeChild.value()->SetParent(instance);
			} else {
				state.PushError("Failed to deserialize child in {}", state.FormatCurrentPath());
				instance->Destroy();
				return std::nullopt;
			}
		}

		state.CurrentPath.pop_back();

		return instance;
	}

	DeserializationState Deserialize(InstanceFormat format, std::istream &input) {
		DeserializationState state;

		if (!input.good()) {
			state.PushError("Bad stream from instance contents");
			return state;
		}

		switch (format) {
		case InstanceFormat::Json: {
			json contents;
			try {
				auto Encoded = ReadBoundedDocument(input);
				if (!Encoded || Encoded->empty()) {
					state.PushError("Instance document byte length is invalid");
					return state;
				}
				ValidateProtocolJsonDocument(*Encoded);
				contents = json::parse(*Encoded);
				ValidateProtocolJsonTree(contents);
			} catch (const std::exception &e) {
				state.PushError("Failed to parse JSON: {}", e.what());
				return state;
			}

			if (!contents.is_object()) {
				state.PushError("Expected a JSON object");
				return state;
			}

			if (!contents.contains("Version") || !contents["Version"].is_number_integer() ||
				(contents["Version"] != 0 && contents["Version"] != 1 && contents["Version"] != 2 &&
					contents["Version"] != 3 && contents["Version"] != 4)) {
				state.PushError("Unsupported instance format version");
				return state;
			}

			const auto version = contents["Version"].get<int>();
			std::optional<std::shared_ptr<Instance>> maybeInstance;
			try {
				maybeInstance = TryDeserializeInstance(
					contents, state, version >= 1, version >= 2, version >= 3, version >= 4, version >= 4
				);
			} catch (const std::exception &Error) {
				state.PushError("Failed to validate instance document: {}", Error.what());
				return state;
			}
			if (maybeInstance.has_value()) {
				try {
					for (const auto &[instance, tags] : state.PendingTags) {
						auto dataModel = instance->GetDataModel();
						if (!dataModel && !tags.empty()) throw std::runtime_error("Tagged Instance is not owned by a DataModel");
						if (dataModel) for (const auto &tag : tags)
							(void)dataModel->Tags.Add(dataModel->GetObjectId(), instance->GetObjectId(), tag, ScriptSecurityContext::CoreTrusted());
					}
					state.Ok = true;
					state.Instance = maybeInstance.value();
				} catch (const std::exception &error) {
					maybeInstance.value()->Destroy();
					state.PushError("Failed to rebuild tag index: {}", error.what());
				}
			}

			break;
		}

		default:
			state.PushError("Binary instance format is not yet implemented");
			break;
		}

		return state;
	};
}
