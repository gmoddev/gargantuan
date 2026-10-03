#pragma once

#include "../../runtime/PublicationLatencyDiagnostics.hpp"
#include "gargantuan/network/CharacterProtocol.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"
#include "gargantuan/network/BinaryCodec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gargantuan::host::detail {
// A farm-only, Main-thread diagnostic. It owns fixed metadata, never payloads,
// and does no file I/O while a measured host loop is running. Overflow or a
// failed GCHR decode invalidates the evidence instead of truncating a PASS.
class FarmPublicationEvidence final {
public:
	enum class Stage : std::uint16_t {
		FrameBegin = 1, CharacterNextDue, CharacterDue, StateBuilt,
		CharacterSnapshot, CharacterProduced, CharacterUnchanged,
		SchedulerAccepted, ClientNativeReceive, ClientHandled,
		RecipientRetired, CharacterDirectOffered, CharacterDirectRejected,
		RpcRequestStarted, RpcRequestSchedulerAccepted, RpcRequestGnsAccepted,
		RpcRequestReceived, RpcHandler, RpcResponseProduced,
		RpcResponseSchedulerAccepted, RpcResponseGnsAccepted,
		RpcResponseReceived, RpcCompletion,
		OrdinaryReliableSent, OrdinaryReliableRejected, TrafficPhase,
	};
	struct Record {
		std::uint64_t Nanoseconds = 0;
		std::uint64_t Tick = 0;
		std::uint64_t Sequence = 0;
		std::uint64_t DueTick = 0;
		std::uint64_t ControlEpoch = 0;
		std::uint64_t MaterializationEpoch = 0;
		std::uint64_t FrameSequence = 0;
		std::uint32_t ConnectionSlot = 0;
		std::uint32_t ConnectionGeneration = 0;
		std::uint32_t ObjectSlot = 0;
		std::uint32_t ObjectGeneration = 0;
		std::uint32_t Bytes = 0;
		Stage Kind = Stage::FrameBegin;
		std::uint16_t Flags = 0;
	};
	static_assert(sizeof(Record) == 80);
	static constexpr std::size_t ServerRecordLimit = 4'194'304;
	static constexpr std::size_t ClientRecordLimit = 131'072;
	static constexpr std::uint64_t MaximumFileBytes = ServerRecordLimit * 80ull + 512;

private:
	std::filesystem::path Path;
	std::string RunId;
	std::uint64_t Nonce = 0;
	std::size_t Limit;
	int Slot;
	bool Server;
	bool Dumped = false, Overflow = false, WriteFailed = false;
	std::uint64_t DecodeFailures = 0, FileBytes = 0;
	std::uint64_t TrafficPhase = 0;
	std::vector<Record> Records;
	runtime_detail::PublicationLatencySink Sink{this, Selected, Append, Packet};
	runtime_detail::PublicationLatencySink *Previous = nullptr;

