cmake_minimum_required(VERSION 3.25)

foreach(GARGANTUAN_REQUIRED IN ITEMS GARGANTUAN_PLAYER GARGANTUAN_SERVER)
	if(NOT DEFINED ${GARGANTUAN_REQUIRED} OR NOT EXISTS "${${GARGANTUAN_REQUIRED}}")
		message(FATAL_ERROR "Missing ${GARGANTUAN_REQUIRED}")
	endif()
endforeach()

function(GargantuanRequireRejected Label Executable Expected)
	execute_process(
		COMMAND "${Executable}" ${ARGN}
		RESULT_VARIABLE Result
		OUTPUT_VARIABLE Output
		ERROR_VARIABLE Error
		TIMEOUT 10
	)
	if(NOT Result EQUAL 2)
		message(FATAL_ERROR "${Label} returned ${Result}, expected bounded role rejection 2: ${Output}${Error}")
	endif()
	string(FIND "${Output}${Error}" "${Expected}" Found)
	if(Found EQUAL -1)
		message(FATAL_ERROR "${Label} did not explain the role boundary: ${Output}${Error}")
	endif()
endfunction()

function(GargantuanRequireParserAccepted Label Executable)
	execute_process(
		COMMAND "${Executable}" ${ARGN}
		RESULT_VARIABLE Result
		OUTPUT_VARIABLE Output
		ERROR_VARIABLE Error
		TIMEOUT 10
	)
	if(Result EQUAL 2 AND NOT "${Output}${Error}" MATCHES "built without production game transport support")
		message(FATAL_ERROR "${Label} was rejected by its role parser: ${Output}${Error}")
	endif()
endfunction()

GargantuanRequireRejected(
	"Player server bind" "${GARGANTUAN_PLAYER}" "use GargantuanServer --bind" --server-bind 127.0.0.1:46001
)
GargantuanRequireRejected(
	"Player server provider" "${GARGANTUAN_PLAYER}" "arguments are invalid" --node-token-environment GARGANTUAN_SECRET
)
GargantuanRequireRejected(
	"Server client connect" "${GARGANTUAN_SERVER}" "use GargantuanPlayer" --connect 127.0.0.1:46001
)
GargantuanRequireRejected(
	"Server graphical state" "${GARGANTUAN_SERVER}" "use GargantuanPlayer" --renderer vulkan
)
GargantuanRequireParserAccepted("Offline Player" "${GARGANTUAN_PLAYER}")
GargantuanRequireParserAccepted(
	"Network Player" "${GARGANTUAN_PLAYER}" --connect 127.0.0.1:46001
)
GargantuanRequireParserAccepted(
	"Dedicated Server" "${GARGANTUAN_SERVER}" --bind 127.0.0.1:46001
)
GargantuanRequireParserAccepted("Server startup smoke" "${GARGANTUAN_SERVER}" --startup-smoke)
