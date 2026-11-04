cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED GARGANTUAN_SOURCE_DIR)
	message(FATAL_ERROR "GARGANTUAN_SOURCE_DIR is required")
endif()

set(Fixture "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-unknown-source")
file(MAKE_DIRECTORY "${Fixture}/src")
file(WRITE "${Fixture}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.22)\n")
file(WRITE "${Fixture}/src/simd.h" "unknown Box3D source\n")
file(WRITE "${Fixture}/Probe.cmake"
	"include(\"${GARGANTUAN_SOURCE_DIR}/cmake/Box3DCompatibility.cmake\")\n"
	"GargantuanPrepareBox3DCompatibility(\"${Fixture}\" PrivateSource)\n")

execute_process(COMMAND "${CMAKE_COMMAND}" -P "${Fixture}/Probe.cmake"
	RESULT_VARIABLE Status OUTPUT_VARIABLE Output ERROR_VARIABLE Error)
if(Status EQUAL 0 OR NOT Error MATCHES "Unknown Box3D src/simd.h SHA256")
	message(FATAL_ERROR "Box3D unknown-source guard did not reject as expected: ${Status}\n${Output}\n${Error}")
endif()
message(STATUS "Box3D compatibility rejects unknown source before creating a private build copy")
