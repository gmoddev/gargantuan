# The pinned sender rounds paced thinker deadlines to whole milliseconds. F1's
# intra-grant drain needs the remaining sub-millisecond deadline while a real
# structural grant is partway through native first-send. Patch only that case.
set(GargantuanWakePath
	"${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_socketthread.cpp")
file(READ "${GargantuanWakePath}" GargantuanWakeCurrent)
string(REPLACE "\r\n" "\n" GargantuanWakeCurrent "${GargantuanWakeCurrent}")
if(EXISTS "${GargantuanWakePath}.gargantuan-wake-original")
	file(READ "${GargantuanWakePath}.gargantuan-wake-original" GargantuanWakeOriginal)
else()
	set(GargantuanWakeOriginal "${GargantuanWakeCurrent}")
endif()
string(SHA256 GargantuanWakeHash "${GargantuanWakeOriginal}")
if(NOT GargantuanWakeHash STREQUAL "1f6ac27b1313c523a41dee94f4c882e30cff819f4e85ffd443101b12676d90a6")
	message(FATAL_ERROR "Pinned GNS sender-wake input mismatch")
endif()
if(NOT GargantuanWakeCurrent STREQUAL GargantuanWakeOriginal)
	if(NOT EXISTS "${GargantuanWakePath}.gargantuan-wake-applied")
		message(FATAL_ERROR "Unknown GNS sender-wake modification")
	endif()
	file(READ "${GargantuanWakePath}.gargantuan-wake-applied" GargantuanWakePrevious)
	if(NOT GargantuanWakeCurrent STREQUAL GargantuanWakePrevious)
		message(FATAL_ERROR "Concurrent GNS sender-wake modification")
	endif()
endif()
set(GargantuanWakeSource "${GargantuanWakeOriginal}")
macro(GargantuanReplaceWake Old New)
	string(FIND "${GargantuanWakeSource}" "${Old}" Position)
	if(Position EQUAL -1)
		message(FATAL_ERROR "Pinned GNS sender-wake transition missing: ${Old}")
	endif()
	string(REPLACE "${Old}" "${New}" GargantuanWakeSource "${GargantuanWakeSource}")
endmacro()

GargantuanReplaceWake("#include <atomic>" "#include <atomic>\n#include \"ReliableServiceFeedback.hpp\"\n#include \"ServiceTimingDiagnostics.hpp\"")
GargantuanReplaceWake("static std::thread *s_pServiceThread = nullptr;" [=[
#if defined( _WIN32 )
static HANDLE s_hGargantuanPreciseWakeTimer = nullptr;
#endif
static std::thread *s_pServiceThread = nullptr;]=])
GargantuanReplaceWake(
	"static bool PollRawUDPSockets( int nMaxTimeoutMS, bool bManualPoll )"
	"static bool PollRawUDPSockets( int nMaxTimeoutMS, bool bManualPoll, SteamNetworkingMicroseconds usecExactWake )")
