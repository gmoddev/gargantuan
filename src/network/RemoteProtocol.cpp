#include "gargantuan/network/RemoteProtocol.hpp"

#include "RemoteValueBudget.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <limits>
#include <new>

namespace gargantuan::network {
	namespace {
		using detail::RemoteHeaderBytes;
		using detail::RemoteValueBudget;

		constexpr std::uint32_t RemoteMagic = 0x544d5247; // "GRMT" in little endian.
		constexpr std::uint8_t ArrayTag = 15;
		constexpr std::uint8_t DictionaryTag = 16;
		constexpr std::uint8_t BufferTag = 17;
		static_assert(std::variant_size_v<WireValue> == ArrayTag);

		bool HasRequest(RemoteMessageKind Kind) {
			return Kind == RemoteMessageKind::Request || Kind == RemoteMessageKind::Response ||
				   Kind == RemoteMessageKind::RequestError || Kind == RemoteMessageKind::Cancellation;
		}

		bool IsValidMessageHeader(const RemoteMessage &Message) {
			if (Message.Version != RemoteProtocolVersion || Message.Kind > RemoteMessageKind::Cancellation ||
				!Message.Remote.IsValid() || !Message.Publication.IsValid() ||
				Message.Arguments.size() > MaximumRemoteArguments)
				return false;
			if (Message.Request.IsValid() != HasRequest(Message.Kind)) return false;
			if (Message.Sequence.IsValid() != (Message.Kind == RemoteMessageKind::SequencedEvent)) return false;
			if (Message.Kind == RemoteMessageKind::Request) {
				if (Message.Deadline.count() <= 0 || Message.Deadline > MaximumRemoteRequestDeadline) return false;
			} else if (Message.Deadline.count() != 0)
				return false;
			if ((Message.Kind == RemoteMessageKind::RequestError) != Message.Error.has_value()) return false;
			if (Message.Error &&
				(!Message.Error->IsValid() || Message.Error->Code.size() > MaximumRemoteErrorCodeBytes ||
				 Message.Error->Message.size() > MaximumRemoteErrorMessageBytes))
				return false;
			return (Message.Kind != RemoteMessageKind::RequestError && Message.Kind != RemoteMessageKind::Cancellation) ||
				Message.Arguments.empty();
		}

		bool ChargeValue(const RemoteValue &Argument, std::size_t Depth, RemoteValueBudget &Budget) {
			if (!Budget.Value() || (Depth != 0 && std::holds_alternative<std::monostate>(Argument))) return false;
			return std::visit([&](const auto &Value) {
				using Type = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<Type, RemoteArray>) {
					if (Depth >= MaximumRemoteContainerDepth || !Budget.Wire(2) ||
						!Budget.Children(Value.Values.size(), sizeof(RemoteValue))) return false;
					for (const auto &Entry : Value.Values)
						if (!ChargeValue(Entry, Depth + 1, Budget)) return false;
					return true;
				} else if constexpr (std::is_same_v<Type, RemoteDictionary>) {
					if (Depth >= MaximumRemoteContainerDepth || Value.Entries.empty() || !Budget.Wire(2) ||
						!Budget.Children(Value.Entries.size(), sizeof(RemoteDictionaryEntry))) return false;
					const std::string *Previous = nullptr;
					for (const auto &Entry : Value.Entries) {
						ValidateProtocolString(Entry.Key, MaximumRemoteDictionaryKeyBytes, "Remote dictionary key");
						if ((Previous && *Previous >= Entry.Key) || !Budget.Wire(4 + Entry.Key.size()) ||
							!Budget.Storage(Entry.Key.size()) || !ChargeValue(Entry.Value, Depth + 1, Budget))
							return false;
						Previous = &Entry.Key;
					}
					return true;
				} else if constexpr (std::is_same_v<Type, RemoteBuffer>) {
					return Value.Bytes.size() <= MaximumRemoteBufferBytes && Budget.Wire(4 + Value.Bytes.size()) &&
						Budget.Storage(Value.Bytes.size());
				} else if constexpr (std::is_same_v<Type, std::string>) {
					ValidateProtocolString(Value, MaximumRemoteStringBytes, "Remote string");
					return detail::ChargeRemoteScalar(Budget, Value);
				} else {
					ValidateProtocolWireValue(WireValue(Value));
					return detail::ChargeRemoteScalar(Budget, Value);
				}
			}, Argument);
		}

