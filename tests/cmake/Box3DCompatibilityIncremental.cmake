cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED GARGANTUAN_SOURCE_DIR OR NOT DEFINED BOX3D_SOURCE_DIR OR NOT DEFINED TEST_GENERATOR)
	message(FATAL_ERROR "GARGANTUAN_SOURCE_DIR, BOX3D_SOURCE_DIR and TEST_GENERATOR are required")
endif()

# Only this isolated fixture is altered. The shared vendor tree is read-only.
set(Fixture "${CMAKE_CURRENT_BINARY_DIR}/fixture")
set(FixtureBox3D "${Fixture}/box3d")
set(FixtureBuild "${CMAKE_CURRENT_BINARY_DIR}/fixture-build")
file(MAKE_DIRECTORY "${FixtureBox3D}")
file(COPY "${BOX3D_SOURCE_DIR}/src" "${BOX3D_SOURCE_DIR}/include"
	"${BOX3D_SOURCE_DIR}/CMakeLists.txt" DESTINATION "${FixtureBox3D}")
set(ProjectSource [=[
cmake_minimum_required(VERSION 3.25)
project(Box3DIncrementalGuard NONE)
include("@GARGANTUAN_SOURCE_DIR@/cmake/Box3DCompatibility.cmake")
GargantuanPrepareBox3DCompatibility("@FixtureBox3D@" PrivateSource)
add_custom_target(Check ALL COMMAND "${CMAKE_COMMAND}" -E true)
]=])
string(CONFIGURE "${ProjectSource}" ProjectSource @ONLY)
file(WRITE "${Fixture}/CMakeLists.txt" "${ProjectSource}")
set(ConfigureArguments -S "${Fixture}" -B "${FixtureBuild}" -G "${TEST_GENERATOR}")
if(DEFINED TEST_MAKE_PROGRAM AND NOT TEST_MAKE_PROGRAM STREQUAL "")
	list(APPEND ConfigureArguments "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${ConfigureArguments}
	RESULT_VARIABLE ConfigureStatus OUTPUT_VARIABLE ConfigureOutput ERROR_VARIABLE ConfigureError)
if(NOT ConfigureStatus EQUAL 0)
	message(FATAL_ERROR "Known Box3D fixture configure failed: ${ConfigureOutput}\n${ConfigureError}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FixtureBuild}" --target Check
	RESULT_VARIABLE InitialStatus OUTPUT_VARIABLE InitialOutput ERROR_VARIABLE InitialError)
if(NOT InitialStatus EQUAL 0)
	message(FATAL_ERROR "Known Box3D fixture build failed: ${InitialOutput}\n${InitialError}")
endif()

file(APPEND "${FixtureBox3D}/src/simd.h" "\n/* Unknown source revision. */\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FixtureBuild}" --target Check
	RESULT_VARIABLE ChangedStatus OUTPUT_VARIABLE ChangedOutput ERROR_VARIABLE ChangedError)
if(ChangedStatus EQUAL 0 OR NOT "${ChangedOutput}\n${ChangedError}" MATCHES "Unknown Box3D src/simd.h SHA256")
	message(FATAL_ERROR "Incremental build failed to rerun the source guard: ${ChangedStatus}\n${ChangedOutput}\n${ChangedError}")
endif()
configure_file("${BOX3D_SOURCE_DIR}/src/simd.h" "${FixtureBox3D}/src/simd.h" COPYONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FixtureBuild}" --target Check
	RESULT_VARIABLE RestoredStatus OUTPUT_VARIABLE RestoredOutput ERROR_VARIABLE RestoredError)
if(NOT RestoredStatus EQUAL 0)
	message(FATAL_ERROR "Restored SIMD fixture failed: ${RestoredOutput}\n${RestoredError}")
endif()
file(APPEND "${FixtureBox3D}/src/block_allocator.c" "\n/* Unknown source revision. */\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FixtureBuild}" --target Check
	RESULT_VARIABLE BlockStatus OUTPUT_VARIABLE BlockOutput ERROR_VARIABLE BlockError)
if(BlockStatus EQUAL 0 OR NOT "${BlockOutput}\n${BlockError}" MATCHES "Unknown Box3D src/block_allocator.c SHA256")
	message(FATAL_ERROR "Incremental build failed to rerun the block allocator guard: ${BlockStatus}\n${BlockOutput}\n${BlockError}")
endif()
message(STATUS "Incremental build reconfigures and rejects altered SIMD and block allocator sources")
