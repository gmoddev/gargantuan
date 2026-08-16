#pragma once

#include "gargantuan/network/Replication.hpp"
#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gargantuan::network {
	inline constexpr std::uint16_t ReplicationProtocolVersion = 1;
	inline constexpr std::size_t MaximumReplicationFrameBytes = 8 * 1024 * 1024;
	inline constexpr std::uint32_t MaximumReplicationOperationsPerFrame = 64 * 1024;
	inline constexpr std::uint32_t MaximumReplicationSchemaDefinitions = 4096;

	enum class ReplicationMessageKind : std::uint8_t {
		Baseline,
		Incremental,
	};

	struct SchemaCompatibilityEntry {
		SchemaId Id;
		std::uint32_t DefinitionVersion = 0;
		SchemaDefinitionKind Kind = SchemaDefinitionKind::Class;
		auto operator<=>(const SchemaCompatibilityEntry &) const = default;
	};

	struct ReplicationFrame {
		std::uint16_t Version = ReplicationProtocolVersion;
		ReplicationMessageKind Kind = ReplicationMessageKind::Incremental;
		ReplicationEpoch Epoch;
		ReliableReplicationSequence Sequence;
		std::vector<SchemaCompatibilityEntry> Schema;
		std::vector<ReplicationOperation> Operations;

		[[nodiscard]] bool IsValid() const;
	};

	[[nodiscard]] std::vector<SchemaCompatibilityEntry> CaptureReplicationSchemaCompatibility();
	[[nodiscard]] bool IsReplicationSchemaCompatible(std::span<const SchemaCompatibilityEntry> Remote);
	[[nodiscard]] SerializationResult<std::vector<std::byte>> EncodeReplicationFrame(const ReplicationFrame &Frame);
	[[nodiscard]] SerializationResult<ReplicationFrame> DecodeReplicationFrame(std::span<const std::byte> Bytes);
}
