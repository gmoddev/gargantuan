set(PHYSICS_EVALUATION_JOLT_ROOT "" CACHE PATH "Verified Jolt v5.6.0 checkout")
set(PHYSICS_EVALUATION_JOLT_COMMIT "e77f175595e64cb44218cc9d9d56fc365ad0e36a" CACHE STRING "Exact independently qualified Jolt source commit")
option(PHYSICS_EVALUATION_GANTRIA_NUMERICS "Compile explicit maintained Jolt API integration" OFF)
option(PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS "Compile isolated reciprocal attachment helper integration" OFF)
option(PHYSICS_EVALUATION_GANTRIA_CONVERGENCE "Compile candidate 80-integration retained-lambda provider" OFF)
option(PHYSICS_EVALUATION_GANTRIA_SLEEP_FIX "Compile opt-in elapsed-time sleep clock integration" OFF)
option(PHYSICS_EVALUATION_NATIVE_PROFILE "Instrument native Jolt scopes for diagnostic runs only" OFF)
if(PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS AND NOT PHYSICS_EVALUATION_GANTRIA_NUMERICS)
    message(FATAL_ERROR "[Physics:Evaluation] Attachment qualification requires maintained numerics")
endif()
if(PHYSICS_EVALUATION_GANTRIA_CONVERGENCE AND NOT PHYSICS_EVALUATION_GANTRIA_NUMERICS)
    message(FATAL_ERROR "[Physics:Evaluation] Convergence qualification requires maintained numerics")
endif()
if(PHYSICS_EVALUATION_GANTRIA_SLEEP_FIX AND NOT PHYSICS_EVALUATION_GANTRIA_NUMERICS)
    message(FATAL_ERROR "[Physics:Evaluation] Sleep clock qualification requires maintained numerics")
endif()
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
if(PHYSICS_EVALUATION_NATIVE_PROFILE)
    set(PROFILER_IN_DEBUG_AND_RELEASE ON CACHE BOOL "" FORCE)
    set(JPH_USE_EXTERNAL_PROFILE ON CACHE BOOL "" FORCE)
else()
    set(JPH_USE_EXTERNAL_PROFILE OFF CACHE BOOL "" FORCE)
endif()
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
    if(PHYSICS_EVALUATION_NATIVE_PROFILE)
        target_sources(${Target} PRIVATE Optimization/ExternalProfile.cpp)
        target_compile_definitions(${Target} PRIVATE PHYSICS_EVALUATION_NATIVE_PROFILE=1)
    endif()
    if(PHYSICS_EVALUATION_GANTRIA_NUMERICS)
        target_compile_definitions(${Target} PRIVATE PHYSICS_GANTRIA_JOLT=1)
    endif()
    if(PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS)
        target_compile_definitions(${Target} PRIVATE PHYSICS_GANTRIA_ATTACHMENTS=1)
    endif()
    if(PHYSICS_EVALUATION_GANTRIA_CONVERGENCE)
        target_compile_definitions(${Target} PRIVATE PHYSICS_GANTRIA_CONVERGENCE=1)
    endif()
    if(PHYSICS_EVALUATION_GANTRIA_SLEEP_FIX)
        target_compile_definitions(${Target} PRIVATE PHYSICS_GANTRIA_SLEEP_FIX=1)
    endif()
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
    PhysicsEvaluationTarget(PhysicsJoltUnifiedRigidEvaluation${PHYSICS_EVALUATION_SUFFIX} UnifiedRigidBenchmark.cpp RigidJoltBackend.cpp)
    PhysicsEvaluationTarget(PhysicsJoltSoftEvaluation${PHYSICS_EVALUATION_SUFFIX} SoftEvaluation.cpp)
    PhysicsEvaluationTarget(PhysicsJoltCouplingEvaluation${PHYSICS_EVALUATION_SUFFIX} CouplingEvaluation.cpp CouplingXpbdBackend.cpp)
    PhysicsEvaluationTarget(PhysicsJoltUnifiedCouplingQualification${PHYSICS_EVALUATION_SUFFIX} UnifiedCouplingEvaluation.cpp CouplingXpbdBackend.cpp)
endforeach()

PhysicsEvaluationTarget(PhysicsJoltXpbdCouplingDiagnostic XpbdCouplingDiagnostic.cpp XpbdDiagnosticBackend.cpp)
PhysicsEvaluationTarget(PhysicsJoltXpbdCouplingDiagnosticAllocations XpbdCouplingDiagnostic.cpp XpbdDiagnosticBackend.cpp)

# Production factories and defaults stay in gargantuan_core. Only this isolated
# executable explicitly constructs the shared-world experimental provider.
PhysicsEvaluationTarget(PhysicsJoltProviderQualification
    Provider/ProviderQualification.cpp RigidJoltBackend.cpp)
PhysicsEvaluationTarget(PhysicsJoltProviderBenchmark
    Provider/ProviderBenchmark.cpp RigidJoltBackend.cpp)
PhysicsEvaluationTarget(PhysicsJoltProviderBenchmarkAllocations
    Provider/ProviderBenchmark.cpp RigidJoltBackend.cpp)
PhysicsEvaluationTarget(PhysicsJoltEngineBenchmark
    Provider/EngineBenchmark.cpp RigidJoltBackend.cpp)

# Diagnostic targets keep scope/allocation instrumentation outside timing rankings.
PhysicsEvaluationTarget(PhysicsJoltAdmissionBenchmark
    Provider/AdmissionBenchmark.cpp RigidJoltBackend.cpp)
target_compile_definitions(PhysicsJoltAdmissionBenchmark PRIVATE
    PHYSICS_EVALUATION_ADMISSION_PROFILE=1 PHYSICS_EVALUATION_TRACK_ALLOCATIONS=1)
PhysicsEvaluationTarget(PhysicsJoltAdmissionTimingBenchmark
    Provider/AdmissionBenchmark.cpp RigidJoltBackend.cpp)
PhysicsEvaluationTarget(PhysicsJoltProviderLongRun
    Provider/ProviderBenchmark.cpp RigidJoltBackend.cpp)
target_compile_definitions(PhysicsJoltProviderLongRun PRIVATE PHYSICS_EVALUATION_LONG_RUN=1)
if(PHYSICS_EVALUATION_NATIVE_PROFILE)
    PhysicsEvaluationTarget(PhysicsJoltProviderProfile
        Provider/ProviderBenchmark.cpp RigidJoltBackend.cpp FinalOptimization/ProfileExit.cpp)
endif()