	static bool Selected(void *Context, network::ConnectionId Connection) noexcept {
		auto &Self = *static_cast<FarmPublicationEvidence *>(Context);
		return Self.Server || !Connection.IsValid() || Connection == Self.ClientConnection;
	}
	static std::optional<Stage> StageOf(std::string_view Value) noexcept {
		if (Value == "FrameBegin") return Stage::FrameBegin;
		if (Value == "CharacterNextDue") return Stage::CharacterNextDue;
		if (Value == "CharacterDue") return Stage::CharacterDue;
		if (Value == "StateBuilt") return Stage::StateBuilt;
		if (Value == "CharacterSnapshot") return Stage::CharacterSnapshot;
		if (Value == "CharacterProduced") return Stage::CharacterProduced;
		if (Value == "CharacterUnchanged") return Stage::CharacterUnchanged;
		if (Value == "SchedulerAccepted") return Stage::SchedulerAccepted;
		if (Value == "ClientNativeReceive") return Stage::ClientNativeReceive;
		if (Value == "ClientHandled") return Stage::ClientHandled;
		if (Value == "RecipientRetired") return Stage::RecipientRetired;
		if (Value == "CharacterDirectOffered") return Stage::CharacterDirectOffered;
		if (Value == "CharacterDirectRejected") return Stage::CharacterDirectRejected;
		if (Value == "RpcRequestStarted") return Stage::RpcRequestStarted;
		if (Value == "RpcRequestSchedulerAccepted") return Stage::RpcRequestSchedulerAccepted;
		if (Value == "RpcRequestReceived") return Stage::RpcRequestReceived;
		if (Value == "RpcHandler") return Stage::RpcHandler;
		if (Value == "RpcResponseProduced") return Stage::RpcResponseProduced;
		if (Value == "RpcResponseSchedulerAccepted") return Stage::RpcResponseSchedulerAccepted;
		if (Value == "RpcResponseReceived") return Stage::RpcResponseReceived;
		if (Value == "RpcCompletion") return Stage::RpcCompletion;
		if (Value == "OrdinaryReliableSent") return Stage::OrdinaryReliableSent;
		if (Value == "OrdinaryReliableRejected") return Stage::OrdinaryReliableRejected;
		if (Value == "TrafficPhaseStart" || Value == "TrafficPhaseEnd") return Stage::TrafficPhase;
		return std::nullopt;
	}
	static std::uint64_t TrafficClockNow() noexcept {
#if defined(_WIN32)
		LARGE_INTEGER Counter{};
		return QueryPerformanceCounter(&Counter) && Counter.QuadPart > 0
			? static_cast<std::uint64_t>(Counter.QuadPart) : 0;
#else
		return 0; // The physical farm's shared-host domain is explicitly Windows QPC.
#endif
	}
	void Add(Record Value) noexcept {
		if (Records.size() >= Limit) { Overflow = true; return; }
		try { Records.push_back(Value); } catch (...) { Overflow = true; }
	}
	static void Append(void *Context, runtime_detail::PublicationLatencyRecord Value) noexcept {
		auto &Self = *static_cast<FarmPublicationEvidence *>(Context);
		const auto Kind = StageOf(Value.Stage);
		if (!Kind || (Self.Server && (*Kind == Stage::ClientNativeReceive || *Kind == Stage::ClientHandled)) ||
			(!Self.Server && *Kind != Stage::ClientHandled && *Kind < Stage::RpcRequestStarted)) return;
		if (*Kind >= Stage::OrdinaryReliableSent) {
			const auto Counter = TrafficClockNow();
			if (!Counter) { ++Self.DecodeFailures; return; }
			Self.Add(Record{.Nanoseconds = Value.Nanoseconds, .Sequence = Value.Sequence,
				.DueTick = Value.Kind, .FrameSequence = Counter,
				.ConnectionSlot = Value.Connection.Slot, .ConnectionGeneration = Value.Connection.Generation,
				.Bytes = Value.Bytes, .Kind = *Kind,
				.Flags = static_cast<std::uint16_t>(Value.Operations)});
			return;
		}
		Self.Add(Record{
			.Nanoseconds = Value.Nanoseconds, .Tick = Value.Tick,
			.Sequence = Value.Sequence, .DueTick = Value.Due,
			.ControlEpoch = *Kind == Stage::StateBuilt ? Value.Epoch : 0,
			.MaterializationEpoch = *Kind == Stage::CharacterProduced ||
				*Kind == Stage::CharacterDirectOffered || *Kind == Stage::CharacterDirectRejected
				? Value.Epoch : 0,
			.ConnectionSlot = Value.Connection.Slot, .ConnectionGeneration = Value.Connection.Generation,
			.ObjectSlot = Value.Object.Slot, .ObjectGeneration = Value.Object.Generation,
			.Bytes = *Kind >= Stage::RpcRequestStarted ? Value.Bytes : 0,
			.Kind = *Kind, .Flags = static_cast<std::uint16_t>(Value.Operations & 1u),
		});
	}
	static void Packet(void *Context, const char *Name, network::ConnectionId Connection,
		std::span<const std::byte> Payload, std::uint64_t Nanoseconds) noexcept {
		auto &Self = *static_cast<FarmPublicationEvidence *>(Context);
		if (std::string_view(Name) == "Handoff" && Payload.size() >= 32 &&
			std::memcmp(Payload.data(), "GRMT", 4) == 0) {
			// The fixed header is sufficient for request identity. Do not decode
			// 16-KiB Event payloads or copy arguments on the measured send path.
			const auto MessageKind = std::to_integer<std::uint8_t>(Payload[6]);
			if (MessageKind != static_cast<std::uint8_t>(network::RemoteMessageKind::Request) &&
				MessageKind != static_cast<std::uint8_t>(network::RemoteMessageKind::Response) &&
				MessageKind != static_cast<std::uint8_t>(network::RemoteMessageKind::RequestError)) return;
			try {
				network::GameBinaryReader Input(Payload, "Remote diagnostic header");
				std::uint32_t Magic = 0;
				std::uint16_t Version = 0;
				std::uint8_t Kind = 0, Reserved = 0;
				ObjectId Remote;
				std::uint64_t Publication = 0, Request = 0;
				if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(Kind) ||
					!Input.Integer(Reserved) || !network::ReadBinaryObjectId(Input, Remote) ||
					!Input.Integer(Publication) || !Input.Integer(Request)) {
					++Self.DecodeFailures;
					return;
				}
				if (Version != network::RemoteProtocolVersion || Reserved != 0 ||
					Publication == 0 || Request == 0) {
					++Self.DecodeFailures;
					return;
				}
				Self.Add(Record{.Nanoseconds = Nanoseconds, .Sequence = Request,
					.ConnectionSlot = Connection.Slot, .ConnectionGeneration = Connection.Generation,
					.ObjectSlot = Remote.Slot, .ObjectGeneration = Remote.Generation,
					.Bytes = static_cast<std::uint32_t>(Payload.size()),
					.Kind = Kind == static_cast<std::uint8_t>(network::RemoteMessageKind::Request)
						? Stage::RpcRequestGnsAccepted : Stage::RpcResponseGnsAccepted});
			} catch (...) { ++Self.DecodeFailures; }
			return;
		}
		const auto Kind = StageOf(Name);
		if (!Kind || (Self.Server && *Kind != Stage::SchedulerAccepted) ||
			(!Self.Server && *Kind != Stage::ClientNativeReceive && *Kind != Stage::ClientHandled) ||
			Payload.size() < 4 || std::memcmp(Payload.data(), "GCHR", 4) != 0) return;
		try {
			auto Message = network::DecodeCharacterMessage(Payload);
			if (!Message) { ++Self.DecodeFailures; return; }
			const auto *Frame = std::get_if<network::CharacterStateFrame>(&*Message);
			if (!Frame) return;
			for (const auto &State : Frame->GetStates())
				Self.Add(Record{
					.Nanoseconds = Nanoseconds, .Tick = State.AuthoritativeTick,
					.Sequence = State.StateSequence.Value(), .ControlEpoch = State.ControlEpoch.Value(),
					.MaterializationEpoch = Frame->MaterializationEpoch.Value(),
					.FrameSequence = Frame->FrameSequence.Value(),
					.ConnectionSlot = Connection.Slot, .ConnectionGeneration = Connection.Generation,
					.ObjectSlot = State.Character.Slot, .ObjectGeneration = State.Character.Generation,
					.Bytes = static_cast<std::uint32_t>(network::GetCompactCharacterStateEncodedBytes(State)),
					.Kind = *Kind,
				});
		} catch (...) { ++Self.DecodeFailures; }
	}
	static void WriteAll(int Handle, std::span<const std::byte> Bytes) {
		while (!Bytes.empty()) {
#if defined(_WIN32)
			const auto Count = _write(Handle, Bytes.data(), static_cast<unsigned>(std::min<std::size_t>(Bytes.size(), 1u << 20)));
#else
			const auto Count = write(Handle, Bytes.data(), std::min<std::size_t>(Bytes.size(), 1u << 20));
#endif
			if (Count <= 0) throw std::runtime_error("publication evidence write failed");
			Bytes = Bytes.subspan(static_cast<std::size_t>(Count));
		}
	}
	static void Put64(std::byte *Bytes, std::uint64_t Value) {
		for (std::size_t Index = 0; Index < 8; ++Index)
			Bytes[Index] = static_cast<std::byte>((Value >> (8 * Index)) & 0xff);
	}
	static void Put32(std::byte *Bytes, std::uint32_t Value) {
		for (std::size_t Index = 0; Index < 4; ++Index)
			Bytes[Index] = static_cast<std::byte>((Value >> (8 * Index)) & 0xff);
	}
	static void Put16(std::byte *Bytes, std::uint16_t Value) {
		Bytes[0] = static_cast<std::byte>(Value & 0xff);
		Bytes[1] = static_cast<std::byte>((Value >> 8) & 0xff);
	}
	static void Encode(std::byte *Bytes, const Record &Value) {
		Put16(Bytes, static_cast<std::uint16_t>(Value.Kind)); Put16(Bytes + 2, Value.Flags);
		Put32(Bytes + 4, Value.ConnectionSlot); Put32(Bytes + 8, Value.ConnectionGeneration);
		Put32(Bytes + 12, Value.ObjectSlot); Put32(Bytes + 16, Value.ObjectGeneration);
		Put32(Bytes + 20, Value.Bytes);
		Put64(Bytes + 24, Value.Nanoseconds); Put64(Bytes + 32, Value.Tick);
		Put64(Bytes + 40, Value.Sequence); Put64(Bytes + 48, Value.DueTick);
		Put64(Bytes + 56, Value.ControlEpoch); Put64(Bytes + 64, Value.MaterializationEpoch);
		Put64(Bytes + 72, Value.FrameSequence);
	}

	network::ConnectionId ClientConnection;

