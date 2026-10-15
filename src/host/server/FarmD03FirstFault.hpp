#pragma once

#include "../../network/PooledServiceDiagnostics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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
// One farm-only, Main-owned sidecar. Healthy observations never write or flush.
// The immutable predicate/owner is copied before fault-only I/O, so that I/O
// cannot be mistaken for the initiating service interval. No scheduling changes.
class FarmD03FirstFault final {
public:
	enum class Stage : std::uint8_t { Setup, Poll, Engine, Session, Qualifier, Sleep, Cleanup };
	static constexpr std::size_t StageCount = 7;
	// Explicit little-endian uint64 words; no native struct layout is persisted.
	enum class Word : std::size_t {
		Version, ConnectionSlot, ConnectionGeneration, SimulationTick, Now,
		Point, Rejected, InvalidAfter, ResultValid, ExecutionFailed, MainFailed, TurnoverFailed,
		WorkReady, AcceptanceAvailable, DiagnosticOnly, BeforeCaptured, BeforeInvalid,
		Armed, HasBoundary, RequiredSince, Boundary, OfferToken, OfferBytes, OfferAt,
		TurnoverToken, TurnoverAt, PermitAvailable, DebtToken, DebtBytes,
		AcceptedAll, AcceptedStructural, AcceptedGameplay, MainElapsedValid, MainElapsed,
		OfferElapsedValid, OfferElapsed, TurnoverElapsedValid, TurnoverElapsed,
		BoundaryBackwards, MainBackwards, OfferBackwards, TurnoverBackwards,
		MaximumMain, MaximumTurnover, RetiredToken, RetiredBytes,
		LastCompletedTurnoverToken, LastCompletedTurnoverActivated, LastCompletedTurnoverAt,
		HostStage, HostTick, HostStageStarted, HostTickStarted, LastHostStage, LastHostStageTick,
		LastHostStageStarted, LastHostStageCompleted, LastIterationTick, LastIterationStarted,
		LastIterationCompleted, NativeObservedAt, FeedbackPresent, NativeState, NativeCountersValid,
		NativeClockValid, Process, NativeThread, ClockSteadyNanoseconds, QpcBefore, QpcAfter, QpcFrequency,
		// Seven fixed slots: stage enum, latest completed tick, start and end.
		CompletedStagesStart, Count = 99
	};
	static constexpr std::size_t RecordBytes = static_cast<std::size_t>(Word::Count) * 8;
	static_assert(static_cast<std::size_t>(Word::Count) == static_cast<std::size_t>(Word::CompletedStagesStart) + StageCount * 4);
	static_assert(RecordBytes == 792 && RecordBytes < 4096);
	struct Correlation {
		std::uint64_t Process = 0, Thread = 0, SteadyNs = 0, QpcBefore = 0, QpcAfter = 0, Frequency = 0;
		bool Valid = false;
	};
	struct Snapshot {
		std::array<std::uint64_t, static_cast<std::size_t>(Word::Count)> Words{};
		std::uint64_t Get(Word Field) const noexcept { return Words[static_cast<std::size_t>(Field)]; }
	};

private:
	int Handle = -1;
	bool Captured = false, WriteFailed = false, Persisted = false;
	Snapshot First;
	Correlation Clock;
	Stage CurrentStage = Stage::Setup, LastStage = Stage::Setup;
	std::uint64_t HostTick = 0, StageStarted = 0, TickStarted = 0;
	std::uint64_t LastStageTick = 0, LastStageStarted = 0, LastStageCompleted = 0;
	std::uint64_t LastIterationTick = 0, LastIterationStarted = 0, LastIterationCompleted = 0;
	struct CompletedStage { std::uint64_t Tick = 0, Started = 0, Completed = 0; };
	std::array<CompletedStage, StageCount> CompletedStages{};

