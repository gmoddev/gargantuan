#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gargantuan::host::detail {
// Farm-only Main-thread evidence. Each completed server iteration is measured
// before the deliberate 60-Hz sleep. Phase markers use the same steady clock.
class FarmServerTickEvidence final {
public:
	enum class Kind : std::uint8_t { Tick = 1, PhaseStart, PhaseEnd };
	static constexpr std::size_t RecordLimit = 48'016;
	static constexpr std::size_t MaximumFileBytes = RecordLimit * 32 + 512;

private:
	struct Record {
		Kind Type;
		std::uint8_t Phase;
		std::uint64_t Tick;
		std::uint64_t StartedNanoseconds;
		std::uint64_t EndedNanoseconds;
	};

	std::filesystem::path Path;
	std::string RunId;
	std::vector<Record> Records;
	std::uint64_t ActiveTick = 0;
	std::uint64_t TickStartedNanoseconds = 0;
	bool Dumped = false, Invalid = false, Overflow = false, WriteFailed = false;
	std::size_t FileBytes = 0;

	static std::uint64_t Now() noexcept {
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	static void Put64(std::byte *Bytes, std::uint64_t Value) noexcept {
		for (std::size_t Index = 0; Index < 8; ++Index)
			Bytes[Index] = static_cast<std::byte>((Value >> (8 * Index)) & 0xff);
	}
	static void WriteAll(int Handle, std::span<const std::byte> Bytes) {
		while (!Bytes.empty()) {
#if defined(_WIN32)
			const auto Count = _write(Handle, Bytes.data(), static_cast<unsigned>(std::min<std::size_t>(Bytes.size(), 1u << 20)));
#else
			const auto Count = write(Handle, Bytes.data(), std::min<std::size_t>(Bytes.size(), 1u << 20));
#endif
			if (Count <= 0) throw std::runtime_error("farm server tick evidence write failed");
			Bytes = Bytes.subspan(static_cast<std::size_t>(Count));
		}
	}
	void Add(Record Value) noexcept {
		if (Records.size() == RecordLimit) { Overflow = true; return; }
		try { Records.push_back(Value); } catch (...) { Overflow = true; }
	}

public:
	FarmServerTickEvidence(std::string RunIdValue, std::filesystem::path EvidencePath)
		: Path(std::move(EvidencePath)), RunId(std::move(RunIdValue)) {
		if (RunId.empty() || RunId.size() > 64 ||
			!std::all_of(RunId.begin(), RunId.end(), [](char Value) {
				return (Value >= 'A' && Value <= 'Z') || (Value >= 'a' && Value <= 'z') ||
					(Value >= '0' && Value <= '9') || Value == '-' || Value == '_';
			}) || !Path.is_absolute() || Path.filename() != "server-work-ticks.bin" ||
			!std::filesystem::is_directory(Path.parent_path()) ||
			std::filesystem::is_symlink(Path.parent_path()) || std::filesystem::exists(Path))
			throw std::invalid_argument("farm server tick evidence path or identity is invalid");
		Records.reserve(RecordLimit);
	}
	~FarmServerTickEvidence() { Dump(); }
	FarmServerTickEvidence(const FarmServerTickEvidence &) = delete;
	FarmServerTickEvidence &operator=(const FarmServerTickEvidence &) = delete;

	void BeginTick(std::uint64_t Tick) noexcept {
		if (ActiveTick != 0 || Tick == 0) { Invalid = true; return; }
		ActiveTick = Tick;
		TickStartedNanoseconds = Now();
	}
	void EndTick(std::uint64_t Tick) noexcept {
		const auto Ended = Now();
		if (ActiveTick != Tick || TickStartedNanoseconds == 0 || Ended < TickStartedNanoseconds) {
			Invalid = true;
			return;
		}
		Add({Kind::Tick, 0, Tick, TickStartedNanoseconds, Ended});
		ActiveTick = TickStartedNanoseconds = 0;
	}
	void PhaseStart(std::uint8_t Phase, std::uint64_t Tick) noexcept {
		if (ActiveTick != Tick || Phase > 4) { Invalid = true; return; }
		Add({Kind::PhaseStart, Phase, Tick, Now(), 0});
	}
	void PhaseEnd(std::uint8_t Phase, std::uint64_t Tick) noexcept {
		if (ActiveTick != Tick || Phase > 4) { Invalid = true; return; }
		Add({Kind::PhaseEnd, Phase, Tick, Now(), 0});
	}
	void Dump() noexcept {
		if (Dumped) return;
		Dumped = true;
		if (ActiveTick != 0) Invalid = true;
		int Handle = -1;
		try {
#if defined(_WIN32)
			if (_wsopen_s(&Handle, Path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
				_SH_DENYRW, _S_IREAD | _S_IWRITE) != 0)
				throw std::runtime_error("farm server tick evidence file is not new");
#else
			Handle = open(Path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (Handle < 0) throw std::runtime_error("farm server tick evidence file is not new");
#endif
			const std::string Header = "format=GargantuanFarmServerTicksV1\trun=" + RunId +
				"\tcount=" + std::to_string(Records.size()) +
				"\tdropped=" + std::to_string(Overflow ? 1 : 0) +
				"\tinvalid=" + std::to_string(Invalid ? 1 : 0) + "\n";
			WriteAll(Handle, std::as_bytes(std::span(Header.data(), Header.size())));
			FileBytes = Header.size();
			std::array<std::byte, 32 * 1024> Buffer{};
			std::size_t Buffered = 0;
			for (const auto &Value : Records) {
				auto *Bytes = Buffer.data() + Buffered;
				Bytes[0] = static_cast<std::byte>(Value.Type);
				Bytes[1] = static_cast<std::byte>(Value.Phase);
				Put64(Bytes + 8, Value.Tick);
				Put64(Bytes + 16, Value.StartedNanoseconds);
				Put64(Bytes + 24, Value.EndedNanoseconds);
				Buffered += 32;
				if (Buffered == Buffer.size()) { WriteAll(Handle, std::span(Buffer)); Buffered = 0; }
			}
			if (Buffered) WriteAll(Handle, std::span(Buffer.data(), Buffered));
			FileBytes += Records.size() * 32;
			if (FileBytes > MaximumFileBytes) throw std::runtime_error("farm server tick evidence byte limit exceeded");
#if defined(_WIN32)
			if (_commit(Handle) != 0) throw std::runtime_error("farm server tick evidence flush failed");
#else
			if (fsync(Handle) != 0) throw std::runtime_error("farm server tick evidence flush failed");
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
	[[nodiscard]] bool Valid() const noexcept { return Dumped && !Invalid && !Overflow && !WriteFailed; }
	[[nodiscard]] std::size_t Count() const noexcept { return Records.size(); }
	[[nodiscard]] std::size_t BytesWritten() const noexcept { return FileBytes; }
};
}