		void WriteValue(GameBinaryWriter &Output, const RemoteValue &Argument) {
			std::visit([&](const auto &Value) {
				using Type = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<Type, RemoteArray>) {
					Output.Integer(ArrayTag);
					Output.Integer(static_cast<std::uint16_t>(Value.Values.size()));
					for (const auto &Entry : Value.Values)
						WriteValue(Output, Entry);
				} else if constexpr (std::is_same_v<Type, RemoteDictionary>) {
					Output.Integer(DictionaryTag);
					Output.Integer(static_cast<std::uint16_t>(Value.Entries.size()));
					for (const auto &Entry : Value.Entries) {
						Output.String(Entry.Key);
						WriteValue(Output, Entry.Value);
					}
				} else if constexpr (std::is_same_v<Type, RemoteBuffer>) {
					Output.Integer(BufferTag);
					Output.Integer(static_cast<std::uint32_t>(Value.Bytes.size()));
					if (!Value.Bytes.empty()) Output.Append(Value.Bytes);
				} else if constexpr (std::is_same_v<Type, std::string>) {
					Output.Integer(static_cast<std::uint8_t>(Argument.index()));
					Output.String(Value);
				} else {
					WriteBinaryWireValue(Output, WireValue(Value));
				}
			}, Argument);
		}

		bool ReadValue(GameBinaryReader &Input, RemoteValue &Argument, std::size_t Depth, RemoteValueBudget &Budget) {
			if (!Budget.Value()) return Input.Fail("Remote value count or payload exceeds its limit");
			std::uint8_t Tag;
			if (!Input.Peek(Tag)) return false;
			if (Tag < ArrayTag) {
				if (Tag == 0 && Depth != 0) return Input.Fail("Remote container entries cannot be nil");
				WireValue Scalar;
				if (!ReadBinaryWireValue(Input, Scalar, MaximumRemoteStringBytes)) return false;
				if (!std::visit([&](const auto &Value) { return detail::ChargeRemoteScalar(Budget, Value); }, Scalar))
					return Input.Fail("Remote scalar exceeds the payload, reference or allocation limit");
				Argument = RemoteValue(std::move(Scalar));
				return true;
			}
			if (!Input.Integer(Tag)) return false;
			if (Tag == BufferTag) {
				std::uint32_t Count;
				std::span<const std::byte> Bytes;
				if (!Input.Integer(Count)) return false;
				if (Count > MaximumRemoteBufferBytes || !Budget.Wire(4 + Count) || !Budget.Storage(Count))
					return Input.Fail("Remote buffer exceeds its byte limit");
				if (!Input.Take(Count, Bytes)) return false;
				RemoteBuffer Result;
				if (Count != 0) Result.Bytes.assign(Bytes.begin(), Bytes.end());
				Argument = RemoteValue(std::move(Result));
				return true;
			}
			if (Tag != ArrayTag && Tag != DictionaryTag) return Input.Fail("Remote value tag is unknown");
			if (Depth >= MaximumRemoteContainerDepth) return Input.Fail("Remote container depth exceeds its limit");
			std::uint16_t Count;
			if (!Input.Integer(Count)) return false;
			if (!Budget.Wire(2)) return Input.Fail("Remote container exceeds the payload limit");
			if (Tag == ArrayTag) {
				if (Count > Input.Remaining() || !Budget.Children(Count, sizeof(RemoteValue)))
					return Input.Fail("Remote array length exceeds its payload or value limit");
				RemoteArray Result;
				Result.Values.reserve(Count);
				for (std::size_t Index = 0; Index < Count; ++Index) {
					RemoteValue Entry;
					if (!ReadValue(Input, Entry, Depth + 1, Budget)) return false;
					Result.Values.push_back(std::move(Entry));
				}
				Argument = RemoteValue(std::move(Result));
				return true;
			}
			if (Count == 0 || Count > Input.Remaining() / 5 ||
				!Budget.Children(Count, sizeof(RemoteDictionaryEntry)))
				return Input.Fail("Remote dictionary length is invalid");
			RemoteDictionary Result;
			Result.Entries.reserve(Count);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				RemoteDictionaryEntry Entry;
				if (!Input.String(Entry.Key, MaximumRemoteDictionaryKeyBytes)) return false;
				if (Entry.Key.find('\0') != std::string::npos)
					return Input.Fail("Remote dictionary key contains an embedded null");
				if (!Result.Entries.empty() && Result.Entries.back().Key >= Entry.Key)
					return Input.Fail("Remote dictionary keys are duplicated or out of order");
				if (!Budget.Wire(4 + Entry.Key.size()) || !Budget.Storage(Entry.Key.size()))
					return Input.Fail("Remote dictionary key exceeds the allocation limit");
				if (!ReadValue(Input, Entry.Value, Depth + 1, Budget)) return false;
				Result.Entries.push_back(std::move(Entry));
			}
			Argument = RemoteValue(std::move(Result));
			return true;
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
		return IsValidMessageHeader(*this) && GetRemoteArgumentStorageBytes(Arguments).has_value();
	}

	std::optional<std::size_t> GetRemoteArgumentStorageBytes(std::span<const RemoteValue> Arguments) {
		RemoteValueBudget Budget;
		if (Arguments.size() > MaximumRemoteArguments || !Budget.Children(Arguments.size(), sizeof(RemoteValue)))
			return std::nullopt;
		try {
			for (const auto &Argument : Arguments)
				if (!ChargeValue(Argument, 0, Budget)) return std::nullopt;
		} catch (...) {
			return std::nullopt;
		}
		return Budget.StorageBytes;
	}

	SerializationResult<std::vector<std::byte>> EncodeRemoteMessage(const RemoteMessage &Message) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RemoteEncode);
		try {
			if (!Message.IsValid())
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Remote message is invalid");
			GameBinaryWriter Payload(MaximumRemoteFrameBytes - RemoteHeaderBytes);
			for (const auto &Argument : Message.Arguments)
				WriteValue(Payload, Argument);
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
			Output.Integer(Message.Publication.Value());
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
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::RemoteEncode, Output.Bytes.size());
			return std::move(Output.Bytes);
		} catch (const std::bad_alloc &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Remote encode allocation failed");
		}
	}

	SerializationResult<RemoteMessage>
	DecodeRemoteMessage(std::span<const std::byte> Bytes, std::size_t *DecodedArgumentBytes) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RemoteDecode);
		if (DecodedArgumentBytes) *DecodedArgumentBytes = 0;
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
			std::uint64_t Publication, Request, Sequence;
			if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(Kind) || !Input.Integer(Reserved) ||
				!ReadBinaryObjectId(Input, Remote) || !Input.Integer(Publication) || !Input.Integer(Request) ||
				!Input.Integer(Sequence) ||
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
			if (ArgumentCount > Input.Remaining())
				return SerializationFailure(
					SerializationErrorCode::TruncatedInput, "Remote arguments cannot fit in the payload"
				);
			RemoteMessage Message{
				.Version = Version,
				.Kind = static_cast<RemoteMessageKind>(Kind),
				.Remote = Remote,
				.Publication = RemotePublicationId(Publication),
				.Request = RemoteRequestId(Request),
				.Sequence = RemoteEventSequence(Sequence),
				.Deadline = std::chrono::milliseconds(Deadline),
			};
			RemoteValueBudget Budget;
			if (!Budget.Children(ArgumentCount, sizeof(RemoteValue)))
				return SerializationFailure(SerializationErrorCode::LimitExceeded, "Remote argument allocation exceeds its limit");
			Message.Arguments.reserve(ArgumentCount);
			for (std::uint16_t Index = 0; Index < ArgumentCount; ++Index) {
				RemoteValue Argument;
				if (!ReadValue(Input, Argument, 0, Budget))
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
			if (!Input.Complete() || !IsValidMessageHeader(Message))
				return SerializationFailure(
					SerializationErrorCode::InvalidValue, "Remote frame contains trailing or invalid data"
				);
			if (DecodedArgumentBytes) *DecodedArgumentBytes = Budget.StorageBytes;
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::RemoteDecode, Budget.StorageBytes);
			return Message;
		} catch (const std::bad_alloc &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Remote decode allocation failed");
		}
	}
}
