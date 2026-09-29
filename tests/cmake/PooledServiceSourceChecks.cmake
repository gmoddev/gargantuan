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
	"PollRawUDPSockets( msWait, bManualPoll )")

file(READ "${GargantuanRoot}/src/network/GameNetworkingSocketsTransport.cpp" Adapter)
string(FIND "${Adapter}" "? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable" ReliableFlag)
if(ReliableFlag EQUAL -1)
	message(FATAL_ERROR "D01 structural adapter no longer uses ordinary reliable GNS flags")
endif()
string(FIND "${Adapter}" "k_nSteamNetworkingSend_NoNagle" NoNagle)
string(FIND "${Adapter}" "FlushMessages" ExplicitFlush)
if(NOT NoNagle EQUAL -1 OR NOT ExplicitFlush EQUAL -1)
	message(FATAL_ERROR "D01 GNS adapter now bypasses the pinned Nagle handoff")
endif()
message(STATUS "D01 pinned GNS quantum, Nagle and requested wake mechanisms verified")
