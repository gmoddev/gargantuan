#pragma once

#include "gargantuan/runtime/WireValue.hpp"

#include <any>
#include <nlohmann/json.hpp>
#include <optional>

namespace gargantuan {
	using WireJson = nlohmann::ordered_json;

	WireJson EncodeWireObjectId(WireObjectId id);
	std::optional<WireObjectId> DecodeWireObjectId(const WireJson &value);
	WireJson EncodeWireValue(const WireValue &value);
	std::optional<WireValue> DecodeWireValue(const WireJson &encoded);
	std::optional<WireValue> EncodeNativeWireValue(const std::any &value);
	std::optional<std::any> DecodeNativeWireValue(const WireValue &value);
}
