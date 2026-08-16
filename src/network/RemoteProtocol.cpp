#include "gargantuan/network/RemoteProtocol.hpp"

#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <limits>
#include <new>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t RemoteMagic = 0x544d5247; // "GRMT" in little endian.
		constexpr std::size_t RemoteHeaderBytes = 44;
		constexpr std::size_t MinimumRemoteArgumentBytes = 1;

		bool HasRequest(RemoteMessageKind Kind) {
			return Kind == RemoteMessageKind::Request || Kind == RemoteMessageKind::Response ||
				   Kind == RemoteMessageKind::RequestError || Kind == RemoteMessageKind::Cancellation;
		}
	}

	bool IsRemoteMessageKindCompatible(RemoteMessageKind Message, RemoteInstanceKind Remote) {
		switch (Remote) {
		case RemoteInstanceKind::ReliableEvent:
			return Message == RemoteMessageKind::ReliableEvent;
		case RemoteInstanceKind::UnreliableEvent:
			return Message == RemoteMessageKind::UnreliableEvent;
		case RemoteInstanceKind::UnreliableSequencedEvent:
			return Message == RemoteMessageKind::SequencedEvent;
		case RemoteInstanceKind::Function:
			return Message == RemoteMessageKind::Request || Message == RemoteMessageKind::Response ||
				   Message == RemoteMessageKind::RequestError || Message == RemoteMessageKind::Cancellation;
		}
		return false;
	}

	bool RemoteMessage::IsValid() const {
		if (Version != RemoteProtocolVersion || Kind > RemoteMessageKind::Cancellation || !Remote.IsValid() ||
			Arguments.size() > MaximumRemoteArguments)
			return false;
		const bool RequestExpected = HasRequest(Kind);
		if (Request.IsValid() != RequestExpected) return false;
		if (Sequence.IsValid() != (Kind == RemoteMessageKind::SequencedEvent)) return false;
		if ((Kind == RemoteMessageKind::Request) != (Deadline.count() > 0)) return false;
		if (Deadline > MaximumRemoteRequestDeadline) return false;
		if ((Kind == RemoteMessageKind::RequestError) != Error.has_value()) return false;
		if (Error && (!Error->IsValid() || Error->Code.size() > MaximumRemoteErrorCodeBytes ||
					  Error->Message.size() > MaximumRemoteErrorMessageBytes))
			return false;
		if ((Kind == RemoteMessageKind::RequestError || Kind == RemoteMessageKind::Cancellation) && !Arguments.empty())
			return false;
		try {
			for (const auto &Argument : Arguments) {
				ValidateProtocolWireValue(Argument);
				if (const auto *String = std::get_if<std::string>(&Argument);
					String && String->size() > MaximumRemoteStringBytes)
					return false;
			}
		} catch (...) {
			return false;
		}
		return true;
	}

	SerializationResult<std::vector<std::byte>> EncodeRemoteMessage(const RemoteMessage &Message) {
		try {
			if (!Message.IsValid())
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Remote message is invalid");
			GameBinaryWriter Payload(MaximumRemoteFrameBytes - RemoteHeaderBytes);
			for (const auto &Argument : Message.Arguments)
				WriteBinaryWireValue(Payload, Argument);
			if (Message.Error) {
				Payload.String(Message.Error->Code);
				Payload.String(Message.Error->Message);
			}
			if (!Payload.Succeeded())
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Remote payload exceeds its byte limit"
				);
			GameBinaryWriter Output(MaximumRemoteFrameBytes);
			Output.Integer(RemoteMagic);
			Output.Integer(Message.Version);
			Output.Integer(static_cast<std::uint8_t>(Message.Kind));
			Output.Integer<std::uint8_t>(0);
			WriteBinaryObjectId(Output, Message.Remote);
			Output.Integer(Message.Request.Value());
			Output.Integer(Message.Sequence.Value());
			Output.Integer(static_cast<std::uint32_t>(Message.Deadline.count()));
			Output.Integer(static_cast<std::uint16_t>(Message.Arguments.size()));
			Output.Integer<std::uint16_t>(0);
			Output.Integer(static_cast<std::uint32_t>(Payload.Bytes.size()));
			Output.Append(Payload.Bytes);
			if (!Output.Succeeded())
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Remote frame exceeds its byte limit"
				);
			return std::move(Output.Bytes);
		} catch (const std::bad_alloc &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Remote encode allocation failed");
		}
	}

	SerializationResult<RemoteMessage> DecodeRemoteMessage(std::span<const std::byte> Bytes) {
		try {
			if (Bytes.size() > MaximumRemoteFrameBytes)
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Remote frame exceeds its byte limit"
				);
			GameBinaryReader Input(Bytes, "Remote frame");
			std::uint32_t Magic, Deadline, PayloadBytes;
			std::uint16_t Version, ArgumentCount, ReservedTail;
			std::uint8_t Kind, Reserved;
			ObjectId Remote;
			std::uint64_t Request, Sequence;
			if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(Kind) || !Input.Integer(Reserved) ||
				!ReadBinaryObjectId(Input, Remote) || !Input.Integer(Request) || !Input.Integer(Sequence) ||
				!Input.Integer(Deadline) || !Input.Integer(ArgumentCount) || !Input.Integer(ReservedTail) ||
				!Input.Integer(PayloadBytes))
				return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
			if (Magic != RemoteMagic)
				return SerializationFailure(SerializationErrorCode::InvalidSyntax, "Remote frame magic is invalid");
			if (Version != RemoteProtocolVersion)
				return SerializationFailure(
					SerializationErrorCode::UnsupportedVersion, "Remote protocol version is unsupported"
				);
			if (Kind > static_cast<std::uint8_t>(RemoteMessageKind::Cancellation) || Reserved != 0 || ReservedTail != 0)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Remote frame header is invalid");
			if (ArgumentCount > MaximumRemoteArguments)
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Remote argument count exceeds its limit"
				);
			if (PayloadBytes != Input.Remaining())
				return SerializationFailure(
					SerializationErrorCode::TruncatedInput, "Remote payload length does not match the frame"
				);
			if (ArgumentCount > Input.Remaining() / MinimumRemoteArgumentBytes)
				return SerializationFailure(
					SerializationErrorCode::TruncatedInput, "Remote arguments cannot fit in the payload"
				);
			RemoteMessage Message{
				.Version = Version,
				.Kind = static_cast<RemoteMessageKind>(Kind),
				.Remote = Remote,
				.Request = RemoteRequestId(Request),
				.Sequence = RemoteEventSequence(Sequence),
				.Deadline = std::chrono::milliseconds(Deadline),
			};
			Message.Arguments.reserve(ArgumentCount);
			for (std::uint16_t Index = 0; Index < ArgumentCount; ++Index) {
				WireValue Argument;
				if (!ReadBinaryWireValue(Input, Argument, MaximumRemoteStringBytes))
					return SerializationFailure(SerializationErrorCode::InvalidValue, Input.Error);
				Message.Arguments.push_back(std::move(Argument));
			}
			if (Message.Kind == RemoteMessageKind::RequestError) {
				StructuredRemoteError Error;
				if (!Input.String(Error.Code, MaximumRemoteErrorCodeBytes) ||
					!Input.String(Error.Message, MaximumRemoteErrorMessageBytes))
					return SerializationFailure(SerializationErrorCode::InvalidValue, Input.Error);
				Message.Error = std::move(Error);
			}
			if (!Input.Complete() || !Message.IsValid())
				return SerializationFailure(
					SerializationErrorCode::InvalidValue, "Remote frame contains trailing or invalid data"
				);
			return Message;
		} catch (const std::bad_alloc &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Remote decode allocation failed");
		}
	}
}
