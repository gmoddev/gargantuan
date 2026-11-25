#pragma once

#include "gargantuan/network/Remote.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/network/RemoteValue.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gargantuan::network {
	// GameSessionProtocolVersion gates this required capability at handshake.
	inline constexpr std::uint16_t RemoteProtocolVersion = 3;
	inline constexpr std::size_t MaximumRemoteFrameBytes = 256 * 1024;
	inline constexpr std::size_t MaximumRemoteStringBytes = 16 * 1024;
	inline constexpr std::size_t MaximumRemoteBufferBytes = 64 * 1024;
	inline constexpr std::size_t MaximumRemoteDictionaryKeyBytes = 256;
	inline constexpr std::size_t MaximumRemoteContainerDepth = 8;
	inline constexpr std::size_t MaximumRemoteValueCount = 1024;
	inline constexpr std::size_t MaximumRemoteLuauTableSlots = 8192;
	inline constexpr std::size_t MaximumRemoteReferenceCount = 128;
	inline constexpr std::size_t MaximumRemoteDecodedArgumentBytes = 512 * 1024;
	inline constexpr std::uint16_t MaximumRemoteArguments = 32;
	inline constexpr std::uint32_t MaximumRemoteErrorCodeBytes = 128;
	inline constexpr std::uint32_t MaximumRemoteErrorMessageBytes = 1024;
	inline constexpr std::chrono::milliseconds DefaultRemoteRequestDeadline{10'000};
	inline constexpr std::chrono::milliseconds MaximumRemoteRequestDeadline{30'000};

	enum class RemoteInstanceKind : std::uint8_t {
		ReliableEvent,
		UnreliableEvent,
		UnreliableSequencedEvent,
		Function,
	};

	enum class RemoteMessageKind : std::uint8_t {
		ReliableEvent,
		UnreliableEvent,
		SequencedEvent,
		Request,
		Response,
		RequestError,
		Cancellation,
	};

	struct RemoteMessage {
		std::uint16_t Version = RemoteProtocolVersion;
		RemoteMessageKind Kind = RemoteMessageKind::ReliableEvent;
		ObjectId Remote;
		RemotePublicationId Publication{1};
		RemoteRequestId Request;
		RemoteEventSequence Sequence;
		std::chrono::milliseconds Deadline{};
		std::vector<RemoteValue> Arguments;
		std::optional<StructuredRemoteError> Error;

		[[nodiscard]] bool IsValid() const;
	};

	[[nodiscard]] bool IsRemoteMessageKindCompatible(RemoteMessageKind Message, RemoteInstanceKind Remote);
	// Returns logical retained native bytes for a valid argument tree. Vector
	// capacity slack and allocator metadata are outside this observation.
	[[nodiscard]] std::optional<std::size_t> GetRemoteArgumentStorageBytes(std::span<const RemoteValue> Arguments);
	[[nodiscard]] SerializationResult<std::vector<std::byte>> EncodeRemoteMessage(const RemoteMessage &Message);
	[[nodiscard]] SerializationResult<RemoteMessage>
	DecodeRemoteMessage(std::span<const std::byte> Bytes, std::size_t *DecodedArgumentBytes = nullptr);
}