public:
	FarmPublicationEvidence(bool ServerValue, std::string RunIdValue, int SlotValue, std::uint64_t NonceValue,
		std::filesystem::path EvidencePath, network::ConnectionId ClientConnectionValue = {},
		std::size_t TestRecordLimit = 0)
		: Path(std::move(EvidencePath)), RunId(std::move(RunIdValue)), Nonce(NonceValue),
			Limit(TestRecordLimit ? TestRecordLimit : ServerValue ? ServerRecordLimit : ClientRecordLimit),
			Slot(SlotValue), Server(ServerValue), ClientConnection(ClientConnectionValue) {
		const auto Name = Server ? "publication-service.bin" : "publication-service-" + std::to_string(Slot) + ".bin";
		if (RunId.empty() || RunId.size() > 64 ||
			!std::all_of(RunId.begin(), RunId.end(), [](char Value) { return
				(Value >= 'A' && Value <= 'Z') || (Value >= 'a' && Value <= 'z') ||
				(Value >= '0' && Value <= '9') || Value == '-' || Value == '_'; }) ||
			(Server ? (Slot != -1 || Nonce != 0) : (Slot < 0 || Slot >= 32 || Nonce == 0 || !ClientConnection.IsValid())) ||
			Limit == 0 || Limit > (Server ? ServerRecordLimit : ClientRecordLimit) ||
			!Path.is_absolute() || Path.filename() != Name ||
			!std::filesystem::is_directory(Path.parent_path()) ||
			std::filesystem::is_symlink(Path.parent_path()) || std::filesystem::exists(Path))
			throw std::invalid_argument("farm publication evidence path or identity is invalid");
		Records.reserve(Limit);
		Previous = runtime_detail::ActivePublicationLatency;
		runtime_detail::ActivePublicationLatency = &Sink;
	}
	~FarmPublicationEvidence() { Dump(); runtime_detail::ActivePublicationLatency = Previous; }
	FarmPublicationEvidence(const FarmPublicationEvidence &) = delete;
	FarmPublicationEvidence &operator=(const FarmPublicationEvidence &) = delete;
	void MarkFrameBegin(std::uint64_t Tick) const noexcept {
		if (Server) runtime_detail::RecordPublicationLatency({.Stage = "FrameBegin", .Tick = Tick});
	}
	void SetTrafficPhase(std::string_view Name) noexcept {
		constexpr std::array<std::string_view, 5> Names{"baseline", "load", "resident", "evict", "reload"};
		std::uint64_t Next = 0;
		for (std::size_t I = 0; I < Names.size(); ++I) if (Name == Names[I]) Next = I + 1;
		if (Next == TrafficPhase) return;
		if (TrafficPhase) runtime_detail::RecordPublicationLatency({.Stage="TrafficPhaseEnd",
			.Connection=ClientConnection, .Sequence=TrafficPhase});
		TrafficPhase = Next;
		if (Next) runtime_detail::RecordPublicationLatency({.Stage="TrafficPhaseStart",
			.Connection=ClientConnection, .Sequence=Next, .Operations=1});
	}
	void WriteTrafficClock(std::ostream &Out) const {
		std::string Host = "unknown";
		std::uint64_t Frequency = 0;
#if defined(_WIN32)
		char Buffer[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD Length = sizeof(Buffer);
		LARGE_INTEGER Value{};
		if (GetComputerNameA(Buffer, &Length) && QueryPerformanceFrequency(&Value) && Value.QuadPart > 0) {
			Host.assign(Buffer, Length); Frequency = static_cast<std::uint64_t>(Value.QuadPart);
		}
#endif
		Out << "[Qualification:TrafficClock] contract=sender_qpc_v1 run=" << RunId
			<< " role=" << (Server ? "SERVER" : "CLIENT") << " slot=" << Slot << " nonce=" << Nonce
			<< " host=" << Host << " frequency=" << Frequency << '\n';
	}
	void Dump() noexcept {
		if (Dumped) return;
		Dumped = true;
		runtime_detail::ActivePublicationLatency = Previous;
		int Handle = -1;
		try {
#if defined(_WIN32)
			if (_wsopen_s(&Handle, Path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
				_SH_DENYRW, _S_IREAD | _S_IWRITE) != 0) throw std::runtime_error("publication evidence file is not new");
#else
			Handle = open(Path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (Handle < 0) throw std::runtime_error("publication evidence file is not new");
#endif
			const std::string Header = "format=GargantuanFarmPublicationV1\trun=" + RunId +
				"\trole=" + (Server ? "SERVER" : "CLIENT") + "\tslot=" + std::to_string(Slot) +
				"\tnonce=" + std::to_string(Nonce) + "\tcount=" + std::to_string(Records.size()) +
				"\tdropped=" + std::to_string(Overflow ? 1 : 0) +
				"\tdecode_failures=" + std::to_string(DecodeFailures) + "\n";
			WriteAll(Handle, std::as_bytes(std::span(Header.data(), Header.size())));
			FileBytes = Header.size();
			std::array<std::byte, 80 * 1024> Buffer;
			std::size_t Buffered = 0;
			for (const auto &Value : Records) {
				Encode(Buffer.data() + Buffered, Value);
				Buffered += 80;
				if (Buffered == Buffer.size()) {
					WriteAll(Handle, std::span(Buffer));
					Buffered = 0;
				}
			}
			if (Buffered) WriteAll(Handle, std::span(Buffer.data(), Buffered));
			FileBytes += Records.size() * 80ull;
			if (FileBytes > MaximumFileBytes) throw std::runtime_error("publication evidence byte limit exceeded");
#if defined(_WIN32)
			if (_commit(Handle) != 0) throw std::runtime_error("publication evidence flush failed");
#else
			if (fsync(Handle) != 0) throw std::runtime_error("publication evidence flush failed");
#endif
		} catch (...) { WriteFailed = true; }
		if (Handle >= 0) {
#if defined(_WIN32)
			_close(Handle);
#else
			close(Handle);
#endif
		}
	}
	[[nodiscard]] bool Valid() const noexcept { return Dumped && !Overflow && !WriteFailed && DecodeFailures == 0; }
	[[nodiscard]] bool Overflowed() const noexcept { return Overflow; }
	[[nodiscard]] std::uint64_t Failures() const noexcept { return DecodeFailures; }
	[[nodiscard]] std::size_t Count() const noexcept { return Records.size(); }
	[[nodiscard]] std::uint64_t BytesWritten() const noexcept { return FileBytes; }
};
}
