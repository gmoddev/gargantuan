cmake_minimum_required(VERSION 3.25)

if(NOT EXISTS "${GARGANTUAN_BENCHMARK}" OR NOT DEFINED GARGANTUAN_TEST_ROOT)
	message(FATAL_ERROR "Missing content unwind regression configuration")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef GARGANTUAN_UNWIND_SUFFIX)
set(GARGANTUAN_UNWIND_PACKAGE "${GARGANTUAN_TEST_ROOT}/content-unwind-${GARGANTUAN_UNWIND_SUFFIX}")
execute_process(
	COMMAND "${GARGANTUAN_BENCHMARK}" --write-content-scale "${GARGANTUAN_UNWIND_PACKAGE}" 100
	RESULT_VARIABLE Result OUTPUT_VARIABLE Output ERROR_VARIABLE Error TIMEOUT 30)
if(NOT Result EQUAL 0)
	message(FATAL_ERROR "Unwind package preparation failed (${Result}): ${Output}${Error}")
endif()
execute_process(
	COMMAND "${GARGANTUAN_BENCHMARK}" --content-unwind-suite "${GARGANTUAN_UNWIND_PACKAGE}" local 100
	RESULT_VARIABLE Result OUTPUT_VARIABLE Output ERROR_VARIABLE Error TIMEOUT 300)
file(REMOVE_RECURSE "${GARGANTUAN_UNWIND_PACKAGE}")
if(NOT Result EQUAL 0 OR NOT Output MATCHES "CONTENT_UNWIND_SUITE_OK iterations=100")
	message(FATAL_ERROR "Content exception cleanup regression failed (${Result}): ${Output}${Error}")
endif()
message(STATUS "Content exception cleanup: 100 same-process iterations passed")
