#pragma once

#include "gargantuan/runtime/WireValue.hpp"

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace gargantuan {
	inline constexpr std::size_t MaximumProtocolDocumentBytes = 8 * 1024 * 1024;
	inline constexpr std::size_t MaximumProtocolJsonDepth = 64;
	inline constexpr std::size_t MaximumProtocolJsonNodes = 1024 * 1024;
	inline constexpr std::size_t MaximumProtocolStringBytes = 64 * 1024;
	inline constexpr std::size_t MaximumProtocolIdentifierBytes = 256;
	inline constexpr std::size_t MaximumSnapshotObjects = 64 * 1024;
	inline constexpr std::size_t MaximumSnapshotPropertiesPerObject = 1024;
	inline constexpr std::size_t MaximumWireJournalRecords = 4096;
	inline constexpr std::size_t MaximumWireJournalHierarchyTraversalSteps = MaximumSnapshotObjects * 4;
	inline constexpr std::size_t MaximumPersistenceObjects = MaximumSnapshotObjects;

	[[nodiscard]] bool IsValidProtocolUtf8(std::string_view Value);
	void ValidateProtocolString(std::string_view Value, std::size_t MaximumBytes, std::string_view Description);
	void ValidateProtocolJsonDocument(std::string_view Value, std::size_t MaximumBytes = MaximumProtocolDocumentBytes);
	void ValidateProtocolJsonTree(const nlohmann::ordered_json &Value);
	void ValidateProtocolWireValue(const WireValue &Value);
}
