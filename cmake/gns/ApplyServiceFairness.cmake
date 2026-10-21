# Pinned GNS KI-008 thinker fairness and F1 active-grant lock retry.
set(GnsThinkerPath "${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets/steamnetworkingsockets_thinker.cpp")
file(READ "${GnsThinkerPath}" GnsThinkerCurrent)
string(REPLACE "\r\n" "\n" GnsThinkerCurrent "${GnsThinkerCurrent}")

set(GnsOldEntry "void IThinker::Thinker_ProcessThinkers()\n{\n\t// We need the lock to access the thinker queue")
set(GnsNewEntry "void IThinker::Thinker_ProcessThinkers()\n{\n\t// Gargantuan KI-008: service only timers due when this pass began.\n\t// Keep fresh callback timestamps below, but return to socket reads before\n\t// servicing timers that became due while this batch was executing.\n\tconst SteamNetworkingMicroseconds usecEligibilityCutoff = SteamNetworkingSockets_GetLocalTimestamp();\n\t// We need the lock to access the thinker queue")
set(GnsOldCheck "if ( pNextThinker->GetNextThinkTime() >= usecNow )")
set(GnsNewCheck "if ( pNextThinker->GetNextThinkTime() >= usecEligibilityCutoff )")
set(GnsOldInclude "#include \"steamnetworkingsockets_thinker.h\"")
set(GnsNewInclude [=[#include "steamnetworkingsockets_thinker.h"

#ifndef IS_STEAMDATAGRAMROUTER
#include "ReliableServiceFeedback.hpp"
#endif]=])
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

# Strip only the known local corrections, then verify immutable upstream text.
# This accepts unpatched, KI-008-only, and fully patched caches.
set(GnsThinkerOriginal "${GnsThinkerCurrent}")
string(REPLACE "${GnsNewRetry}" "${GnsOldRetry}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewInclude}" "${GnsOldInclude}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewEntry}" "${GnsOldEntry}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(REPLACE "${GnsNewCheck}" "${GnsOldCheck}" GnsThinkerOriginal "${GnsThinkerOriginal}")
string(SHA256 GnsOriginalHash "${GnsThinkerOriginal}")
if(NOT GnsOriginalHash STREQUAL "3173e3850d9ce7ad479f799f2e60daaffbbfbfc225605e7d22310911baee1613")
	message(FATAL_ERROR "Pinned GNS thinker source differs from the KI-008/F1 correction input")
endif()

set(GnsThinkerDesired "${GnsThinkerOriginal}")
string(REPLACE "${GnsOldEntry}" "${GnsNewEntry}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldCheck}" "${GnsNewCheck}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldInclude}" "${GnsNewInclude}" GnsThinkerDesired "${GnsThinkerDesired}")
string(REPLACE "${GnsOldRetry}" "${GnsNewRetry}" GnsThinkerDesired "${GnsThinkerDesired}")
if(NOT GnsThinkerCurrent STREQUAL GnsThinkerDesired)
	file(WRITE "${GnsThinkerPath}" "${GnsThinkerDesired}")
endif()