	bool WriteAll(std::span<const std::byte> Bytes) noexcept {
		if (Handle < 0) return false;
		while (!Bytes.empty()) {
#if defined(_WIN32)
			const auto Count = _write(Handle, Bytes.data(), static_cast<unsigned>(Bytes.size()));
#else
			const auto Count = write(Handle, Bytes.data(), Bytes.size());
#endif
			if (Count <= 0) return false;
			Bytes = Bytes.subspan(static_cast<std::size_t>(Count));
		}
		return true;
	}
	bool Flush() noexcept {
#if defined(_WIN32)
		return Handle >= 0 && _commit(Handle) == 0;
#else
		return Handle >= 0 && fsync(Handle) == 0;
#endif
	}

public:
	FarmD03FirstFault(const std::filesystem::path &Path, std::string_view RunId) {
		if (!Path.is_absolute() || Path.filename() != "first-execution-fault.bin" ||
			!std::filesystem::is_directory(Path.parent_path()) || std::filesystem::is_symlink(Path.parent_path()) ||
			RunId.empty() || RunId.size() > 64)
			throw std::invalid_argument("first execution fault path or identity is invalid");
		for (const auto Value : RunId)
			if (!((Value >= 'a' && Value <= 'z') || (Value >= 'A' && Value <= 'Z') ||
				(Value >= '0' && Value <= '9') || Value == '-' || Value == '_'))
				throw std::invalid_argument("first execution fault run identity is invalid");
		const auto Header = "format=GargantuanFarmD03FirstFaultV1\trun=" + std::string(RunId) +
			"\tclock=steady_us\trecord_bytes=" + std::to_string(RecordBytes) + "\n";
#if defined(_WIN32)
		if (_wsopen_s(&Handle, Path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
			_SH_DENYRW, _S_IREAD | _S_IWRITE) != 0)
			throw std::runtime_error("first execution fault file is not new");
#else
		Handle = open(Path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (Handle < 0) throw std::runtime_error("first execution fault file is not new");
#endif
		if (!WriteAll(std::as_bytes(std::span(Header.data(), Header.size()))) || !Flush()) {
			Close();
			throw std::runtime_error("first execution fault header persistence failed");
		}
	}
	~FarmD03FirstFault() { Close(); }
	FarmD03FirstFault(const FarmD03FirstFault &) = delete;
	FarmD03FirstFault &operator=(const FarmD03FirstFault &) = delete;
	void Close() noexcept {
		if (Handle < 0) return;
#if defined(_WIN32)
		_close(Handle);
#else
		close(Handle);
#endif
		Handle = -1;
	}
	// At is an already sampled host steady timestamp. This method reads no clock.
	void MarkStage(Stage Value, std::uint64_t Tick, std::uint64_t At) noexcept {
		if (Captured || static_cast<std::size_t>(Value) >= StageCount) return;
		if (StageStarted) {
			LastStage = CurrentStage; LastStageTick = HostTick;
			LastStageStarted = StageStarted; LastStageCompleted = At;
			CompletedStages[static_cast<std::size_t>(CurrentStage)] = {HostTick, StageStarted, At};
		}
		if (Tick != HostTick) TickStarted = At;
		if (Value == Stage::Sleep) {
			LastIterationTick = Tick; LastIterationStarted = TickStarted; LastIterationCompleted = At;
		}
		CurrentStage = Value; HostTick = Tick; StageStarted = At;
	}
	// The host supplies the existing recorder's fault-only Clock::Capture value.
	// A failed/unsupported native clock stays explicitly unmeasured.
	void SetClock(Correlation Value) noexcept { if (!Captured) Clock = Value; }
	bool Persist(const network::detail::PooledServiceRecord &Value) noexcept {
		if (Captured) return Persisted;
		if (Value.Point == network::detail::PooledObservationPoint::PeerTeardown ||
			!(Value.ExecutionRejected || Value.Result.ExecutionFailed || Value.InvalidAfter)) return true;
		Captured = true; // A later failure, teardown or generation can never replace this owner.
		auto Put = [&](Word Field, std::uint64_t Number) { First.Words[static_cast<std::size_t>(Field)] = Number; };
		const auto &B = Value.ExecutionBefore;
		Put(Word::Version, 1); Put(Word::ConnectionSlot, Value.Connection.Slot);
		Put(Word::ConnectionGeneration, Value.Connection.Generation); Put(Word::SimulationTick, Value.SimulationTick);
		Put(Word::Now, Value.NowMicroseconds); Put(Word::Point, static_cast<unsigned>(Value.Point));
		Put(Word::Rejected, Value.ExecutionRejected); Put(Word::InvalidAfter, Value.InvalidAfter);
		Put(Word::ResultValid, Value.Result.Valid); Put(Word::ExecutionFailed, Value.Result.ExecutionFailed);
		Put(Word::MainFailed, Value.Result.MainOpportunityFailed); Put(Word::TurnoverFailed, Value.Result.TurnoverFailed);
		Put(Word::WorkReady, Value.WorkReady); Put(Word::AcceptanceAvailable, Value.AcceptanceAvailable);
		Put(Word::DiagnosticOnly, Value.DiagnosticOnly); Put(Word::BeforeCaptured, B.Captured);
		Put(Word::BeforeInvalid, B.Invalid); Put(Word::Armed, B.Armed); Put(Word::HasBoundary, B.HasBoundary);
		Put(Word::RequiredSince, B.RequiredSince); Put(Word::Boundary, B.Boundary);
		Put(Word::OfferToken, B.OfferToken); Put(Word::OfferBytes, B.OfferBytes); Put(Word::OfferAt, B.OfferAt);
		Put(Word::TurnoverToken, B.TurnoverToken); Put(Word::TurnoverAt, B.TurnoverAt);
		Put(Word::PermitAvailable, B.PermitAvailable); Put(Word::DebtToken, Value.DebtToken); Put(Word::DebtBytes, Value.DebtBytes);
		Put(Word::AcceptedAll, Value.Accepted.All); Put(Word::AcceptedStructural, Value.Accepted.Structural);
		Put(Word::AcceptedGameplay, Value.Accepted.Gameplay);
		auto Elapsed = [&](bool Active, std::uint64_t Started, Word Valid, Word Duration, Word Backwards) {
			if (!B.Captured || !Active) return;
			Put(Backwards, Value.NowMicroseconds < Started);
			if (Value.NowMicroseconds >= Started) { Put(Valid, 1); Put(Duration, Value.NowMicroseconds - Started); }
		};
		Elapsed(B.Armed, B.RequiredSince, Word::MainElapsedValid, Word::MainElapsed, Word::MainBackwards);
		Elapsed(B.OfferToken && B.OfferAt != std::numeric_limits<std::uint64_t>::max(), B.OfferAt,
			Word::OfferElapsedValid, Word::OfferElapsed, Word::OfferBackwards);
		Elapsed(B.TurnoverToken != 0, B.TurnoverAt, Word::TurnoverElapsedValid, Word::TurnoverElapsed, Word::TurnoverBackwards);
		Put(Word::BoundaryBackwards, B.Captured && B.HasBoundary && Value.NowMicroseconds < B.Boundary);
		Put(Word::MaximumMain, Value.Result.MaximumMainOpportunityMicroseconds);
		Put(Word::MaximumTurnover, Value.Result.MaximumTurnoverMicroseconds);
		Put(Word::RetiredToken, Value.Result.RetiredToken); Put(Word::RetiredBytes, Value.Result.RetiredBytes);
		Put(Word::LastCompletedTurnoverToken, Value.Result.LastCompletedTurnoverToken);
		Put(Word::LastCompletedTurnoverActivated, Value.Result.LastCompletedTurnoverActivatedAtMicroseconds);
		Put(Word::LastCompletedTurnoverAt, Value.Result.LastCompletedTurnoverAtMicroseconds);
		Put(Word::HostStage, static_cast<unsigned>(CurrentStage)); Put(Word::HostTick, HostTick);
		Put(Word::HostStageStarted, StageStarted); Put(Word::HostTickStarted, TickStarted);
		Put(Word::LastHostStage, static_cast<unsigned>(LastStage)); Put(Word::LastHostStageTick, LastStageTick);
		Put(Word::LastHostStageStarted, LastStageStarted); Put(Word::LastHostStageCompleted, LastStageCompleted);
		Put(Word::LastIterationTick, LastIterationTick); Put(Word::LastIterationStarted, LastIterationStarted);
		Put(Word::LastIterationCompleted, LastIterationCompleted);
		if (Value.Feedback) {
			Put(Word::FeedbackPresent, 1); Put(Word::NativeObservedAt, Value.Feedback->ObservedAtMicroseconds);
			Put(Word::NativeState, static_cast<unsigned>(Value.Feedback->State));
			Put(Word::NativeCountersValid, Value.Feedback->CountersValid);
		}
		Put(Word::NativeClockValid, Clock.Valid && Clock.Process && Clock.Thread && Clock.SteadyNs &&
			Clock.Frequency && Clock.QpcAfter >= Clock.QpcBefore);
		Put(Word::Process, Clock.Process); Put(Word::NativeThread, Clock.Thread);
		Put(Word::ClockSteadyNanoseconds, Clock.SteadyNs); Put(Word::QpcBefore, Clock.QpcBefore);
		Put(Word::QpcAfter, Clock.QpcAfter); Put(Word::QpcFrequency, Clock.Frequency);
		for (std::size_t Index = 0; Index < CompletedStages.size(); ++Index) {
			const auto Base = static_cast<std::size_t>(Word::CompletedStagesStart) + Index * 4;
			First.Words[Base] = Index;
			First.Words[Base + 1] = CompletedStages[Index].Tick;
			First.Words[Base + 2] = CompletedStages[Index].Started;
			First.Words[Base + 3] = CompletedStages[Index].Completed;
		}
		std::array<std::byte, RecordBytes> Bytes{};
		for (std::size_t Index = 0; Index < First.Words.size(); ++Index)
			for (unsigned Shift = 0; Shift < 8; ++Shift)
				Bytes[Index * 8 + Shift] = static_cast<std::byte>(First.Words[Index] >> (Shift * 8));
		Persisted = WriteAll(Bytes) && Flush();
		WriteFailed = !Persisted;
		return Persisted;
	}
	static bool Record(void *Context, const network::detail::PooledServiceRecord &Value) noexcept {
		return static_cast<FarmD03FirstFault *>(Context)->Persist(Value);
	}
	bool HasFault() const noexcept { return Captured; }
	bool Failed() const noexcept { return WriteFailed; }
	bool FaultPersisted() const noexcept { return Persisted; }
	const Snapshot &FirstFault() const noexcept { return First; }
};
}