GargantuanReplaceWake([=[
	#if defined( USE_EPOLL )
		struct epoll_event epoll_events[ 32 ];
		int num_epoll_events = epoll_wait( s_epollfd, epoll_events, V_ARRAYSIZE( epoll_events ), nMaxTimeoutMS );
	#elif defined( USE_POLL )
		int poll_result = poll( s_vecPollFDs.Base(), s_vecPollFDs.Count(), nMaxTimeoutMS );
	#elif defined( _WIN32 )
		WaitForSingleObject( s_hEventWakeThread, nMaxTimeoutMS );
	#else
		#error "How do?"
	#endif]=] [=[
	#if defined( USE_EPOLL )
		struct epoll_event epoll_events[ 32 ];
		int num_epoll_events = 0;
		#if IsLinux()
		if ( !bManualPoll && nMaxTimeoutMS > 0 &&
			SteamNetworkingSocketsLib::GargantuanHasRunningStructuralGrant() &&
			usecExactWake < k_nThinkTime_Never )
		{
			const SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
			SteamNetworkingMicroseconds usecRemaining = usecExactWake - usecNow;
			usecRemaining = std::min( usecRemaining, (SteamNetworkingMicroseconds)nMaxTimeoutMS * 1000 );
			if ( usecRemaining > 0 )
			{
				const SteamNetworkingMicroseconds usecTarget = usecNow + usecRemaining;
				bool bSpin = usecRemaining <= 1000;
				if ( usecRemaining > 1000 )
				{
					struct pollfd fd = { s_epollfd, POLLIN, 0 };
					const SteamNetworkingMicroseconds usecEarly = usecRemaining - 1000;
					struct timespec timeout = { usecEarly / 1000000, ( usecEarly % 1000000 ) * 1000 };
					bSpin = ppoll( &fd, 1, &timeout, nullptr ) == 0;
				}
				while ( bSpin && SteamNetworkingSockets_GetLocalTimestamp() < usecTarget )
				{
					num_epoll_events = epoll_wait( s_epollfd, epoll_events, V_ARRAYSIZE( epoll_events ), 0 );
					if ( num_epoll_events != 0 )
						break;
					std::atomic_signal_fence( std::memory_order_seq_cst );
				}
			}
			if ( num_epoll_events == 0 )
				num_epoll_events = epoll_wait( s_epollfd, epoll_events, V_ARRAYSIZE( epoll_events ), 0 );
		}
		else
			num_epoll_events = epoll_wait( s_epollfd, epoll_events, V_ARRAYSIZE( epoll_events ), nMaxTimeoutMS );
		#else
			num_epoll_events = epoll_wait( s_epollfd, epoll_events, V_ARRAYSIZE( epoll_events ), nMaxTimeoutMS );
		#endif
	#elif defined( USE_POLL )
		int poll_result = poll( s_vecPollFDs.Base(), s_vecPollFDs.Count(), nMaxTimeoutMS );
	#elif defined( _WIN32 )
		bool bWaited = false;
		SteamNetworkingSocketsLib::GargantuanServiceTimingSpan GargantuanWait(
			SteamNetworkingSocketsLib::GargantuanServiceTimingPhase::TimerWait);
		GargantuanWait.SetCount(nMaxTimeoutMS);
		GargantuanWait.SetDetail(3); // Integer-ms fallback unless precise path succeeds.
		if ( !bManualPoll && nMaxTimeoutMS > 0 &&
			SteamNetworkingSocketsLib::GargantuanHasRunningStructuralGrant() &&
			usecExactWake < k_nThinkTime_Never )
		{
			if (s_hGargantuanPreciseWakeTimer != nullptr)
				GargantuanWait.AddFlags(1); // Preserve the timer-available diagnostic bit.
			const SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
			SteamNetworkingMicroseconds usecRemaining = usecExactWake - usecNow;
			if ( usecRemaining <= 0 )
			{
				bWaited = true;
				GargantuanWait.SetDetail(0); // Already due, no wait.
			}
			else
			{
				usecRemaining = std::min( usecRemaining, (SteamNetworkingMicroseconds)nMaxTimeoutMS * 1000 );
				const SteamNetworkingMicroseconds usecTarget = usecNow + usecRemaining;
				bool bSpin = usecRemaining <= 1000;
				GargantuanWait.SetBytes(usecRemaining); // Requested remaining microseconds.
				if (bSpin) GargantuanWait.SetDetail(1);
				if ( usecRemaining > 1000 && s_hGargantuanPreciseWakeTimer != nullptr )
				{
					LARGE_INTEGER due;
					due.QuadPart = -10 * ( usecRemaining - 1000 );
					GargantuanWait.AddFlags(2); // Set attempted.
					const BOOL bGargantuanTimerSet = SetWaitableTimer( s_hGargantuanPreciseWakeTimer, &due, 0, nullptr, nullptr, FALSE );
					if ( bGargantuanTimerSet )
					{
						GargantuanWait.AddFlags(4); // Set succeeded.
						GargantuanWait.SetDetail(2);
						HANDLE waits[2] = { s_hEventWakeThread, s_hGargantuanPreciseWakeTimer };
						const DWORD result = WaitForMultipleObjects( 2, waits, FALSE, INFINITE );
						GargantuanWait.SetResult(result);
						if (result == WAIT_FAILED && GargantuanWait.Enabled()) GargantuanWait.SetError(GetLastError());
						CancelWaitableTimer( s_hGargantuanPreciseWakeTimer );
						if ( result == WAIT_OBJECT_0 )
							bWaited = true;
						bSpin = result == WAIT_OBJECT_0 + 1;
					}
					else if (GargantuanWait.Enabled()) GargantuanWait.SetError(GetLastError());
				}
				if ( bSpin )
				{
					while ( SteamNetworkingSockets_GetLocalTimestamp() < usecTarget )
					{
						if ( WaitForSingleObject( s_hEventWakeThread, 0 ) == WAIT_OBJECT_0 )
							break;
						std::atomic_signal_fence( std::memory_order_seq_cst );
					}
					bWaited = true;
				}
			}
		}
		if ( !bWaited )
		{
			GargantuanWait.SetDetail(3);
			const DWORD result = WaitForSingleObject( s_hEventWakeThread, nMaxTimeoutMS );
			GargantuanWait.SetResult(result);
			if (result == WAIT_FAILED && GargantuanWait.Enabled()) GargantuanWait.SetError(GetLastError());
		}
		GargantuanWait.Finish();
	#else
		#error "How do?"
	#endif]=])
GargantuanReplaceWake("PollRawUDPSockets( msWait, bManualPoll )"
	"PollRawUDPSockets( msWait, bManualPoll, usecNextWakeTime )")
GargantuanReplaceWake([=[
			// Note: Using "automatic reset" style event.
			s_hEventWakeThread = CreateEvent( nullptr, false, false, nullptr );
			if ( s_hEventWakeThread == NULL || s_hEventWakeThread == INVALID_HANDLE_VALUE )
			{
				s_hEventWakeThread = INVALID_HANDLE_VALUE;
				V_sprintf_safe( errMsg, "CreateEvent() call failed.  Error code 0x%08x.", GetLastError() );
				return false;
			}]=] [=[
			// Note: Using "automatic reset" style event.
			s_hEventWakeThread = CreateEvent( nullptr, false, false, nullptr );
			if ( s_hEventWakeThread == NULL || s_hEventWakeThread == INVALID_HANDLE_VALUE )
			{
				s_hEventWakeThread = INVALID_HANDLE_VALUE;
				V_sprintf_safe( errMsg, "CreateEvent() call failed.  Error code 0x%08x.", GetLastError() );
				return false;
			}
			// The timer is used only while a structural grant has unsent native bytes.
			SteamNetworkingSocketsLib::GargantuanServiceTimingSpan GargantuanTimerCreate(
				SteamNetworkingSocketsLib::GargantuanServiceTimingPhase::TimerCreate);
			s_hGargantuanPreciseWakeTimer = CreateWaitableTimerExW( nullptr, nullptr,
				CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE );
			GargantuanTimerCreate.SetResult(s_hGargantuanPreciseWakeTimer != nullptr);
			if (s_hGargantuanPreciseWakeTimer == nullptr && GargantuanTimerCreate.Enabled())
				GargantuanTimerCreate.SetError(GetLastError());
			GargantuanTimerCreate.Finish();]=])
GargantuanReplaceWake([=[
		if ( s_hEventWakeThread != INVALID_HANDLE_VALUE )
		{
			CloseHandle( s_hEventWakeThread );]=] [=[
		if ( s_hGargantuanPreciseWakeTimer != nullptr )
		{
			CloseHandle( s_hGargantuanPreciseWakeTimer );
			s_hGargantuanPreciseWakeTimer = nullptr;
		}
		if ( s_hEventWakeThread != INVALID_HANDLE_VALUE )
		{
			CloseHandle( s_hEventWakeThread );]=])

# Aggregate phase spans observe the existing loops without changing their policy.
GargantuanReplaceWake("static bool DrainSocket( CRawUDPSocketImpl *pSock )\n{" [=[static bool DrainSocket( CRawUDPSocketImpl *pSock )
{
	SteamNetworkingSocketsLib::GargantuanServiceTimingSpan GargantuanDrain(
		SteamNetworkingSocketsLib::GargantuanServiceTimingPhase::ReceiveDrain);]=])
GargantuanReplaceWake("\t\tif ( ret < 0 )\n\t\t\tbreak;\n\n\t\t// Emit ETW event" [=[		if ( ret < 0 )
			break;

		GargantuanDrain.AddCount();
		GargantuanDrain.AddBytes(iov_buf.iov_len);

		// Emit ETW event]=])
GargantuanReplaceWake("\t// We're back awake.  Grab the lock again" [=[	SteamNetworkingSocketsLib::GargantuanServiceTimingSpan GargantuanLockWait(
		SteamNetworkingSocketsLib::GargantuanServiceTimingPhase::GlobalLockWait);
	// We're back awake.  Grab the lock again]=])
GargantuanReplaceWake("\t\tif ( SteamNetworkingGlobalLock::TryLock( \"ServiceThread\", 20 ) )" [=[		GargantuanLockWait.AddCount();
		if ( SteamNetworkingGlobalLock::TryLock( "ServiceThread", 20 ) )]=])
GargantuanReplaceWake("\t// If we waited a long time, then that's probably bad.  Spew about it" [=[	GargantuanLockWait.Finish();
	// If we waited a long time, then that's probably bad.  Spew about it]=])

if(NOT EXISTS "${GargantuanWakePath}.gargantuan-wake-original")
	file(WRITE "${GargantuanWakePath}.gargantuan-wake-original" "${GargantuanWakeOriginal}")
endif()
if(NOT GargantuanWakeCurrent STREQUAL GargantuanWakeSource)
	file(WRITE "${GargantuanWakePath}" "${GargantuanWakeSource}")
	file(WRITE "${GargantuanWakePath}.gargantuan-wake-applied" "${GargantuanWakeSource}")
endif()

# The existing fairness target tests these exact applied branches under bounded
# OS/clock stubs. No copied pacing model or production clock seam is involved.
string(FIND "${GargantuanWakeSource}" "\t\tbool bWaited = false;" WakeWindowsBegin)
string(FIND "${GargantuanWakeSource}" "\t\tGargantuanWait.Finish();" WakeWindowsEnd)
if(WakeWindowsBegin LESS 0 OR WakeWindowsEnd LESS WakeWindowsBegin)
	message(FATAL_ERROR "Pinned Windows wake test branch missing")
endif()
string(LENGTH "\t\tGargantuanWait.Finish();" WakeWindowsTail)
math(EXPR WakeWindowsLength "${WakeWindowsEnd} - ${WakeWindowsBegin} + ${WakeWindowsTail}")
string(SUBSTRING "${GargantuanWakeSource}" ${WakeWindowsBegin} ${WakeWindowsLength} WakeWindowsBody)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gns-wake-windows.inc" CONTENT "${WakeWindowsBody}" @ONLY)
string(FIND "${GargantuanWakeSource}" "\t\tstruct epoll_event epoll_events[ 32 ];" WakeLinuxBegin)
if(WakeLinuxBegin LESS 0)
	message(FATAL_ERROR "Pinned Linux wake test branch missing")
endif()
string(SUBSTRING "${GargantuanWakeSource}" ${WakeLinuxBegin} -1 WakeLinuxRemainder)
string(FIND "${WakeLinuxRemainder}" "\t\t#if IsLinux()\n" WakeLinuxIf)
if(WakeLinuxIf LESS 0)
	message(FATAL_ERROR "Pinned Linux wake test branch missing")
endif()
string(LENGTH "\t\t#if IsLinux()\n" WakeLinuxIfLength)
math(EXPR WakeLinuxContentBegin "${WakeLinuxIf} + ${WakeLinuxIfLength}")
string(SUBSTRING "${WakeLinuxRemainder}" ${WakeLinuxContentBegin} -1 WakeLinuxBodyRemainder)
string(FIND "${WakeLinuxBodyRemainder}" "\t\t#else" WakeLinuxLength)
if(WakeLinuxLength LESS 0)
	message(FATAL_ERROR "Pinned Linux wake test branch missing")
endif()
string(SUBSTRING "${WakeLinuxRemainder}" 0 ${WakeLinuxIf} WakeLinuxPrefix)
string(SUBSTRING "${WakeLinuxBodyRemainder}" 0 ${WakeLinuxLength} WakeLinuxBody)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gns-wake-linux.inc" CONTENT "${WakeLinuxPrefix}${WakeLinuxBody}" @ONLY)
