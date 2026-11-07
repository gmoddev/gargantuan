#pragma once

#include "gargantuan/assets/InstanceSerialization.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace gargantuan { class RuntimeSchemaRegistry; }

namespace gargantuan::InstanceSerialization::Internal {
	struct PreparedInstanceDocument;
	using PreparedInstanceDocumentPtr = std::shared_ptr<const PreparedInstanceDocument>;
	using PreparedInstanceDocumentResult = std::expected<PreparedInstanceDocumentPtr, std::string>;

	[[nodiscard]] PreparedInstanceDocumentResult PrepareDetachedJson(
		std::span<const std::uint8_t> Bytes, std::size_t ExpectedObjects = 0,
		std::shared_ptr<const RuntimeSchemaRegistry> SchemaSnapshot = {});
	// Conservative owned-capacity accounting, not allocator metadata or process RSS.
	[[nodiscard]] std::size_t GetPreparedDocumentRetainedBytes(const PreparedInstanceDocumentPtr &Prepared);
	[[nodiscard]] DeserializationState MaterializeDetachedJson(const PreparedInstanceDocumentPtr &Prepared);
}
