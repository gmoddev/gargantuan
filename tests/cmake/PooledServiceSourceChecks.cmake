if(NOT DEFINED GnsRoot OR NOT DEFINED GargantuanRoot)
	message(FATAL_ERROR "Pinned GNS and Gargantuan source roots are required")
endif()

function(RequireSource RelativePath Snippet)
	file(READ "${GnsRoot}/src/steamnetworkingsockets/${RelativePath}" Source)
	string(FIND "${Source}" "${Snippet}" Position)
	if(Position EQUAL -1)
		message(FATAL_ERROR "D01 pinned GNS mechanism changed: ${RelativePath}: ${Snippet}")
	endif()
endfunction()

RequireSource("steamnetworkingsockets_internal.h"
	"const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend = 1248;")
RequireSource("clientlib/steamnetworkingsockets_snp.h"
	"const float k_flSendRateBurstOverageAllowance = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;")
RequireSource("clientlib/csteamnetworkingsockets.cpp"
	"DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, NagleTime, 5000, 0, 20000 );")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"pSendMessage->SNPSend_SetUsecNagle( usecNow + m_connectionConfig.NagleTime.Get() );")
RequireSource("clientlib/steamnetworkingsockets_socketthread.cpp"
	"int msTaskWait = ( usecUntilNextThinkTime + 500 ) / 1000;")
RequireSource("clientlib/steamnetworkingsockets_socketthread.cpp"
	"msTaskWait = std::max( 1, msTaskWait );")
RequireSource("clientlib/steamnetworkingsockets_socketthread.cpp"
	"PollRawUDPSockets( msWait, bManualPoll, usecNextWakeTime )")
RequireSource("clientlib/steamnetworkingsockets_socketthread.cpp"
	"SteamNetworkingSocketsLib::GargantuanHasRunningStructuralGrant()")
RequireSource("clientlib/steamnetworkingsockets_socketthread.cpp"
	"CREATE_WAITABLE_TIMER_HIGH_RESOLUTION")
RequireSource("clientlib/steamnetworkingsockets_socketthread.cpp"
	"usecRemaining - 1000")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"GargantuanFeedback.NativePacket(nBytesSent)")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"GargantuanSeg.m_bGargantuanEverSent = true")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"GargantuanGrant.SenderPacketQuantum(GargantuanHasRunningStructuralGrant())")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"nGargantuanStructuralPacketsThisThink >= nGargantuanPacketQuantum")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"GargantuanOrdinaryPermit.Reserve(")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"GargantuanOrdinaryPermit.Complete(nBytesSent, helper.GargantuanHasData)")
RequireSource("clientlib/steamnetworkingsockets_snp.cpp"
	"GargantuanOrdinaryDataDeadline(usecNextSend, GargantuanNow, GargantuanEligible)")
RequireSource("steamnetworkingsockets_thinker.cpp"
	"GargantuanHasRunningStructuralGrant()")
RequireSource("steamnetworkingsockets_thinker.cpp"
	"usecTargetThinkTime = SteamNetworkingSockets_GetLocalTimestamp();")
RequireSource("steamnetworkingsockets_thinker.cpp"
	"usecRetry = usecNow + 25")

file(READ "${GargantuanRoot}/src/network/GameNetworkingSocketsTransport.cpp" Adapter)
string(FIND "${Adapter}" "Token && Message.Traffic() == TrafficClass::StructuralReplication" ScopedStructural)
string(FIND "${Adapter}" "? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_Reliable" ScopedFlag)
if(ScopedStructural EQUAL -1 OR ScopedFlag EQUAL -1)
	message(FATAL_ERROR "F1 pooled structural NoNagle scope changed")
endif()
string(FIND "${Adapter}" "FlushMessages" ExplicitFlush)
if(NOT ExplicitFlush EQUAL -1)
	message(FATAL_ERROR "F1 structural send added a connection-wide flush")
endif()
message(STATUS "F1 pinned GNS quantum, scoped structural NoNagle and requested wake mechanisms verified")
