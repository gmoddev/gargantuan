#pragma once

#include "gargantuan/runtime/WireValue.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace gargantuan {
	inline constexpr std::size_t MaximumAttributeNameBytes = 100;
	inline constexpr std::size_t MaximumAttributesPerInstance = 64;
	inline constexpr std::size_t MaximumAttributeValueBytes = 4 * 1024;
	inline constexpr std::size_t MaximumAttributeBytesPerInstance = 16 * 1024;
	inline constexpr std::size_t MaximumReplicatedAttributeBytesPerInstance = MaximumAttributeBytesPerInstance;

	void ValidateAttributeName(std::string_view name);
	[[nodiscard]] std::size_t ValidateAttributeValue(const WireValue &value);
	[[nodiscard]] std::size_t ValidateAttributeCollection(const std::map<std::string, WireValue> &attributes);
}
