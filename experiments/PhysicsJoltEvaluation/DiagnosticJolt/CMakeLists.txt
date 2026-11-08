cmake_minimum_required(VERSION 3.25)
project(JoltCouplingInstrumentation LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(DIAGNOSTIC_JOLT_ROOT "" CACHE PATH "Isolated patched Jolt worktree")
set(EVALUATION_SOURCE_ROOT "" CACHE PATH "Exact experiment source")
option(DIAGNOSTIC_EXPECT_CLEAN "Build a pristine native control instead of isolated diagnostic Jolt" OFF)
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DIAGNOSTIC_JOLT_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE PIN OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT PIN STREQUAL "e77f175595e64cb44218cc9d9d56fc365ad0e36a")
    message(FATAL_ERROR "Wrong diagnostic Jolt pin")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DIAGNOSTIC_JOLT_ROOT}" diff --name-only HEAD
    OUTPUT_VARIABLE CHANGED OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(DIAGNOSTIC_EXPECT_CLEAN AND NOT CHANGED STREQUAL "")
    message(FATAL_ERROR "Pristine control has upstream changes")
elseif(NOT DIAGNOSTIC_EXPECT_CLEAN AND NOT CHANGED STREQUAL "Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp")
    message(FATAL_ERROR "Unexpected diagnostic upstream changes")
endif()
foreach(FEATURE USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
    TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER
    JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE INTERPROCEDURAL_OPTIMIZATION GENERATE_DEBUG_SYMBOLS
    ENABLE_ALL_WARNINGS FLOATING_POINT_EXCEPTIONS_ENABLED DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE
    JPH_BUILD_SHARED_LIBS OVERRIDE_CXX_FLAGS ENABLE_INSTALL USE_STATIC_MSVC_RUNTIME_LIBRARY)
    set(${FEATURE} OFF CACHE BOOL "" FORCE)
endforeach()
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
add_subdirectory("${DIAGNOSTIC_JOLT_ROOT}/Build" JoltBuild EXCLUDE_FROM_ALL)
add_executable(PhysicsJoltNativeCouplingInstrumented
    "${EVALUATION_SOURCE_ROOT}/experiments/PhysicsJoltEvaluation/NativeCouplingDiagnostic.cpp"
    "${EVALUATION_SOURCE_ROOT}/experiments/PhysicsJoltEvaluation/ExperimentSupport.cpp")
target_include_directories(PhysicsJoltNativeCouplingInstrumented PRIVATE
    "${EVALUATION_SOURCE_ROOT}/vendor/json/include" "${EVALUATION_SOURCE_ROOT}/vendor/box3d/include")
target_link_libraries(PhysicsJoltNativeCouplingInstrumented PRIVATE Jolt)
add_executable(PhysicsJoltUnifiedCouplingQualification
    "${EVALUATION_SOURCE_ROOT}/experiments/PhysicsJoltEvaluation/UnifiedCouplingEvaluation.cpp"
    "${EVALUATION_SOURCE_ROOT}/experiments/PhysicsJoltEvaluation/ExperimentSupport.cpp")
target_include_directories(PhysicsJoltUnifiedCouplingQualification PRIVATE
    "${EVALUATION_SOURCE_ROOT}/vendor/json/include" "${EVALUATION_SOURCE_ROOT}/vendor/box3d/include")
target_compile_definitions(PhysicsJoltUnifiedCouplingQualification PRIVATE PHYSICS_UNIFIED_NATIVE_ONLY=1)
target_link_libraries(PhysicsJoltUnifiedCouplingQualification PRIVATE Jolt)
if(NOT DIAGNOSTIC_EXPECT_CLEAN)
    target_compile_definitions(PhysicsJoltNativeCouplingInstrumented PRIVATE PHYSICS_S1_DIAGNOSTIC=1)
endif()
if(WIN32)
    target_link_libraries(PhysicsJoltNativeCouplingInstrumented PRIVATE psapi)
    target_link_libraries(PhysicsJoltUnifiedCouplingQualification PRIVATE psapi)
endif()
