set(PHYSICS_EVALUATION_JOLT_ROOT "" CACHE PATH "Verified Jolt v5.6.0 checkout")
set(PHYSICS_EVALUATION_JOLT_COMMIT "e77f175595e64cb44218cc9d9d56fc365ad0e36a")
if(NOT EXISTS "${PHYSICS_EVALUATION_JOLT_ROOT}/Jolt/Jolt.h")
    message(FATAL_ERROR "[Physics:Evaluation] Supply PHYSICS_EVALUATION_JOLT_ROOT at the pinned checkout")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PHYSICS_EVALUATION_JOLT_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE PHYSICS_EVALUATION_ACTUAL_JOLT OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PHYSICS_EVALUATION_GIT_EXIT)
if(NOT PHYSICS_EVALUATION_GIT_EXIT EQUAL 0 OR NOT PHYSICS_EVALUATION_ACTUAL_JOLT STREQUAL PHYSICS_EVALUATION_JOLT_COMMIT)
    message(FATAL_ERROR "[Physics:Evaluation] Jolt checkout does not match the pinned release commit")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PHYSICS_EVALUATION_JOLT_ROOT}" status --porcelain
    OUTPUT_VARIABLE PHYSICS_EVALUATION_JOLT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PHYSICS_EVALUATION_JOLT_STATUS_EXIT)
if(NOT PHYSICS_EVALUATION_JOLT_STATUS_EXIT EQUAL 0 OR PHYSICS_EVALUATION_JOLT_DIRTY)
    message(FATAL_ERROR "[Physics:Evaluation] Jolt source checkout is not clean")
endif()

# Controlled common SSE2 profile. Jolt SIMD extensions are disabled rather
# than silently propagating AVX2 flags to only part of the Engine comparator.
foreach(PHYSICS_EVALUATION_FEATURE USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD)
    set(${PHYSICS_EVALUATION_FEATURE} OFF CACHE BOOL "" FORCE)
endforeach()

# Root-cause probes are isolated experiments; the native runner uses no Engine
# physics adapter or core library. Its Box3D link supplies allocation hooks only.
foreach(PHYSICS_DIAGNOSTIC_SUFFIX "" Allocations)
    add_executable(PhysicsJoltNativeCouplingDiagnostic${PHYSICS_DIAGNOSTIC_SUFFIX}
        NativeCouplingDiagnostic.cpp ExperimentSupport.cpp)
    target_link_libraries(PhysicsJoltNativeCouplingDiagnostic${PHYSICS_DIAGNOSTIC_SUFFIX}
        PRIVATE Jolt box3d nlohmann_json)
    target_compile_features(PhysicsJoltNativeCouplingDiagnostic${PHYSICS_DIAGNOSTIC_SUFFIX} PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(PhysicsJoltNativeCouplingDiagnostic${PHYSICS_DIAGNOSTIC_SUFFIX} PRIVATE psapi)
    endif()
    if(PHYSICS_DIAGNOSTIC_SUFFIX)
        target_compile_definitions(PhysicsJoltNativeCouplingDiagnostic${PHYSICS_DIAGNOSTIC_SUFFIX}
            PRIVATE PHYSICS_EVALUATION_TRACK_ALLOCATIONS=1)
    endif()
endforeach()

foreach(PHYSICS_EVALUATION_FEATURE TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER
    JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE INTERPROCEDURAL_OPTIMIZATION GENERATE_DEBUG_SYMBOLS
    ENABLE_ALL_WARNINGS FLOATING_POINT_EXCEPTIONS_ENABLED DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE
    JPH_BUILD_SHARED_LIBS OVERRIDE_CXX_FLAGS ENABLE_INSTALL)
    set(${PHYSICS_EVALUATION_FEATURE} OFF CACHE BOOL "" FORCE)
endforeach()
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
else()
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${PHYSICS_EVALUATION_JOLT_ROOT}/Build" "${CMAKE_CURRENT_BINARY_DIR}/JoltBuild" EXCLUDE_FROM_ALL)

function(PhysicsEvaluationTarget Target)
    add_executable(${Target} ${ARGN} ExperimentSupport.cpp)
    target_link_libraries(${Target} PRIVATE gargantuan_core Jolt nlohmann_json)
    target_compile_features(${Target} PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(${Target} PRIVATE psapi)
        add_custom_command(TARGET ${Target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${Target}>")
    endif()
    if(Target MATCHES "Allocations$")
        target_compile_definitions(${Target} PRIVATE PHYSICS_EVALUATION_TRACK_ALLOCATIONS=1)
    endif()
endfunction()

foreach(PHYSICS_EVALUATION_SUFFIX "" Allocations)
    PhysicsEvaluationTarget(PhysicsJoltRigidEvaluation${PHYSICS_EVALUATION_SUFFIX} RigidBenchmark.cpp RigidJoltBackend.cpp)
    PhysicsEvaluationTarget(PhysicsJoltSoftEvaluation${PHYSICS_EVALUATION_SUFFIX} SoftEvaluation.cpp)
    PhysicsEvaluationTarget(PhysicsJoltCouplingEvaluation${PHYSICS_EVALUATION_SUFFIX} CouplingEvaluation.cpp CouplingXpbdBackend.cpp)
endforeach()

PhysicsEvaluationTarget(PhysicsJoltXpbdCouplingDiagnostic XpbdCouplingDiagnostic.cpp XpbdDiagnosticBackend.cpp)
PhysicsEvaluationTarget(PhysicsJoltXpbdCouplingDiagnosticAllocations XpbdCouplingDiagnostic.cpp XpbdDiagnosticBackend.cpp)
