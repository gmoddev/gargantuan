if(NOT DEFINED GargantuanRoot)
	message(FATAL_ERROR "Gargantuan source root is required")
endif()

file(READ "${GargantuanRoot}/src/network/GameNetworkingSocketsTransport.cpp" Adapter)
string(FIND "${Adapter}" "void Observe(ConnectionId Id" ObserveStart)
string(FIND "${Adapter}" "static void StatusChanged" ObserveEnd)
if(ObserveStart EQUAL -1 OR ObserveEnd LESS ObserveStart)
	message(FATAL_ERROR "GNS observation function was not found")
endif()
math(EXPR ObserveLength "${ObserveEnd} - ${ObserveStart}")
string(SUBSTRING "${Adapter}" ${ObserveStart} ${ObserveLength} ObserveBody)

# During a token-bearing finite grant, only the already-fetched pre-send
# snapshot may populate diagnostic status. Config is unsampled on both send
# events, and the post-send event must not cause another status query.
string(FIND "${ObserveBody}" "if (SampleBackend && !KnownStatus &&" GuardedStatus)
string(FIND "${ObserveBody}" "if (SampleBackend) {\n\t\t\t\tauto ReadConfig" GuardedConfig)
string(REGEX MATCHALL "GetConnectionRealTimeStatus" StatusReads "${ObserveBody}")
string(REGEX MATCHALL "GetConfigValue" ConfigReads "${ObserveBody}")
list(LENGTH StatusReads StatusReadCount)
list(LENGTH ConfigReads ConfigReadCount)
if(GuardedStatus EQUAL -1 OR GuardedConfig EQUAL -1 OR
	NOT StatusReadCount EQUAL 1 OR NOT ConfigReadCount EQUAL 1)
	message(FATAL_ERROR "F1 diagnostic observation added an unguarded native status/config read")
endif()

string(FIND "${Adapter}" "const bool PooledStructuralGrant = Token && Message.Traffic() == TrafficClass::StructuralReplication;" GrantScope)
string(FIND "${Adapter}" "PooledStructuralGrant && HasPendingStatus ? &PreSendStatus : nullptr," ReusedStatus)
string(REGEX MATCHALL "!PooledStructuralGrant\\)" SampledSendSites "${Adapter}")
list(LENGTH SampledSendSites SampledSendCount)
string(FIND "${Adapter}" "\"GnsBefore\", Message.Payload()" BeforeStage)
string(FIND "${Adapter}" "\"GnsQueued\", Message.Payload()" QueuedStage)
string(FIND "${Adapter}" "MessageNumber, -1, static_cast<int>(Result), nullptr, !PooledStructuralGrant" QueuedResult)
if(GrantScope EQUAL -1 OR ReusedStatus EQUAL -1 OR NOT SampledSendCount EQUAL 2 OR
	BeforeStage EQUAL -1 OR QueuedStage EQUAL -1 OR QueuedResult EQUAL -1)
	message(FATAL_ERROR "F1 structural send events must retain metadata while skipping redundant backend introspection")
endif()

file(READ "${GargantuanRoot}/src/network/GnsServiceDiagnostics.hpp" DiagnosticRecord)
string(FIND "${DiagnosticRecord}" "MessageNumber = -1, ReceiveAgeUs = -1, QueueUs = -1" UnknownQueue)
string(FIND "${DiagnosticRecord}" "Rate = -1, RateMin = -1, RateMax = -1, SendBuffer = -1" UnknownConfig)
if(UnknownQueue EQUAL -1 OR UnknownConfig EQUAL -1)
	message(FATAL_ERROR "Unsampled GNS diagnostic fields must remain explicitly unavailable")
endif()
message(STATUS "Token-bearing structural send diagnostics retain metadata without redundant native introspection")
