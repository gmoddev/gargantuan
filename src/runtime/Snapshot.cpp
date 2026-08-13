#include "gargantuan/runtime/Snapshot.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <any>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace gargantuan {
	namespace {
		using Json = nlohmann::ordered_json;

		Json EncodeId(WireObjectId id) {
			return Json{{"Slot", id.Slot}, {"Generation", id.Generation}};
		}

		std::optional<WireObjectId> DecodeId(const Json &value) {
			if (!value.is_object() || !value.contains("Slot") || !value.contains("Generation") ||
				!value["Slot"].is_number_unsigned() || !value["Generation"].is_number_unsigned())
				return std::nullopt;
			WireObjectId result{value["Slot"].get<std::uint32_t>(), value["Generation"].get<std::uint32_t>()};
			return result.IsValid() ? std::optional(result) : std::nullopt;
		}

		std::optional<WireValue> EncodeNativeValue(const std::any &value) {
			if (auto *typed = std::any_cast<bool>(&value)) return *typed;
			if (auto *typed = std::any_cast<int>(&value)) return *typed;
			if (auto *typed = std::any_cast<double>(&value)) return *typed;
			if (auto *typed = std::any_cast<float>(&value)) return WireFloat{*typed};
			if (auto *typed = std::any_cast<std::string>(&value)) return *typed;
			if (auto *typed = std::any_cast<std::string_view>(&value)) return std::string(*typed);
			if (auto *typed = std::any_cast<Vector2>(&value)) return WireVector2{typed->GetX(), typed->GetY()};
			if (auto *typed = std::any_cast<glm::vec3>(&value)) return WireVector3{typed->x, typed->y, typed->z};
			if (auto *typed = std::any_cast<Color3>(&value)) return WireColor3{typed->R, typed->G, typed->B};
			if (auto *typed = std::any_cast<UDim>(&value)) return WireUDim{typed->Scale, typed->Offset};
			if (auto *typed = std::any_cast<UDim2>(&value))
				return WireUDim2{{typed->X.Scale, typed->X.Offset}, {typed->Y.Scale, typed->Y.Offset}};
			if (auto *typed = std::any_cast<CFrame>(&value)) {
				return WireCFrame{{
					typed->Position.x,
					typed->Position.y,
					typed->Position.z,
					typed->Rotation[0][0],
					typed->Rotation[0][1],
					typed->Rotation[0][2],
					typed->Rotation[1][0],
					typed->Rotation[1][1],
					typed->Rotation[1][2],
					typed->Rotation[2][0],
					typed->Rotation[2][1],
					typed->Rotation[2][2],
				}};
			}
			if (auto *typed = std::any_cast<EnumItem>(&value)) {
				if (!typed->EnumType) return std::nullopt;
				return WireEnumItem{std::string(typed->EnumType->Name), std::string(typed->Name)};
			}
			return std::nullopt;
		}

		std::optional<std::any> DecodeNativeValue(const WireValue &value) {
			return std::visit(
				[](const auto &typed) -> std::optional<std::any> {
					using Value = std::decay_t<decltype(typed)>;
					if constexpr (std::is_same_v<Value, std::monostate> ||
						std::is_same_v<Value, WireObjectReference>) {
						return std::nullopt;
					} else if constexpr (std::is_same_v<Value, WireFloat>) {
						return typed.Value;
					} else if constexpr (std::is_same_v<Value, WireVector2>) {
						return Vector2(typed.X, typed.Y);
					} else if constexpr (std::is_same_v<Value, WireVector3>) {
						return glm::vec3(typed.X, typed.Y, typed.Z);
					} else if constexpr (std::is_same_v<Value, WireColor3>) {
						return Color3(typed.R, typed.G, typed.B);
					} else if constexpr (std::is_same_v<Value, WireUDim>) {
						return UDim(typed.Scale, typed.Offset);
					} else if constexpr (std::is_same_v<Value, WireUDim2>) {
						return UDim2(
							typed.X.Scale, typed.X.Offset, typed.Y.Scale, typed.Y.Offset
						);
					} else if constexpr (std::is_same_v<Value, WireCFrame>) {
						const auto &c = typed.Components;
						return CFrame(
							glm::vec3(c[0], c[1], c[2]),
							glm::mat3(
								glm::vec3(c[3], c[6], c[9]),
								glm::vec3(c[4], c[7], c[10]),
								glm::vec3(c[5], c[8], c[11])
							)
						);
					} else if constexpr (std::is_same_v<Value, WireEnumItem>) {
						return std::nullopt;
					} else {
						return std::any(typed);
					}
				},
				value
			);
		}

		Json EncodeValue(const WireValue &value) {
			return std::visit(
				[](const auto &typed) -> Json {
					using Value = std::decay_t<decltype(typed)>;
					if constexpr (std::is_same_v<Value, std::monostate>) return {{"Type", "Null"}};
					if constexpr (std::is_same_v<Value, bool>) return {{"Type", "Bool"}, {"Value", typed}};
					if constexpr (std::is_same_v<Value, int>) return {{"Type", "Int"}, {"Value", typed}};
					if constexpr (std::is_same_v<Value, double>) return {{"Type", "Double"}, {"Value", typed}};
					if constexpr (std::is_same_v<Value, WireFloat>) return {{"Type", "Float"}, {"Value", typed.Value}};
					if constexpr (std::is_same_v<Value, std::string>) return {{"Type", "String"}, {"Value", typed}};
					if constexpr (std::is_same_v<Value, WireVector2>) return {{"Type", "Vector2"}, {"Value", {typed.X, typed.Y}}};
					if constexpr (std::is_same_v<Value, WireVector3>) return {{"Type", "Vector3"}, {"Value", {typed.X, typed.Y, typed.Z}}};
					if constexpr (std::is_same_v<Value, WireColor3>) return {{"Type", "Color3"}, {"Value", {typed.R, typed.G, typed.B}}};
					if constexpr (std::is_same_v<Value, WireUDim>) return {{"Type", "UDim"}, {"Value", {typed.Scale, typed.Offset}}};
					if constexpr (std::is_same_v<Value, WireUDim2>)
						return {{"Type", "UDim2"}, {"Value", {typed.X.Scale, typed.X.Offset, typed.Y.Scale, typed.Y.Offset}}};
					if constexpr (std::is_same_v<Value, WireCFrame>) return {{"Type", "CFrame"}, {"Value", typed.Components}};
					if constexpr (std::is_same_v<Value, WireEnumItem>) return {{"Type", "EnumItem"}, {"Enum", typed.EnumType}, {"Value", typed.Item}};
					if constexpr (std::is_same_v<Value, WireObjectReference>) return {{"Type", "ObjectReference"}, {"Value", EncodeId(typed.Object)}};
				},
				value
			);
		}

		std::optional<WireValue> DecodeValue(const Json &encoded) {
			if (!encoded.is_object() || !encoded.contains("Type") || !encoded["Type"].is_string()) return std::nullopt;
			const auto type = encoded["Type"].get<std::string>();
			if (type == "Null") return std::monostate{};
			if (!encoded.contains("Value")) return std::nullopt;
			const auto &value = encoded["Value"];
			if (type == "Bool" && value.is_boolean()) return value.get<bool>();
			if (type == "Int" && value.is_number_integer()) return value.get<int>();
			if (type == "Double" && value.is_number()) return value.get<double>();
			if (type == "Float" && value.is_number()) return WireFloat{value.get<float>()};
			if (type == "String" && value.is_string()) return value.get<std::string>();
			if (type == "Vector2" && value.is_array() && value.size() == 2)
				return WireVector2{value[0].get<float>(), value[1].get<float>()};
			if (type == "Vector3" && value.is_array() && value.size() == 3)
				return WireVector3{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			if (type == "Color3" && value.is_array() && value.size() == 3)
				return WireColor3{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			if (type == "UDim" && value.is_array() && value.size() == 2)
				return WireUDim{value[0].get<float>(), value[1].get<int>()};
			if (type == "UDim2" && value.is_array() && value.size() == 4)
				return WireUDim2{{value[0].get<float>(), value[1].get<int>()}, {value[2].get<float>(), value[3].get<int>()}};
			if (type == "CFrame" && value.is_array() && value.size() == 12) {
				WireCFrame result;
				for (std::size_t i = 0; i < result.Components.size(); ++i) result.Components[i] = value[i].get<float>();
				return result;
			}
			if (type == "EnumItem" && encoded.contains("Enum") && encoded["Enum"].is_string() && value.is_string())
				return WireEnumItem{encoded["Enum"].get<std::string>(), value.get<std::string>()};
			if (type == "ObjectReference") {
				auto id = DecodeId(value);
				if (id) return WireObjectReference{*id};
			}
			return std::nullopt;
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
				if (!property->Serializable || !property->Read || !property->Write) continue;
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
			snapshot.Objects.push_back(std::move(object));
		}
		snapshot.Cursor = ChangeJournal::Get().CreateCursor();
		return snapshot;
	}

	std::string SerializeSnapshot(const Snapshot &snapshot) {
		Json document;
		document["Version"] = snapshot.Version;
		document["Cursor"] = snapshot.Cursor.NextSequence;
		document["Objects"] = Json::array();
		for (const auto &object : snapshot.Objects) {
			Json encoded;
			encoded["Id"] = EncodeId(object.Id);
			encoded["ClassName"] = object.ClassName;
			encoded["Name"] = object.Name;
			encoded["Parent"] = object.Parent ? EncodeId(*object.Parent) : Json(nullptr);
			encoded["Properties"] = Json::object();
			for (const auto &[name, value] : object.Properties) encoded["Properties"][name] = EncodeValue(value);
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
				!document.contains("Cursor") || !document["Cursor"].is_number_unsigned() ||
				!document.contains("Objects") || !document["Objects"].is_array()) {
				result.Errors.push_back("Invalid or unsupported snapshot envelope");
				return result;
			}

			Snapshot snapshot;
			snapshot.Cursor.NextSequence = document["Cursor"].get<std::uint64_t>();
			if (snapshot.Cursor.NextSequence == 0) {
				result.Errors.push_back("Invalid zero snapshot cursor");
				return result;
			}

			for (const auto &encoded : document["Objects"]) {
				if (!encoded.is_object() || !encoded.contains("Id") || !encoded.contains("ClassName") ||
					!encoded["ClassName"].is_string() || !encoded.contains("Name") || !encoded["Name"].is_string() ||
					!encoded.contains("Parent") || !encoded.contains("Properties") ||
					!encoded["Properties"].is_object()) {
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
		if (snapshot.Version != SnapshotFormatVersion) {
			result.Errors.push_back("Unsupported snapshot version");
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

			for (const auto &object : snapshot.Objects) {
				auto instance = result.Resolve(object.Id);
				for (const auto &[name, value] : object.Properties) {
					auto *property = instance->FindProperty(name);
					if (!property || !property->Serializable || !property->Write)
						throw std::runtime_error("Snapshot contains an unknown or non-serializable property");
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
			}
		} catch (const std::exception &error) {
			result.Errors.push_back(error.what());
		}
		return result;
	}
}
