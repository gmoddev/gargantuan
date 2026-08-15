#pragma once

#include "gargantuan/runtime/WireValue.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <any>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan {
	class RuntimeSchemaRegistry;
	struct SchemaEnumItem;
	SerializationResult<std::string> EncodeWireObjectIdJson(WireObjectId Id);
	SerializationResult<WireObjectId> DecodeWireObjectIdJson(std::string_view Encoded);
	SerializationResult<std::string> EncodeWireValueJson(const WireValue &Value);
	SerializationResult<WireValue> DecodeWireValueJson(std::string_view Encoded);
	std::size_t MeasureWireValueJsonBytes(const WireValue &Value);
	std::optional<WireValue> EncodeNativeWireValue(const std::any &value);
	std::optional<std::any> DecodeNativeWireValue(const WireValue &value);
	const SchemaEnumItem &ValidateSchemaEnumValue(
		const WireSchemaEnumValue &value,
		const RuntimeSchemaRegistry &registry
	);
	std::string FormatSchemaEnumValue(
		const WireSchemaEnumValue &value,
		const RuntimeSchemaRegistry &registry
	);
}
