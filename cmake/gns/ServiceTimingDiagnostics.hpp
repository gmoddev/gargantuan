#pragma once

#include <atomic>
#include <cstdint>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>
#include <mmsystem.h>
#endif

namespace SteamNetworkingSocketsLib {
// Private, opt-in observation. Fixture-owned bounded storage is installed before
// GNS initialization and removed after all pairs are destroyed. No service policy.
enum class GargantuanServiceTimingPhase : std::uint8_t {
	TimerCreate = 1, TimerWait, GlobalLockWait, ReceiveDrain, Thinkers, ThinkerCallback, SnpSender
};
struct GargantuanServiceTimingRecord {
	GargantuanServiceTimingPhase Phase;
	bool ClockValid;
	std::uint32_t ThreadId;
	std::uint64_t BeginQpc, EndQpc;
	std::uint64_t Count, Bytes, Detail;
	std::int64_t Result;
	std::uint32_t Error, Flags;
};
struct GargantuanServiceTimingSink {
	void *Context = nullptr;
	void (*Record)(void *, const GargantuanServiceTimingRecord &) noexcept = nullptr;
	bool DetailedCallbacks = false;
};
class GargantuanServiceTimingSpan;
inline thread_local GargantuanServiceTimingSpan *GargantuanCurrentSnpTiming = nullptr;
inline std::atomic<GargantuanServiceTimingSink *> GargantuanServiceTimingObserver{nullptr};
inline bool GargantuanSetServiceTimingSink(GargantuanServiceTimingSink *Sink) noexcept {
	if (!Sink) {
		GargantuanServiceTimingObserver.store(nullptr, std::memory_order_release);
		return true;
	}
	if (!Sink->Context || !Sink->Record) return false;
	GargantuanServiceTimingSink *Empty = nullptr;
	return GargantuanServiceTimingObserver.compare_exchange_strong(Empty, Sink,
		std::memory_order_acq_rel, std::memory_order_acquire);
}
class GargantuanServiceTimingSpan final {
	GargantuanServiceTimingSink *Sink = nullptr;
	GargantuanServiceTimingSpan *PreviousSnpTiming = nullptr;
	GargantuanServiceTimingRecord Value;
public:
	explicit GargantuanServiceTimingSpan(GargantuanServiceTimingPhase Phase) noexcept
		: Sink(GargantuanServiceTimingObserver.load(std::memory_order_acquire)) {
		if (Sink && (Phase == GargantuanServiceTimingPhase::ThinkerCallback || Phase == GargantuanServiceTimingPhase::SnpSender) &&
			!Sink->DetailedCallbacks) Sink = nullptr;
		if (!Sink) return; // Disabled: no clocks, counting, allocation or output.
		Value = {};
		Value.Phase = Phase;
		if (Phase == GargantuanServiceTimingPhase::SnpSender) {
			PreviousSnpTiming = GargantuanCurrentSnpTiming;
			GargantuanCurrentSnpTiming = this;
		}
#if defined(_WIN32)
		LARGE_INTEGER Now;
		Value.ThreadId = GetCurrentThreadId();
		Value.ClockValid = QueryPerformanceCounter(&Now) && Now.QuadPart >= 0;
		if (Value.ClockValid) Value.BeginQpc = static_cast<std::uint64_t>(Now.QuadPart);
#endif
	}
	GargantuanServiceTimingSpan(const GargantuanServiceTimingSpan &) = delete;
	GargantuanServiceTimingSpan &operator=(const GargantuanServiceTimingSpan &) = delete;
	~GargantuanServiceTimingSpan() { Finish(); }
	bool Enabled() const noexcept { return Sink != nullptr; }
	bool Detailed() const noexcept { return Sink && Sink->DetailedCallbacks; }
	bool PacketOwner(std::uint32_t Handle) const noexcept { return Sink && Value.Result == static_cast<std::int64_t>(Handle); }
	void AddCount() noexcept { if (Sink) ++Value.Count; }
	void AddBytes(std::uint64_t Bytes) noexcept { if (Sink) Value.Bytes += Bytes; }
	void AddDetail() noexcept { if (Sink) ++Value.Detail; }
	void SetCount(std::uint64_t Count) noexcept { if (Sink) Value.Count = Count; }
	void SetBytes(std::uint64_t Bytes) noexcept { if (Sink) Value.Bytes = Bytes; }
	void SetDetail(std::uint64_t Detail) noexcept { if (Sink) Value.Detail = Detail; }
	void SetResult(std::int64_t Result) noexcept { if (Sink) Value.Result = Result; }
	void SetError(std::uint32_t Error) noexcept { if (Sink) Value.Error = Error; }
	void AddFlags(std::uint32_t Flags) noexcept { if (Sink) Value.Flags |= Flags; }
	void Finish() noexcept {
		if (!Sink) return;
		if (Value.Phase == GargantuanServiceTimingPhase::SnpSender)
			GargantuanCurrentSnpTiming = PreviousSnpTiming;
#if defined(_WIN32)
		LARGE_INTEGER Now;
		const bool EndValid = QueryPerformanceCounter(&Now) && Now.QuadPart >= 0;
		if (EndValid) Value.EndQpc = static_cast<std::uint64_t>(Now.QuadPart);
		Value.ClockValid = Value.ClockValid && EndValid && Value.EndQpc >= Value.BeginQpc;
#endif
		auto *Observer = Sink;
		Sink = nullptr;
		Observer->Record(Observer->Context, Value);
	}
};
// Count only the preexisting successful native packet hook, on this callback's
// thread. The borrowed stack span is restored on every return/unwind; no owner
// pointer, registration, allocation or clock is introduced in the packet path.
inline void GargantuanServiceTimingPacket(std::uint64_t Bytes, std::uint32_t Handle) noexcept {
	if (!GargantuanCurrentSnpTiming || !GargantuanCurrentSnpTiming->PacketOwner(Handle)) return;
	GargantuanCurrentSnpTiming->AddCount();
	GargantuanCurrentSnpTiming->AddBytes(Bytes);
}
}
