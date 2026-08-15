#include "gargantuan/runtime/ProtocolInput.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace gargantuan {
	namespace {
		void ValidateJsonNode(
			const nlohmann::ordered_json &Value,
			std::size_t Depth,
			std::size_t &Nodes
		) {
			if (Depth > MaximumProtocolJsonDepth)
				throw std::invalid_argument("Protocol JSON exceeds its nesting-depth limit");
			if (Nodes == MaximumProtocolJsonNodes)
				throw std::invalid_argument("Protocol JSON exceeds its node-count limit");
			++Nodes;

			if (Value.is_string()) {
				ValidateProtocolString(
					Value.get_ref<const std::string &>(), MaximumProtocolStringBytes, "Protocol string"
				);
				return;
			}
			if (Value.is_object()) {
				for (const auto &[Name, Child] : Value.items()) {
					ValidateProtocolString(Name, MaximumProtocolIdentifierBytes, "Protocol field name");
					ValidateJsonNode(Child, Depth + 1, Nodes);
				}
				return;
			}
			if (Value.is_array())
				for (const auto &Child : Value) ValidateJsonNode(Child, Depth + 1, Nodes);
		}
	}

	bool IsValidProtocolUtf8(std::string_view Value) {
		for (std::size_t Index = 0; Index < Value.size();) {
			const auto First = static_cast<unsigned char>(Value[Index]);
			std::size_t Count = 0;
			std::uint32_t CodePoint = 0;
			if (First <= 0x7f) { Count = 1; CodePoint = First; }
			else if ((First & 0xe0) == 0xc0) { Count = 2; CodePoint = First & 0x1f; }
			else if ((First & 0xf0) == 0xe0) { Count = 3; CodePoint = First & 0x0f; }
			else if ((First & 0xf8) == 0xf0) { Count = 4; CodePoint = First & 0x07; }
			else return false;
			if (Index + Count > Value.size()) return false;
			for (std::size_t Offset = 1; Offset < Count; ++Offset) {
				const auto Continuation = static_cast<unsigned char>(Value[Index + Offset]);
				if ((Continuation & 0xc0) != 0x80) return false;
				CodePoint = (CodePoint << 6) | (Continuation & 0x3f);
			}
			if ((Count == 2 && CodePoint < 0x80) || (Count == 3 && CodePoint < 0x800) ||
				(Count == 4 && CodePoint < 0x10000) || CodePoint > 0x10ffff ||
				(CodePoint >= 0xd800 && CodePoint <= 0xdfff)) return false;
			Index += Count;
		}
		return true;
	}

	void ValidateProtocolString(
		std::string_view Value,
		std::size_t MaximumBytes,
		std::string_view Description
	) {
		if (Value.size() > MaximumBytes)
			throw std::invalid_argument(std::string(Description) + " exceeds its byte limit");
		if (Value.find('\0') != std::string_view::npos)
			throw std::invalid_argument(std::string(Description) + " contains an embedded null");
		if (!IsValidProtocolUtf8(Value))
			throw std::invalid_argument(std::string(Description) + " is not valid UTF-8");
	}

	void ValidateProtocolJsonDocument(std::string_view Value, std::size_t MaximumBytes) {
		if (Value.empty() || Value.size() > MaximumBytes)
			throw std::invalid_argument("Protocol document byte length is invalid");
		std::size_t Depth = 0;
		bool InString = false;
		bool Escaped = false;
		for (const char Character : Value) {
			if (InString) {
				if (Escaped) Escaped = false;
				else if (Character == '\\') Escaped = true;
				else if (Character == '"') InString = false;
				continue;
			}
			if (Character == '"') InString = true;
			else if (Character == '{' || Character == '[') {
				if (++Depth > MaximumProtocolJsonDepth)
					throw std::invalid_argument("Protocol JSON exceeds its nesting-depth limit");
			} else if ((Character == '}' || Character == ']') && Depth != 0) --Depth;
		}
	}

	void ValidateProtocolJsonTree(const nlohmann::ordered_json &Value) {
		std::size_t Nodes = 0;
		ValidateJsonNode(Value, 1, Nodes);
	}

	void ValidateProtocolWireValue(const WireValue &Value) {
		std::visit(
			[](const auto &Typed) {
				using Type = std::decay_t<decltype(Typed)>;
				if constexpr (std::is_same_v<Type, double>) {
					if (!std::isfinite(Typed)) throw std::invalid_argument("Wire double must be finite");
				} else if constexpr (std::is_same_v<Type, WireFloat>) {
					if (!std::isfinite(Typed.Value)) throw std::invalid_argument("Wire float must be finite");
				} else if constexpr (std::is_same_v<Type, std::string>) {
					ValidateProtocolString(Typed, MaximumProtocolStringBytes, "Wire string");
				} else if constexpr (std::is_same_v<Type, WireVector2>) {
					if (!std::isfinite(Typed.X) || !std::isfinite(Typed.Y))
						throw std::invalid_argument("Wire Vector2 must be finite");
				} else if constexpr (std::is_same_v<Type, WireVector3>) {
					if (!std::isfinite(Typed.X) || !std::isfinite(Typed.Y) || !std::isfinite(Typed.Z))
						throw std::invalid_argument("Wire Vector3 must be finite");
				} else if constexpr (std::is_same_v<Type, WireColor3>) {
					if (!std::isfinite(Typed.R) || !std::isfinite(Typed.G) || !std::isfinite(Typed.B))
						throw std::invalid_argument("Wire Color3 must be finite");
				} else if constexpr (std::is_same_v<Type, WireUDim>) {
					if (!std::isfinite(Typed.Scale)) throw std::invalid_argument("Wire UDim must be finite");
				} else if constexpr (std::is_same_v<Type, WireUDim2>) {
					if (!std::isfinite(Typed.X.Scale) || !std::isfinite(Typed.Y.Scale))
						throw std::invalid_argument("Wire UDim2 must be finite");
				} else if constexpr (std::is_same_v<Type, WireCFrame>) {
					if (!std::ranges::all_of(Typed.Components, [](float Component) { return std::isfinite(Component); }))
						throw std::invalid_argument("Wire CFrame must be finite");
				} else if constexpr (std::is_same_v<Type, WireEnumItem>) {
					ValidateProtocolString(Typed.EnumType, MaximumProtocolIdentifierBytes, "Wire enum type");
					ValidateProtocolString(Typed.Item, MaximumProtocolIdentifierBytes, "Wire enum item");
				} else if constexpr (std::is_same_v<Type, WireSchemaEnumValue>) {
					if (!Typed.EnumSchemaId.IsValid() || Typed.DefinitionVersion == 0)
						throw std::invalid_argument("Wire schema enum identity or version is invalid");
				} else if constexpr (std::is_same_v<Type, WireObjectReference>) {
					if (!Typed.Object.IsValid()) throw std::invalid_argument("Wire object reference identity is invalid");
				}
			},
			Value
		);
	}
}
