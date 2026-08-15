#include "gargantuan/runtime/AttributeValidation.hpp"

#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace gargantuan {
	namespace {
		template <typename... Values> bool AllFinite(Values... values) {
			return (... && std::isfinite(static_cast<double>(values)));
		}
	}

	void ValidateAttributeName(std::string_view name) {
		if (name.empty()) throw std::invalid_argument("Attribute name cannot be empty");
		if (name.size() > MaximumAttributeNameBytes) throw std::invalid_argument("Attribute name exceeds its byte limit");
		if (!IsValidProtocolUtf8(name)) throw std::invalid_argument("Attribute name is not valid UTF-8");
		if (name.find('\0') != std::string_view::npos) throw std::invalid_argument("Attribute name contains an embedded null");
	}

	std::size_t ValidateAttributeValue(const WireValue &value) {
		const bool supported = std::visit(
			[](const auto &typed) {
				using Value = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<Value, std::monostate> || std::is_same_v<Value, WireEnumItem> ||
					std::is_same_v<Value, WireSchemaEnumValue> ||
					std::is_same_v<Value, WireObjectReference>) return false;
				else if constexpr (std::is_same_v<Value, double>) return std::isfinite(typed);
				else if constexpr (std::is_same_v<Value, WireFloat>) return std::isfinite(typed.Value);
				else if constexpr (std::is_same_v<Value, std::string>) return IsValidProtocolUtf8(typed) && typed.find('\0') == std::string::npos;
				else if constexpr (std::is_same_v<Value, WireVector2>) return AllFinite(typed.X, typed.Y);
				else if constexpr (std::is_same_v<Value, WireVector3>) return AllFinite(typed.X, typed.Y, typed.Z);
				else if constexpr (std::is_same_v<Value, WireColor3>) return AllFinite(typed.R, typed.G, typed.B);
				else if constexpr (std::is_same_v<Value, WireUDim>) return std::isfinite(typed.Scale);
				else if constexpr (std::is_same_v<Value, WireUDim2>) return AllFinite(typed.X.Scale, typed.Y.Scale);
				else if constexpr (std::is_same_v<Value, WireCFrame>) {
					for (const auto component : typed.Components) if (!std::isfinite(component)) return false;
					return true;
				} else return true;
			},
			value
		);
		if (!supported) throw std::invalid_argument("Attribute WireValue type is unsupported or non-finite");
		if (const auto *string = std::get_if<std::string>(&value); string && string->size() > MaximumAttributeValueBytes)
			throw std::invalid_argument("Attribute value exceeds its encoded byte limit");
		const auto encodedBytes = EncodeWireValue(value).dump().size();
		if (encodedBytes > MaximumAttributeValueBytes) throw std::invalid_argument("Attribute value exceeds its encoded byte limit");
		return encodedBytes;
	}

	std::size_t ValidateAttributeCollection(const std::map<std::string, WireValue> &attributes) {
		if (attributes.size() > MaximumAttributesPerInstance) throw std::invalid_argument("Instance exceeds its attribute count limit");
		std::size_t aggregateBytes = 0;
		for (const auto &[name, value] : attributes) {
			ValidateAttributeName(name);
			aggregateBytes += name.size() + ValidateAttributeValue(value);
			if (aggregateBytes > MaximumAttributeBytesPerInstance ||
				aggregateBytes > MaximumReplicatedAttributeBytesPerInstance)
				throw std::invalid_argument("Instance exceeds its aggregate attribute byte limit");
		}
		return aggregateBytes;
	}
}
