#include "gargantuan/runtime/WireCodec.hpp"

#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace gargantuan {
	namespace {
		bool HasOnlyFields(const WireJson &Value, std::initializer_list<std::string_view> Allowed) {
			if (!Value.is_object()) return false;
			for (const auto &[Name, Child] : Value.items()) {
				(void)Child;
				if (std::find(Allowed.begin(), Allowed.end(), Name) == Allowed.end()) return false;
			}
			return true;
		}

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

		std::optional<double> DecodeFiniteDouble(const WireJson &Value) {
			if (!Value.is_number()) return std::nullopt;
			const auto Result = Value.get<double>();
			return std::isfinite(Result) ? std::optional(Result) : std::nullopt;
		}

		std::optional<float> DecodeFiniteFloat(const WireJson &Value) {
			auto Decoded = DecodeFiniteDouble(Value);
			if (!Decoded || *Decoded < -std::numeric_limits<float>::max() ||
				*Decoded > std::numeric_limits<float>::max()) return std::nullopt;
			const auto Result = static_cast<float>(*Decoded);
			return std::isfinite(Result) ? std::optional(Result) : std::nullopt;
		}

		bool ValidateWireText(std::string_view Value, std::size_t MaximumBytes = MaximumProtocolStringBytes) {
			try {
				ValidateProtocolString(Value, MaximumBytes, "Wire string");
				return true;
			} catch (const std::invalid_argument &) {
				return false;
			}
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
		if (!HasOnlyFields(value, {"Slot", "Generation"}) || value.size() != 2 ||
			!value.contains("Slot") || !value.contains("Generation")) return std::nullopt;
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
		ValidateProtocolWireValue(value);
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
		const auto &type = encoded["Type"].get_ref<const std::string &>();
		if (!ValidateWireText(type, 32)) return std::nullopt;
		if (type == "Null") return HasOnlyFields(encoded, {"Type"}) && encoded.size() == 1
			? std::optional<WireValue>(std::monostate{}) : std::nullopt;
		if (!encoded.contains("Value")) return std::nullopt;
		const auto &value = encoded["Value"];
		if (type == "Bool" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 && value.is_boolean())
			return value.get<bool>();
		if (type == "Int" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2)
			if (auto Result = DecodeSignedInt32(value)) return static_cast<int>(*Result);
		if (type == "Double" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2)
			if (auto Result = DecodeFiniteDouble(value)) return *Result;
		if (type == "Float" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2)
			if (auto Result = DecodeFiniteFloat(value)) return WireFloat{*Result};
		if (type == "String" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 && value.is_string()) {
			const auto &Text = value.get_ref<const std::string &>();
			if (ValidateWireText(Text)) return Text;
		}
		if (type == "Vector2" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 &&
			value.is_array() && value.size() == 2) {
			auto X = DecodeFiniteFloat(value[0]);
			auto Y = DecodeFiniteFloat(value[1]);
			if (X && Y) return WireVector2{*X, *Y};
		}
		if (type == "Vector3" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 &&
			value.is_array() && value.size() == 3) {
			auto X = DecodeFiniteFloat(value[0]);
			auto Y = DecodeFiniteFloat(value[1]);
			auto Z = DecodeFiniteFloat(value[2]);
			if (X && Y && Z) return WireVector3{*X, *Y, *Z};
		}
		if (type == "Color3" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 &&
			value.is_array() && value.size() == 3) {
			auto R = DecodeFiniteFloat(value[0]);
			auto G = DecodeFiniteFloat(value[1]);
			auto B = DecodeFiniteFloat(value[2]);
			if (R && G && B) return WireColor3{*R, *G, *B};
		}
		if (type == "UDim" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 &&
			value.is_array() && value.size() == 2) {
			auto Scale = DecodeFiniteFloat(value[0]);
			auto Offset = DecodeSignedInt32(value[1]);
			if (Scale && Offset) return WireUDim{*Scale, static_cast<int>(*Offset)};
		}
		if (type == "UDim2" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2 &&
			value.is_array() && value.size() == 4) {
			auto XScale = DecodeFiniteFloat(value[0]);
			auto XOffset = DecodeSignedInt32(value[1]);
			auto YScale = DecodeFiniteFloat(value[2]);
			auto YOffset = DecodeSignedInt32(value[3]);
			if (XScale && XOffset && YScale && YOffset)
				return WireUDim2{{*XScale, static_cast<int>(*XOffset)}, {*YScale, static_cast<int>(*YOffset)}};
		}
		if (type == "CFrame" && value.is_array() && value.size() == 12) {
			WireCFrame result;
			if (!HasOnlyFields(encoded, {"Type", "Value"}) || encoded.size() != 2) return std::nullopt;
			for (std::size_t i = 0; i < result.Components.size(); ++i) {
				auto Component = DecodeFiniteFloat(value[i]);
				if (!Component) return std::nullopt;
				result.Components[i] = *Component;
			}
			return result;
		}
		if (type == "EnumItem" && HasOnlyFields(encoded, {"Type", "Enum", "Value"}) && encoded.size() == 3 &&
			encoded.contains("Enum") && encoded["Enum"].is_string() && value.is_string()) {
			const auto &EnumType = encoded["Enum"].get_ref<const std::string &>();
			const auto &Item = value.get_ref<const std::string &>();
			if (ValidateWireText(EnumType, MaximumProtocolIdentifierBytes) &&
				ValidateWireText(Item, MaximumProtocolIdentifierBytes)) return WireEnumItem{EnumType, Item};
		}
		if (type == "SchemaEnum" && encoded.size() == 4 && encoded.contains("SchemaId") &&
			HasOnlyFields(encoded, {"Type", "SchemaId", "DefinitionVersion", "Value"}) &&
			encoded["SchemaId"].is_string() && encoded.contains("DefinitionVersion") &&
			encoded["DefinitionVersion"].is_number_unsigned() && value.is_number_integer()) {
			auto id = SchemaId::Parse(encoded["SchemaId"].get<std::string>());
			const auto version = encoded["DefinitionVersion"].get<std::uint64_t>();
			const auto item = DecodeSignedInt32(value);
			if (id && version > 0 && version <= std::numeric_limits<std::uint32_t>::max() &&
				item)
				return WireSchemaEnumValue{*id, static_cast<std::uint32_t>(version), *item};
		}
		if (type == "ObjectReference" && HasOnlyFields(encoded, {"Type", "Value"}) && encoded.size() == 2) {
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
