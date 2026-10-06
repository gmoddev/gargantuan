# Pinned GNS KI-008 thinker fairness and F1 active-grant lock retry.
set(GnsThinkerPath "${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets/steamnetworkingsockets_thinker.cpp")
file(READ "${GnsThinkerPath}" GnsThinkerCurrent)
string(REPLACE "\r\n" "\n" GnsThinkerCurrent "${GnsThinkerCurrent}")

set(GnsOldEntry "void IThinker::Thinker_ProcessThinkers()\n{\n\t// We need the lock to access the thinker queue")
set(GnsNewEntry "void IThinker::Thinker_ProcessThinkers()\n{\n\t// Gargantuan KI-008: service only timers due when this pass began.\n\t// Keep fresh callback timestamps below, but return to socket reads before\n\t// servicing timers that became due while this batch was executing.\n\tconst SteamNetworkingMicroseconds usecEligibilityCutoff = SteamNetworkingSockets_GetLocalTimestamp();\n\t// We need the lock to access the thinker queue")
set(GnsPreviousEntry "${GnsNewEntry}")
string(REPLACE "\n{\n" "\n{\n\t#ifndef IS_STEAMDATAGRAMROUTER\n\tGargantuanServiceTimingSpan GargantuanThinkers(GargantuanServiceTimingPhase::Thinkers);\n\t#endif\n" GnsNewEntry "${GnsNewEntry}")
set(GnsOldCheck "if ( pNextThinker->GetNextThinkTime() >= usecNow )")
set(GnsNewCheck "if ( pNextThinker->GetNextThinkTime() >= usecEligibilityCutoff )")
set(GnsOldInclude "#include \"steamnetworkingsockets_thinker.h\"")
set(GnsNewInclude [=[#include "steamnetworkingsockets_thinker.h"

#ifndef IS_STEAMDATAGRAMROUTER
#include "ReliableServiceFeedback.hpp"
#include "ServiceTimingDiagnostics.hpp"
#endif]=])
set(GnsPreviousInclude "#include \"steamnetworkingsockets_thinker.h\"\n\n#ifndef IS_STEAMDATAGRAMROUTER\n#include \"ReliableServiceFeedback.hpp\"\n#endif")
set(GnsOldRetry [=[		else
		{
			// Deadlock!  Should be extremely rare.  Reschedule him for 1ms in the
			// future, and we'll try again.
			pNextThinker->InternalSetNextThinkTime( usecNow + 1000 );
		}]=])
set(GnsNewRetry [=[		else
		{
			// A partly first-sent F1 grant cannot afford a one-millisecond
			// reschedule after a brief connection-lock collision.
			SteamNetworkingMicroseconds usecRetry = usecNow + 1000;
			#ifndef IS_STEAMDATAGRAMROUTER
				if ( GargantuanHasRunningStructuralGrant() )
					usecRetry = usecNow + 25;
			#endif
			pNextThinker->InternalSetNextThinkTime( usecRetry );
		}]=])
set(GnsPreviousRetry "${GnsNewRetry}")
string(REPLACE "\t\t\tSteamNetworkingMicroseconds usecRetry" "\t\t\t#ifndef IS_STEAMDATAGRAMROUTER\n\t\t\tGargantuanThinkers.AddDetail(); // Connection-lock collision count.\n\t\t\t#endif\n\t\t\tSteamNetworkingMicroseconds usecRetry" GnsNewRetry "${GnsNewRetry}")
set(GnsOldCallback "\t\tif ( pNextThinker->TryLock() )\n\t\t{")
set(GnsNewCallback "${GnsOldCallback}\n\t\t\t#ifndef IS_STEAMDATAGRAMROUTER\n\t\t\tGargantuanThinkers.AddCount();\n\t\t\t#endif")
set(GnsPreviousCallback "${GnsNewCallback}")
string(APPEND GnsNewCallback "\n\t\t\t#ifndef IS_STEAMDATAGRAMROUTER\n\t\t\tconst SteamNetworkingMicroseconds usecGargantuanDeadline = GargantuanThinkers.Detailed() ? pNextThinker->GetNextThinkTime() : 0;\n\t\t\t#endif")
set(GnsOldInvoke "\t\t\tpNextThinker->Think( usecNow );")
set(GnsNewInvoke [=[			{
				#ifndef IS_STEAMDATAGRAMROUTER
				GargantuanServiceTimingSpan GargantuanCallback(GargantuanServiceTimingPhase::ThinkerCallback);
				GargantuanCallback.SetDetail(usecGargantuanDeadline);
				GargantuanCallback.SetResult(usecNow);
				GargantuanCallback.SetCount(nIterations);
				#endif
				// Think may destroy its owner. Nothing below reads that owner.
				pNextThinker->Think( usecNow );
			}]=])

# ASAP is a request for current native time, not an immortal heap priority.
# Resolve under the existing table lock. ASAP bypasses public lockless hints so
# a raced earlier-deadline hint cannot lose the request; finite/Never keep them.
set(GnsOldSetHint "if ( usecTargetThinkTime == m_usecNextThinkTime )")
set(GnsNewSetHint "if ( usecTargetThinkTime != k_nThinkTime_ASAP && usecTargetThinkTime == m_usecNextThinkTime )")
set(GnsOldEnsureHint "// Lockless fast-path read -- see SetNextThinkTime for explanation of the intentional race.\n\tif ( usecTargetThinkTime < m_usecNextThinkTime )")
set(GnsNewEnsureHint "// Lockless fast-path read -- see SetNextThinkTime for explanation of the intentional race.\n\tif ( usecTargetThinkTime == k_nThinkTime_ASAP || usecTargetThinkTime < m_usecNextThinkTime )")
set(GnsOldEnsureLocked "void IThinker::InternalEnsureMinThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime )\n{\n\ts_mutexThinkerTable.lock();")
set(GnsNewEnsureLocked "${GnsOldEnsureLocked}\n\tif ( usecTargetThinkTime == k_nThinkTime_ASAP )\n\t\tusecTargetThinkTime = SteamNetworkingSockets_GetLocalTimestamp();")
set(GnsOldSetLocked "void IThinker::InternalSetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime )\n{")
set(GnsNewSetLocked "${GnsOldSetLocked}\n\tif ( usecTargetThinkTime == k_nThinkTime_ASAP )\n\t\tusecTargetThinkTime = SteamNetworkingSockets_GetLocalTimestamp();")

# Strip only the known local corrections, then verify immutable upstream text.
# This accepts unpatched, KI-008-only, and fully patched caches.
set(GnsThinkerOriginal "${GnsThinkerCurrent}")
string(REPLACE "${GnsNewSetHint}" "${GnsOldSetHint}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewEnsureHint}" "${GnsOldEnsureHint}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewEnsureLocked}" "${GnsOldEnsureLocked}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewSetLocked}" "${GnsOldSetLocked}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewInvoke}" "${GnsOldInvoke}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewCallback}" "${GnsOldCallback}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsPreviousCallback}" "${GnsOldCallback}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewRetry}" "${GnsOldRetry}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsPreviousRetry}" "${GnsOldRetry}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewInclude}" "${GnsOldInclude}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsPreviousInclude}" "${GnsOldInclude}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewEntry}" "${GnsOldEntry}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsPreviousEntry}" "${GnsOldEntry}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewCheck}" "${GnsOldCheck}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(SHA256 GnsOriginalHash "${GnsThinkerOriginal}")
if(NOT GnsOriginalHash STREQUAL "3173e3850d9ce7ad479f799f2e60daaffbbfbfc225605e7d22310911baee1613")
	message(FATAL_ERROR "Pinned GNS thinker source differs from the KI-008/F1 correction input")
endif()

set(GnsThinkerDesired "${GnsThinkerOriginal}")
string(REPLACE "${GnsOldSetHint}" "${GnsNewSetHint}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldEnsureHint}" "${GnsNewEnsureHint}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldEnsureLocked}" "${GnsNewEnsureLocked}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldSetLocked}" "${GnsNewSetLocked}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldEntry}" "${GnsNewEntry}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldCheck}" "${GnsNewCheck}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldInclude}" "${GnsNewInclude}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldRetry}" "${GnsNewRetry}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldCallback}" "${GnsNewCallback}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldInvoke}" "${GnsNewInvoke}" GnsThinkerDesired "${GnsThinkerDesired}")
if(NOT GnsThinkerCurrent STREQUAL GnsThinkerDesired)
	file(WRITE "${GnsThinkerPath}" "${GnsThinkerDesired}")
endif()
