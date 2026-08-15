#pragma once

#include "gargantuan/runtime/WireValue.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan::JsonCodec {
	using Json = nlohmann::ordered_json;

	SerializationResult<Json> Parse(
		std::string_view Encoded,
		std::size_t MaximumBytes,
		std::string_view DocumentName
	);
	SerializationResult<std::string> Encode(const Json &Value, std::string_view DocumentName);
	void ValidateTree(const Json &Value);

	bool HasOnlyFields(const Json &Value, std::initializer_list<std::string_view> Allowed);
	std::optional<std::uint32_t> DecodeUnsigned32(const Json &Value);
	Json EncodeObjectId(WireObjectId Id);
	std::optional<WireObjectId> DecodeObjectId(const Json &Value);
	Json EncodeWireValue(const WireValue &Value);
	std::optional<WireValue> DecodeWireValue(const Json &Encoded);
}
