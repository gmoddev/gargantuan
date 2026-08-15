#include "gargantuan/runtime/WireCodec.hpp"

#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/reflection/RuntimeSchema.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace gargantuan {
	namespace {
		std::optional<std::int32_t> DecodeSignedInt32(const WireJson &Value) {
			if (Value.is_number_unsigned()) {
				const auto Raw = Value.get<std::uint64_t>();
				if (Raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
					return static_cast<std::int32_t>(Raw);
				return std::nullopt;
			}
			if (!Value.is_number_integer()) return std::nullopt;
			const auto Raw = Value.get<std::int64_t>();
			if (Raw < std::numeric_limits<std::int32_t>::min() || Raw > std::numeric_limits<std::int32_t>::max())
				return std::nullopt;
			return static_cast<std::int32_t>(Raw);
		}
	}

	std::optional<std::uint32_t> DecodeWireUnsigned32(const WireJson &value) {
		if (!value.is_number_unsigned()) return std::nullopt;
		const auto raw = value.get<std::uint64_t>();
		if (raw > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
		return static_cast<std::uint32_t>(raw);
	}

	WireJson EncodeWireObjectId(WireObjectId id) {
		return WireJson{{"Slot", id.Slot}, {"Generation", id.Generation}};
	}

	std::optional<WireObjectId> DecodeWireObjectId(const WireJson &value) {
		if (!value.is_object() || !value.contains("Slot") || !value.contains("Generation")) return std::nullopt;
		auto slot = DecodeWireUnsigned32(value["Slot"]);
		auto generation = DecodeWireUnsigned32(value["Generation"]);
		if (!slot || !generation) return std::nullopt;
		WireObjectId result{*slot, *generation};
		return result.IsValid() ? std::optional(result) : std::nullopt;
	}

	std::optional<WireValue> EncodeNativeWireValue(const std::any &value) {
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
				typed->Position.x, typed->Position.y, typed->Position.z,
				typed->Rotation[0][0], typed->Rotation[0][1], typed->Rotation[0][2],
				typed->Rotation[1][0], typed->Rotation[1][1], typed->Rotation[1][2],
				typed->Rotation[2][0], typed->Rotation[2][1], typed->Rotation[2][2],
			}};
		}
		if (auto *typed = std::any_cast<EnumItem>(&value)) {
			if (!typed->EnumType) return std::nullopt;
			return WireEnumItem{std::string(typed->EnumType->Name), std::string(typed->Name)};
		}
		if (auto *typed = std::any_cast<WireSchemaEnumValue>(&value)) return *typed;
		return std::nullopt;
	}

	std::optional<std::any> DecodeNativeWireValue(const WireValue &value) {
		return std::visit(
			[](const auto &typed) -> std::optional<std::any> {
				using Value = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<Value, std::monostate> ||
					std::is_same_v<Value, WireObjectReference> || std::is_same_v<Value, WireEnumItem> ||
					std::is_same_v<Value, WireSchemaEnumValue>) {
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
					return UDim2(typed.X.Scale, typed.X.Offset, typed.Y.Scale, typed.Y.Offset);
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
				} else {
					return std::any(typed);
				}
			},
			value
		);
	}

	WireJson EncodeWireValue(const WireValue &value) {
		return std::visit(
			[](const auto &typed) -> WireJson {
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
				if constexpr (std::is_same_v<Value, WireEnumItem>)
					return {{"Type", "EnumItem"}, {"Enum", typed.EnumType}, {"Value", typed.Item}};
				if constexpr (std::is_same_v<Value, WireSchemaEnumValue>) return {
					{"Type", "SchemaEnum"},
					{"SchemaId", typed.EnumSchemaId.ToString()},
					{"DefinitionVersion", typed.DefinitionVersion},
					{"Value", typed.ItemValue},
				};
				if constexpr (std::is_same_v<Value, WireObjectReference>)
					return {{"Type", "ObjectReference"}, {"Value", EncodeWireObjectId(typed.Object)}};
			},
			value
		);
	}

	std::optional<WireValue> DecodeWireValue(const WireJson &encoded) {
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
		if (type == "SchemaEnum" && encoded.size() == 4 && encoded.contains("SchemaId") &&
			encoded["SchemaId"].is_string() && encoded.contains("DefinitionVersion") &&
			encoded["DefinitionVersion"].is_number_unsigned() && value.is_number_integer()) {
			auto id = SchemaId::Parse(encoded["SchemaId"].get<std::string>());
			const auto version = encoded["DefinitionVersion"].get<std::uint64_t>();
			const auto item = DecodeSignedInt32(value);
			if (id && version > 0 && version <= std::numeric_limits<std::uint32_t>::max() &&
				item)
				return WireSchemaEnumValue{*id, static_cast<std::uint32_t>(version), *item};
		}
		if (type == "ObjectReference") {
			auto id = DecodeWireObjectId(value);
			if (id) return WireObjectReference{*id};
		}
		return std::nullopt;
	}

	const SchemaEnumItem &ValidateSchemaEnumValue(
		const WireSchemaEnumValue &value,
		const RuntimeSchemaRegistry &registry
	) {
		if (!value.EnumSchemaId.IsValid() || value.DefinitionVersion == 0)
			throw std::invalid_argument("Schema enum value identity or version is invalid");
		const auto *definition = registry.FindDefinitionById(value.EnumSchemaId);
		if (!definition) throw std::invalid_argument("Schema enum definition is missing");
		const auto *enumDefinition = std::get_if<SchemaEnumDefinition>(definition);
		if (!enumDefinition) throw std::invalid_argument("Schema enum value resolves to the wrong definition kind");
		if (enumDefinition->DefinitionVersion != value.DefinitionVersion)
			throw std::invalid_argument("Schema enum definition version is incompatible");
		auto item = std::find_if(enumDefinition->Items.begin(), enumDefinition->Items.end(), [&](const auto &candidate) {
			return candidate.Value == value.ItemValue;
		});
		if (item == enumDefinition->Items.end()) throw std::invalid_argument("Schema enum item value is unknown");
		return *item;
	}

	std::string FormatSchemaEnumValue(
		const WireSchemaEnumValue &value,
		const RuntimeSchemaRegistry &registry
	) {
		const auto &item = ValidateSchemaEnumValue(value, registry);
		const auto *definition = registry.FindEnumById(value.EnumSchemaId);
		return definition->CanonicalName + "." + item.Name;
	}
}
