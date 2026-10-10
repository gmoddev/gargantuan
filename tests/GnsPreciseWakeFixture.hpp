#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

// Only the OS/clock boundaries are supplied. The two .inc bodies are extracted
// from the actual applied pinned socketthread.cpp by ApplyPreciseSenderWake.
namespace GnsPreciseWakeFixture {
using SteamNetworkingMicroseconds = std::int64_t;
using HANDLE = void *;
using DWORD = std::uint32_t;
using BOOL = int;
#ifndef WAIT_OBJECT_0
constexpr DWORD WAIT_OBJECT_0 = 0;
#endif
#ifndef WAIT_TIMEOUT
constexpr DWORD WAIT_TIMEOUT = 258;
#endif
#ifndef WAIT_FAILED
constexpr DWORD WAIT_FAILED = 0xffffffff;
#endif
#ifndef FALSE
constexpr BOOL FALSE = 0;
#endif
constexpr SteamNetworkingMicroseconds k_nThinkTime_Never = std::numeric_limits<std::int64_t>::max();
struct LARGE_INTEGER { std::int64_t QuadPart; };
struct epoll_event { int Unused; };
struct pollfd { int fd, events, revents; };
struct timespec { std::int64_t tv_sec, tv_nsec; };
#ifndef POLLIN
constexpr int POLLIN = 1;
#endif
constexpr int s_epollfd = 9;
#ifndef V_ARRAYSIZE
template<class T, std::size_t N> constexpr std::size_t V_ARRAYSIZE(const T (&)[N]) { return N; }
#endif
inline HANDLE s_hEventWakeThread = reinterpret_cast<HANDLE>(1);
inline HANDLE s_hGargantuanPreciseWakeTimer = reinterpret_cast<HANDLE>(2);
struct State {
	std::vector<SteamNetworkingMicroseconds> Clocks{100, 150, 160, 210};
	std::size_t Reads = 0;
	SteamNetworkingMicroseconds Last = 0, Due = 0;
	bool Running = true, SetSucceeded = true;
	DWORD MultiResult = WAIT_OBJECT_0 + 1, SingleResult = WAIT_TIMEOUT;
	int Fallbacks = 0, Timeout = -1, EventPolls = 0, Sets = 0, Cancels = 0, Ppolls = 0;
	std::uint64_t Detail = 0;
	std::uint32_t Flags = 0;
};
inline State *Current = nullptr;
inline SteamNetworkingMicroseconds SteamNetworkingSockets_GetLocalTimestamp() {
	if (Current->Reads >= Current->Clocks.size()) throw std::runtime_error("bounded wake fixture clock exhausted");
	return Current->Last = Current->Clocks[Current->Reads++];
}
inline DWORD WaitForSingleObject(HANDLE, int Timeout) {
	if (!Timeout) { ++Current->EventPolls; return Current->SingleResult; }
	++Current->Fallbacks; Current->Timeout = Timeout; return WAIT_TIMEOUT;
}
inline BOOL SetWaitableTimer(HANDLE, const LARGE_INTEGER *Due, int, void *, void *, BOOL) {
	++Current->Sets; Current->Due = Due->QuadPart; return Current->SetSucceeded;
}
#ifndef INFINITE
constexpr DWORD INFINITE = 0xffffffff;
#endif
inline DWORD WaitForMultipleObjects(int, HANDLE *, BOOL, DWORD) { return Current->MultiResult; }
inline BOOL CancelWaitableTimer(HANDLE) { ++Current->Cancels; return 1; }
inline DWORD GetLastError() { return 6; }
inline int epoll_wait(int, epoll_event *, std::size_t, int Timeout) {
	if (Timeout) { ++Current->Fallbacks; Current->Timeout = Timeout; }
	else ++Current->EventPolls;
	return 0;
}
inline int ppoll(pollfd *, int, const timespec *Timeout, void *) {
	++Current->Ppolls; Current->Due = Timeout->tv_sec * 1000000 + Timeout->tv_nsec / 1000; return 0;
}
namespace SteamNetworkingSocketsLib {
enum class GargantuanServiceTimingPhase { TimerWait };
inline bool GargantuanHasRunningStructuralGrant() { return Current->Running; }
struct GargantuanServiceTimingSpan {
	explicit GargantuanServiceTimingSpan(GargantuanServiceTimingPhase) {}
	void SetCount(int) {}
	void SetDetail(std::uint64_t Value) { Current->Detail = Value; }
	void SetBytes(std::int64_t) {}
	void AddFlags(std::uint32_t Value) { Current->Flags |= Value; }
	void SetResult(DWORD) {}
	bool Enabled() { return true; }
	void SetError(DWORD) {}
	void Finish() {}
};
}
inline void Windows(int nMaxTimeoutMS, bool bManualPoll, SteamNetworkingMicroseconds usecExactWake) {
#include "gns-wake-windows.inc"
}
inline void Linux(int nMaxTimeoutMS, bool bManualPoll, SteamNetworkingMicroseconds usecExactWake) {
#include "gns-wake-linux.inc"
}
inline bool Run() {
	bool Passed = true;
	const auto Call = [](bool LinuxCase, State &Value, bool Timer, int Maximum, bool Manual,
		SteamNetworkingMicroseconds Deadline) {
		Current = &Value;
		s_hGargantuanPreciseWakeTimer = Timer ? reinterpret_cast<HANDLE>(2) : nullptr;
		if (LinuxCase) Linux(Maximum, Manual, Deadline); else Windows(Maximum, Manual, Deadline);
	};
	for (const bool LinuxCase : {false, true}) {
		State Shift;
		Call(LinuxCase, Shift, true, 1, false, 160);
		std::fprintf(stderr, "[Gns:PreciseWake] platform=%s shifted_end=%lld expected=160\n",
			LinuxCase ? "Linux" : "Windows", static_cast<long long>(Shift.Last));
		Passed = Shift.Last == 160 && Shift.Fallbacks == 0 && Passed;
		State Clamp; Clamp.Clocks = {100, 600, 1100, 1600};
		Call(LinuxCase, Clamp, true, 1, false, 5000);
		Passed = Clamp.Last == 1100 && Clamp.Fallbacks == 0 && Passed;
		for (int Case = 0; Case < 4; ++Case) {
			State Bypass; Bypass.Running = Case != 3;
			Call(LinuxCase, Bypass, true, Case == 1 ? 0 : 1, Case == 0,
				Case == 2 ? k_nThinkTime_Never : 160);
			Passed = Bypass.Reads == 0 && Bypass.Sets == 0 && Bypass.Ppolls == 0 && Passed;
			if (Case != 1) Passed = Bypass.Fallbacks == 1 && Bypass.Timeout == 1 && Passed;
		}
		State AlreadyDue;
		Call(LinuxCase, AlreadyDue, true, 1, false, 99);
		Passed = AlreadyDue.Reads == 1 && AlreadyDue.Fallbacks == 0 && Passed;
	}
	State NoTimer;
	Call(false, NoTimer, false, 1, false, 160);
	std::fprintf(stderr, "[Gns:PreciseWake] null_short_end=%lld fallback=%d expected=160/0\n",
		static_cast<long long>(NoTimer.Last), NoTimer.Fallbacks);
	Passed = NoTimer.Last == 160 && NoTimer.Fallbacks == 0 && NoTimer.Sets == 0 && NoTimer.Flags == 0 && Passed;
	State LongNull;
	Call(false, LongNull, false, 5, false, 2100);
	Passed = LongNull.Reads == 1 && LongNull.Sets == 0 && LongNull.Fallbacks == 1 && LongNull.Timeout == 5 && Passed;
	State Timer; Timer.Clocks = {100, 1100, 2100, 3100};
	Call(false, Timer, true, 5, false, 2100);
	Passed = Timer.Sets == 1 && Timer.Due == -10000 && Timer.Cancels == 1 && Timer.Last == 2100 && Timer.Fallbacks == 0 && Passed;
	State Event; Event.MultiResult = WAIT_OBJECT_0;
	Call(false, Event, true, 5, false, 2100);
	Passed = Event.Sets == 1 && Event.Cancels == 1 && Event.Reads == 1 && Event.Fallbacks == 0 && Passed;
	State SpinEvent; SpinEvent.SingleResult = WAIT_OBJECT_0;
	Call(false, SpinEvent, false, 1, false, 160);
	Passed = SpinEvent.Reads == 2 && SpinEvent.EventPolls == 1 && SpinEvent.Fallbacks == 0 && Passed;
	State SetFailed; SetFailed.SetSucceeded = false;
	Call(false, SetFailed, true, 5, false, 2100);
	Passed = SetFailed.Sets == 1 && SetFailed.Cancels == 0 && SetFailed.Fallbacks == 1 && Passed;
	State WaitFailed; WaitFailed.MultiResult = WAIT_FAILED;
	Call(false, WaitFailed, true, 5, false, 2100);
	Passed = WaitFailed.Sets == 1 && WaitFailed.Cancels == 1 && WaitFailed.Fallbacks == 1 && Passed;
	Current = nullptr;
	std::fprintf(stderr, "[Gns:PreciseWake] SourceExtractedDeadlineNullTimerFallbackRoutes=%s NO_NATIVE_IO\n", Passed ? "PASS" : "FAIL");
	return Passed;
}
} // namespace GnsPreciseWakeFixture

inline bool TestGnsPreciseWake() {
	try { return GnsPreciseWakeFixture::Run(); }
	catch (const std::exception &Error) {
		std::fprintf(stderr, "[Gns:PreciseWake] FAIL %s\n", Error.what());
		return false;
	}
}
