# CMakeLists.txt for Gargantuan Engine:
# https://github.com/teamfireworks/gargantuan/

cmake_minimum_required(VERSION 3.25)
project(gargantuan CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GARGANTUAN_REQUIRED_DEPENDENCY_FILES
	"vendor/argparse/CMakeLists.txt"
	"vendor/box3d/CMakeLists.txt"
	"vendor/glaze/CMakeLists.txt"
	"vendor/glm/CMakeLists.txt"
	"vendor/json/CMakeLists.txt"
	"vendor/luau/CMakeLists.txt"
	"vendor/magicenum/include/magic_enum/magic_enum.hpp"
	"vendor/sdl/CMakeLists.txt"
	"vendor/sdl_image/CMakeLists.txt"
	"vendor/sdl_ttf/CMakeLists.txt"
	"vendor/tracy/CMakeLists.txt"
)
set(GARGANTUAN_MISSING_DEPENDENCY_FILES "")
foreach(GARGANTUAN_REQUIRED_DEPENDENCY_FILE IN LISTS GARGANTUAN_REQUIRED_DEPENDENCY_FILES)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${GARGANTUAN_REQUIRED_DEPENDENCY_FILE}")
		list(APPEND GARGANTUAN_MISSING_DEPENDENCY_FILES "${GARGANTUAN_REQUIRED_DEPENDENCY_FILE}")
	endif()
endforeach()

if(GARGANTUAN_MISSING_DEPENDENCY_FILES)
	list(JOIN GARGANTUAN_MISSING_DEPENDENCY_FILES ", " GARGANTUAN_MISSING_DEPENDENCY_DIAGNOSTIC)
	message(FATAL_ERROR
		"Required dependency content is missing: ${GARGANTUAN_MISSING_DEPENDENCY_DIAGNOSTIC}. "
		"Clone with --recursive or run 'git submodule update --init --recursive', then configure again."
	)
endif()

set(GARGANTUAN_REQUIRED_GENERATED_FILES "")
foreach(GARGANTUAN_GENERATED_KIND classes services)
	file(GLOB GARGANTUAN_GENERATED_DEFINITIONS CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/assets/${GARGANTUAN_GENERATED_KIND}/*.luau"
	)
	foreach(GARGANTUAN_GENERATED_DEFINITION IN LISTS GARGANTUAN_GENERATED_DEFINITIONS)
		get_filename_component(GARGANTUAN_GENERATED_NAME "${GARGANTUAN_GENERATED_DEFINITION}" NAME_WE)
		if(NOT GARGANTUAN_GENERATED_NAME OR GARGANTUAN_GENERATED_NAME STREQUAL ".config")
			continue()
		endif()
		list(APPEND GARGANTUAN_REQUIRED_GENERATED_FILES
			"include/gargantuan/${GARGANTUAN_GENERATED_KIND}/generated/${GARGANTUAN_GENERATED_NAME}.hpp"
			"src/${GARGANTUAN_GENERATED_KIND}/generated/${GARGANTUAN_GENERATED_NAME}.cpp"
		)
	endforeach()
endforeach()
set(GARGANTUAN_MISSING_GENERATED_FILES "")
foreach(GARGANTUAN_REQUIRED_GENERATED_FILE IN LISTS GARGANTUAN_REQUIRED_GENERATED_FILES)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${GARGANTUAN_REQUIRED_GENERATED_FILE}")
		list(APPEND GARGANTUAN_MISSING_GENERATED_FILES "${GARGANTUAN_REQUIRED_GENERATED_FILE}")
	endif()
endforeach()
if(GARGANTUAN_MISSING_GENERATED_FILES)
	list(JOIN GARGANTUAN_MISSING_GENERATED_FILES ", " GARGANTUAN_MISSING_GENERATED_DIAGNOSTIC)
	message(FATAL_ERROR
		"Required generated class/reflection sources are missing: ${GARGANTUAN_MISSING_GENERATED_DIAGNOSTIC}. "
		"Install the pinned tools with 'rokit install', ensure clang-format is on PATH, run "
		"'lute tools/classgen', then configure again."
	)
endif()

set(GARGANTUAN_RUNTIME_LUAU_MODULES
	"DefaultActionMap.luau"
	"DefaultInteractionRuntime.luau"
	"DefaultPlayerController.luau"
	"DefaultCamera.luau"
	"DefaultPlayerRuntime.luau"
)
foreach(GARGANTUAN_RUNTIME_LUAU_MODULE IN LISTS GARGANTUAN_RUNTIME_LUAU_MODULES)
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/assets/runtime/${GARGANTUAN_RUNTIME_LUAU_MODULE}"
		"${CMAKE_CURRENT_BINARY_DIR}/runtime/${GARGANTUAN_RUNTIME_LUAU_MODULE}"
		COPYONLY
	)
endforeach()

################################################################################
# Shaders
################################################################################

include(cmake/ShaderTools.cmake)
GargantuanFindGlslc(GARGANTUAN_GLSLC_EXECUTABLE)
if(APPLE)
	find_program(SPIRV_CROSS spirv-cross)
	find_program(XCRUN xcrun)

	if(NOT SPIRV_CROSS)
		message(WARNING "Missing spirv-cross, install via `brew install spirv-cross` to compile Metal shaders.")
	endif()
endif()

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Gargantuan requires a 64-bit toolchain. Please build from the x64 Native Tools Command Prompt.")
endif()

file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
	"assets/shaders/*.vert"
	"assets/shaders/*.frag"
)
file(GLOB_RECURSE SHADER_INCLUDE_SOURCES CONFIGURE_DEPENDS "assets/shaders/*.glsl")

set(SHADER_OUTPUTS "")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(SHADER IN LISTS SHADER_SOURCES)
	get_filename_component(SHADER_NAME ${SHADER} NAME)

	set(SPIRV_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
	add_custom_command(
		OUTPUT ${SPIRV_OUTPUT}
		COMMAND ${GARGANTUAN_GLSLC_EXECUTABLE} ${SHADER} -o ${SPIRV_OUTPUT}
		DEPENDS ${SHADER} ${SHADER_INCLUDE_SOURCES}
		COMMENT "Compiling SPIR-V ${SHADER_NAME}"
	)
	list(APPEND SHADER_OUTPUTS ${SPIRV_OUTPUT})

	if(APPLE AND SPIRV_CROSS AND XCRUN)
		set(METAL_SRC "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.metal")
		set(METAL_AIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.air")
		set(METALLIB_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.metallib")

		# SPIR-V -> MSL
		add_custom_command(
			OUTPUT ${METAL_SRC}
			COMMAND ${SPIRV_CROSS} ${SPIRV_OUTPUT} --msl --output ${METAL_SRC}
			DEPENDS ${SPIRV_OUTPUT}
			COMMENT "Cross-compiling SPIR-V to MSL: ${SHADER_NAME}"
		)

		# MSL -> AIR (.air) -> Metal Library
		add_custom_command(
			OUTPUT ${METALLIB_OUTPUT}
			COMMAND ${XCRUN} -sdk macosx metal -c ${METAL_SRC} -o ${METAL_AIR}
			COMMAND ${XCRUN} -sdk macosx metallib ${METAL_AIR} -o ${METALLIB_OUTPUT}
			DEPENDS ${METAL_SRC}
			COMMENT "Building Metal Library: ${SHADER_NAME}.metallib"
		)

		list(APPEND SHADER_OUTPUTS ${METALLIB_OUTPUT})
	endif()
endforeach()

add_custom_target(gargantuan_shaders DEPENDS ${SHADER_OUTPUTS})

################################################################################
# Vendors
################################################################################

set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_PCH ON CACHE BOOL "" FORCE)
set(SDL_RENDER ON CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE) #was giving me compile issues (don't wnna install nasm for no reason)
# SDL_image's vendored zlib CMake renames its tracked source zconf.h during an
# out-of-source configure. The stb backend already provides the PNG support the
# engine uses, so keep that support without configuring the mutating libpng path.
set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(vendor/argparse SYSTEM)
add_subdirectory(vendor/box3d SYSTEM)
add_subdirectory(vendor/glm SYSTEM)
add_subdirectory(vendor/glaze SYSTEM)
add_subdirectory(vendor/json SYSTEM)
add_subdirectory(vendor/luau SYSTEM)
add_subdirectory(vendor/sdl SYSTEM)
add_subdirectory(vendor/sdl_image SYSTEM)
add_subdirectory(vendor/sdl_ttf SYSTEM)

# SDL expands __FILE__ into diagnostics that ship in its runtime library. Keep
# those strings independent of the developer checkout without changing codegen
# for Gargantuan or the rest of the dependency graph.
if(MSVC)
	target_compile_options(SDL3-shared PRIVATE
		/experimental:deterministic
		"/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=."
		"/pathmap:${CMAKE_CURRENT_BINARY_DIR}=."
	)
else()
	target_compile_options(SDL3-shared PRIVATE
		"-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
		"-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=."
	)
endif()

set(GARGANTUAN_GUI_RUNTIME_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
file(MAKE_DIRECTORY "${GARGANTUAN_GUI_RUNTIME_DIRECTORY}")
set(GARGANTUAN_DEFAULT_GUI_FONT_PATH "${GARGANTUAN_GUI_RUNTIME_DIRECTORY}/GargantuanSans.ttf")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl_ttf/external/harfbuzz/perf/fonts/Roboto-Regular.ttf"
	"${GARGANTUAN_DEFAULT_GUI_FONT_PATH}"
	COPYONLY
)
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/GuiRuntimeConfig.hpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/gargantuan/GuiRuntimeConfig.hpp"
	@ONLY
)

################################################################################
# Tracy
################################################################################

option(GARGANTUAN_TRACY "Compile in the Tracy client" OFF)
option(GARGANTUAN_BUILD_RENDERER_BENCHMARKS "Build the manual renderer feasibility benchmark" ON)
option(GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS "Build the serialization baseline benchmark" ON)
option(GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE "Build the private Glaze codec evaluation targets" OFF)
option(GARGANTUAN_WITH_GNS "Build the optional GameNetworkingSockets transport adapter" OFF)
option(GARGANTUAN_RUN_GRAPHICAL_PACKAGE_SMOKE "Run the packaged player smoke on a GPU-capable test host" OFF)
set(GARGANTUAN_FILAMENT_ROOT "" CACHE PATH "External Filament SDK root for the experimental renderer benchmark")
set(GARGANTUAN_WITH_FILAMENT OFF)
if(GARGANTUAN_FILAMENT_ROOT)
	if(NOT WIN32 OR NOT MSVC)
		message(FATAL_ERROR "The experimental Filament v1.71.5 benchmark integration currently supports only 64-bit MSVC Windows builds")
	endif()
	if(MSVC_VERSION LESS 1950)
		message(FATAL_ERROR "The official Filament v1.71.5 Windows SDK requires an MSVC 19.50-compatible standard library ABI")
	endif()
	if(NOT EXISTS "${GARGANTUAN_FILAMENT_ROOT}/include/filament/Engine.h")
		message(FATAL_ERROR "GARGANTUAN_FILAMENT_ROOT does not contain Filament headers")
	endif()
	if(NOT EXISTS "${GARGANTUAN_FILAMENT_ROOT}/bin/matc.exe")
		message(FATAL_ERROR "GARGANTUAN_FILAMENT_ROOT does not contain the matching matc tool")
	endif()
	set(GARGANTUAN_WITH_FILAMENT ON)
endif()
set(TRACY_ENABLE ${GARGANTUAN_TRACY} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
set(TRACY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/tracy)

if(GARGANTUAN_WITH_GNS)
	include(cmake/GameNetworkingSockets.cmake)
endif()

option(
	GARGANTUAN_TRACY_TOOLS
	"Build tracy-profiler, tracy-capture and tracy-csvexport"
	OFF
)
if(GARGANTUAN_TRACY_TOOLS)
	include(ExternalProject)

	if(NOT WIN32 AND NOT APPLE AND NOT DEFINED ENV{WAYLAND_DISPLAY})
		set(GARGANTUAN_TRACY_LEGACY ON)
	else()
		set(GARGANTUAN_TRACY_LEGACY OFF)
	endif()

	foreach(GARGANTUAN_TRACY_TOOL capture csvexport profiler)
		ExternalProject_Add(tracy-${GARGANTUAN_TRACY_TOOL}
			SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tracy/${GARGANTUAN_TRACY_TOOL}"
			BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/tracy/${GARGANTUAN_TRACY_TOOL}"
			CMAKE_ARGS
				-DCMAKE_BUILD_TYPE=Release
				-DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
				-DLEGACY=${GARGANTUAN_TRACY_LEGACY}
		)
	endforeach()
endif()

################################################################################
# Linking
################################################################################

file(GLOB_RECURSE GARGANTUAN_SOURCES CONFIGURE_DEPENDS
	"src/*.cpp"
	"src/*.hpp"
)
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp")
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/player/PlayerMain.cpp")
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/serialization/GlazePrototype.cpp")
set(GARGANTUAN_TELEMETRY_HOST_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/OptionalTelemetry.cpp"
)
list(REMOVE_ITEM GARGANTUAN_SOURCES ${GARGANTUAN_TELEMETRY_HOST_SOURCES})
if(NOT GARGANTUAN_WITH_FILAMENT)
	list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/render/FilamentRenderer.cpp")
endif()
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/network/GameNetworkingSocketsTransport.cpp")

add_library(gargantuan_core OBJECT ${GARGANTUAN_SOURCES})
add_executable(gargantuan "src/Main.cpp" ${GARGANTUAN_TELEMETRY_HOST_SOURCES} ${SHADER_OUTPUTS})
target_link_libraries(gargantuan PRIVATE gargantuan_core ${CMAKE_DL_LIBS})
add_executable(gargantuan_player "src/player/PlayerMain.cpp")
set_target_properties(gargantuan_player PROPERTIES OUTPUT_NAME "GargantuanPlayer")
if(APPLE)
	set_target_properties(gargantuan_player PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
	set_target_properties(gargantuan_player PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
target_link_libraries(gargantuan_player PRIVATE gargantuan_core ${CMAKE_DL_LIBS})
add_executable(gargantuan_packager "tools/PackagerMain.cpp")
set_target_properties(gargantuan_packager PROPERTIES OUTPUT_NAME "gargantuan-packager")
target_link_libraries(gargantuan_packager PRIVATE gargantuan_core ${CMAKE_DL_LIBS})

# Shader generation and executable-local staging are separate dependencies.
# Keep staging in ALL so an incremental build that adds a shader refreshes the
# runtime directory even when the executable itself does not relink.
add_custom_target(gargantuan_stage_shaders ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_BINARY_DIR}/shaders"
		"$<TARGET_FILE_DIR:gargantuan>/shaders"
	DEPENDS gargantuan gargantuan_shaders
	COMMENT "Staging shaders next to the Gargantuan executable"
	VERBATIM
)

# This is the one authoritative redistributable runtime input consumed by the
# CLI and EditorHost packaging paths. The manifest is deliberately closed: the
# packager copies only files named here and validates every staged byte.
set(GARGANTUAN_SDL_SONAME_ARGUMENT)
if(NOT WIN32)
	list(APPEND GARGANTUAN_SDL_SONAME_ARGUMENT
		-DGARGANTUAN_SDL_SONAME_FILE=$<TARGET_SONAME_FILE:SDL3::SDL3>)
endif()
set(GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS)
if(MSVC)
	set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
	include(InstallRequiredSystemLibraries)
	if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
		message(FATAL_ERROR "MSVC runtime redistribution files could not be resolved")
	endif()
	set(GARGANTUAN_SYSTEM_RUNTIME_COUNT 0)
	foreach(GARGANTUAN_SYSTEM_RUNTIME_FILE IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
		if(NOT EXISTS "${GARGANTUAN_SYSTEM_RUNTIME_FILE}")
			message(FATAL_ERROR "Required MSVC runtime redistribution file is unavailable")
		endif()
		list(APPEND GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS
			"-DGARGANTUAN_SYSTEM_RUNTIME_FILE_${GARGANTUAN_SYSTEM_RUNTIME_COUNT}=${GARGANTUAN_SYSTEM_RUNTIME_FILE}")
		math(EXPR GARGANTUAN_SYSTEM_RUNTIME_COUNT "${GARGANTUAN_SYSTEM_RUNTIME_COUNT} + 1")
	endforeach()
	list(APPEND GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS
		-DGARGANTUAN_SYSTEM_RUNTIME_COUNT=${GARGANTUAN_SYSTEM_RUNTIME_COUNT})
endif()
add_custom_target(gargantuan_runtime_distribution ALL
	COMMAND ${CMAKE_COMMAND}
		-DGARGANTUAN_RUNTIME_DISTRIBUTION_ROOT=$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution
		-DGARGANTUAN_PLAYER_FILE=$<TARGET_FILE:gargantuan_player>
		-DGARGANTUAN_SDL_FILE=$<TARGET_FILE:SDL3::SDL3>
		${GARGANTUAN_SDL_SONAME_ARGUMENT}
		${GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS}
		-DGARGANTUAN_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		-DGARGANTUAN_PLATFORM=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeDistribution.cmake
	DEPENDS gargantuan_player gargantuan_shaders
	COMMENT "Staging the canonical Gargantuan player runtime distribution"
	VERBATIM
)
add_dependencies(gargantuan_packager gargantuan_runtime_distribution)

if(MSVC)
	target_compile_options(gargantuan_core PUBLIC /EHsc)
endif()

# GCC 13+ rejects members named after their own type (ie. ScrptEngine's
# ThreadEngine)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(gargantuan_core PRIVATE -Wno-changes-meaning)
endif()

target_include_directories(gargantuan_core PUBLIC
	"src"
	"include"
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(GARGANTUAN_WITH_FILAMENT)
	set(FILAMENT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/filament")
	set(FILAMENT_GENERATED_MATERIAL "${FILAMENT_GENERATED_DIR}/GargantuanBenchmarkMaterial.h")
	file(MAKE_DIRECTORY "${FILAMENT_GENERATED_DIR}")
	add_custom_command(
		OUTPUT "${FILAMENT_GENERATED_MATERIAL}"
		COMMAND "${GARGANTUAN_FILAMENT_ROOT}/bin/matc.exe"
			--platform desktop
			--api opengl
			--api vulkan
			--variant-filter=dynamicLighting,shadowReceiver,skinning,vsm,fog,ssr,stereo
			--output-format header
			--output "${FILAMENT_GENERATED_MATERIAL}"
			"${CMAKE_CURRENT_SOURCE_DIR}/assets/filament/benchmark.mat"
		MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/assets/filament/benchmark.mat"
		COMMENT "Compiling the experimental Filament benchmark material"
		VERBATIM
	)
	target_sources(gargantuan_core PRIVATE "${FILAMENT_GENERATED_MATERIAL}")
	target_include_directories(gargantuan_core SYSTEM PRIVATE
		"${GARGANTUAN_FILAMENT_ROOT}/include"
		"${FILAMENT_GENERATED_DIR}"
	)
	target_compile_definitions(gargantuan_core PUBLIC GARGANTUAN_WITH_FILAMENT=1)

	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(FILAMENT_RUNTIME_VARIANT mdd)
	else()
		set(FILAMENT_RUNTIME_VARIANT md)
	endif()
	set(FILAMENT_LIBRARY_DIR "${GARGANTUAN_FILAMENT_ROOT}/lib/x86_64/${FILAMENT_RUNTIME_VARIANT}")
	foreach(FILAMENT_LIBRARY filament backend filaflat filabridge utils zstd bluegl bluevk smol-v)
		if(NOT EXISTS "${FILAMENT_LIBRARY_DIR}/${FILAMENT_LIBRARY}.lib")
			message(FATAL_ERROR "Filament library is missing: ${FILAMENT_LIBRARY_DIR}/${FILAMENT_LIBRARY}.lib")
		endif()
		target_link_libraries(gargantuan_core PUBLIC "${FILAMENT_LIBRARY_DIR}/${FILAMENT_LIBRARY}.lib")
	endforeach()
	if(WIN32)
		target_link_libraries(gargantuan_core PUBLIC opengl32 gdi32 user32 shlwapi)
	endif()
endif()

target_include_directories(gargantuan_core SYSTEM PUBLIC
	"vendor/argparse/include"
	"vendor/box3d/include"
	"vendor/glm/glm"
	"vendor/luau/Ast/include"
	"vendor/luau/CodeGen/include"
	"vendor/luau/Common/include"
	"vendor/luau/Compiler/include"
	"vendor/luau/Config/include"
	"vendor/luau/Require/include"
	"vendor/luau/VM/include"
	"vendor/magicenum/include"
	"vendor/sdl/include"
	"vendor/sdl_image/include"
	"vendor/sdl_ttf/include"
)

if(APPLE)
	target_link_options(gargantuan PRIVATE "LINKER:-no_warn_duplicate_libraries")
endif()

target_link_libraries(gargantuan_core
	PUBLIC
		argparse::argparse
		SDL3::SDL3
		SDL3_ttf-static
		SDL3_image-static
		box3d::box3d
		glm
		Luau.Compiler
		Luau.CodeGen
		Luau.Ast
		Luau.VM
		Luau.Require
		Luau.Config
		Tracy::TracyClient
	PRIVATE
		nlohmann_json
)

if(GARGANTUAN_WITH_GNS)
	add_library(gargantuan_gns_transport STATIC
		"src/network/GameNetworkingSocketsTransport.cpp"
	)
	target_compile_features(gargantuan_gns_transport PUBLIC cxx_std_23)
	target_compile_definitions(gargantuan_gns_transport PUBLIC GARGANTUAN_WITH_GNS=1)
	target_include_directories(gargantuan_gns_transport PUBLIC "include")
	target_link_libraries(gargantuan_gns_transport PUBLIC GameNetworkingSockets::static)
	if(MSVC)
		target_compile_options(gargantuan_gns_transport PRIVATE /EHsc)
	endif()
endif()

include(CTest)
if(BUILD_TESTING)
	add_test(NAME gargantuan_shader_tool_validation
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
			-DGARGANTUAN_GLSLC_EXECUTABLE=${GARGANTUAN_GLSLC_EXECUTABLE}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/ShaderToolValidationTests.cmake
	)
	add_test(NAME gargantuan_packaged_shaders
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_SHADER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
			-DGARGANTUAN_SHADER_RUNTIME_DIR=$<TARGET_FILE_DIR:gargantuan>/shaders
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRuntimeShaders.cmake
	)
	set_tests_properties(gargantuan_packaged_shaders PROPERTIES LABELS "build;shader;packaging")
	if(GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE)
		if(MSVC AND MSVC_VERSION LESS 1950)
			message(FATAL_ERROR "The pinned Glaze prototype requires MSVC 19.50 or newer")
		endif()
		add_library(gargantuan_glaze_serialization_prototype STATIC
			"src/serialization/GlazePrototype.cpp"
		)
		target_compile_features(gargantuan_glaze_serialization_prototype PRIVATE cxx_std_23)
		target_include_directories(gargantuan_glaze_serialization_prototype PRIVATE "src" "include")
		target_link_libraries(gargantuan_glaze_serialization_prototype PRIVATE gargantuan_core glaze::glaze)
		if(MSVC)
			target_compile_options(gargantuan_glaze_serialization_prototype PRIVATE /EHsc)
		endif()
		add_executable(gargantuan_glaze_serialization_tests "tests/GlazeSerializationTests.cpp")
		target_link_libraries(gargantuan_glaze_serialization_tests PRIVATE
			gargantuan_core
			gargantuan_glaze_serialization_prototype
		)
		add_test(NAME GlazeSerializationTests COMMAND gargantuan_glaze_serialization_tests)
	endif()
	add_executable(gargantuan_foundation_tests "tests/FoundationTests.cpp")
	target_link_libraries(gargantuan_foundation_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_asset_foundation_tests "tests/AssetFoundationTests.cpp")
	target_link_libraries(gargantuan_asset_foundation_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_asset_foundation_tests PRIVATE
		GARGANTUAN_ASSET_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl_ttf/external/harfbuzz/perf/fonts/Roboto-Regular.ttf"
		GARGANTUAN_ASSET_TEST_PNG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/github/ross.png"
	)
	add_executable(gargantuan_asset_foundation_benchmark "tests/AssetFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_asset_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_asset_foundation_benchmark PRIVATE
		GARGANTUAN_ASSET_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl_ttf/external/harfbuzz/perf/fonts/Roboto-Regular.ttf"
	)
	add_executable(gargantuan_animation_foundation_tests "tests/AnimationFoundationTests.cpp")
	target_link_libraries(gargantuan_animation_foundation_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_animation_foundation_benchmark "tests/AnimationFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_animation_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_audio_foundation_tests "tests/AudioFoundationTests.cpp")
	target_link_libraries(gargantuan_audio_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_audio_foundation_benchmark "tests/AudioFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_audio_foundation_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_renderer_publication_tests "tests/RendererPublicationTests.cpp")
	target_link_libraries(gargantuan_renderer_publication_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_environment_lighting_benchmark "tests/EnvironmentLightingBenchmark.cpp")
	target_link_libraries(gargantuan_environment_lighting_benchmark PRIVATE gargantuan_core)
	add_library(gargantuan_fake_telemetry SHARED "tests/telemetry/FakeTelemetryLibrary.cpp")
	target_include_directories(gargantuan_fake_telemetry PRIVATE "src")
	add_library(gargantuan_fake_telemetry_missing_export SHARED "tests/telemetry/FakeTelemetryMissingExport.cpp")
	add_executable(gargantuan_optional_telemetry_tests
		"tests/OptionalTelemetryTests.cpp"
		${GARGANTUAN_TELEMETRY_HOST_SOURCES}
	)
	target_include_directories(gargantuan_optional_telemetry_tests PRIVATE "src")
	target_link_libraries(gargantuan_optional_telemetry_tests PRIVATE ${CMAKE_DL_LIBS})
	if(MSVC)
		target_compile_options(gargantuan_fake_telemetry PRIVATE /EHsc)
		target_compile_options(gargantuan_optional_telemetry_tests PRIVATE /EHsc)
	endif()
	if(GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE)
		target_compile_definitions(gargantuan_foundation_tests PRIVATE GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE=1)
		target_link_libraries(gargantuan_foundation_tests PRIVATE gargantuan_glaze_serialization_prototype)
	endif()
	add_executable(gargantuan_physics_backend_tests "tests/PhysicsBackendTests.cpp")
	target_link_libraries(gargantuan_physics_backend_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_physics_query_benchmark "tests/PhysicsQueryBenchmark.cpp")
	target_link_libraries(gargantuan_physics_query_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_soft_body_physics_tests "tests/SoftBodyPhysicsTests.cpp")
	target_link_libraries(gargantuan_soft_body_physics_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_soft_body_physics_benchmark "tests/SoftBodyPhysicsBenchmark.cpp")
	target_link_libraries(gargantuan_soft_body_physics_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_platform_input_boundary_tests "tests/PlatformInputBoundaryTests.cpp")
	target_link_libraries(gargantuan_platform_input_boundary_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_player_runtime_tests "tests/PlayerRuntimeTests.cpp")
	target_link_libraries(gargantuan_player_runtime_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_interaction_foundation_tests "tests/InteractionFoundationTests.cpp")
	target_link_libraries(gargantuan_interaction_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_interaction_foundation_benchmark "tests/InteractionFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_interaction_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_entitlement_service_tests "tests/EntitlementServiceTests.cpp")
	target_link_libraries(gargantuan_entitlement_service_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_entitlement_service_tests PRIVATE
		GARGANTUAN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
	)
	add_executable(gargantuan_entitlement_provider_benchmark "tests/EntitlementProviderBenchmark.cpp")
	target_link_libraries(gargantuan_entitlement_provider_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_gui_foundation_tests "tests/GuiFoundationTests.cpp")
	target_link_libraries(gargantuan_gui_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_first_complete_game_tests "tests/FirstCompleteGameTests.cpp")
	target_link_libraries(gargantuan_first_complete_game_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_first_complete_game_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
	)
	add_executable(gargantuan_packaging_tests "tests/PackagingTests.cpp")
	target_link_libraries(gargantuan_packaging_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_packaging_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
		GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT="$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution"
	)
	add_dependencies(gargantuan_packaging_tests gargantuan_packager gargantuan_runtime_distribution)
	add_executable(gargantuan_packaging_editor_host_tests "tests/PackagingEditorHostTests.cpp")
	target_link_libraries(gargantuan_packaging_editor_host_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_packaging_editor_host_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
	)
	add_dependencies(gargantuan_packaging_editor_host_tests gargantuan_runtime_distribution)
	add_executable(gargantuan_packaging_benchmark "tests/PackagingBenchmark.cpp")
	target_link_libraries(gargantuan_packaging_benchmark PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_packaging_benchmark PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
		GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT="$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution"
	)
	add_dependencies(gargantuan_packaging_benchmark gargantuan_runtime_distribution)
	add_executable(gargantuan_gui_foundation_benchmark "tests/GuiFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_gui_foundation_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_prerun_bootstrap_tests "tests/PreRunBootstrapTests.cpp")
	target_link_libraries(gargantuan_prerun_bootstrap_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_attributes_tags_profile "tests/AttributesTagsProfile.cpp")
	target_link_libraries(gargantuan_attributes_tags_profile PRIVATE gargantuan_core)
	add_executable(gargantuan_networking_contract_tests "tests/NetworkingContractsTests.cpp")
	target_link_libraries(gargantuan_networking_contract_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_simulated_transport_tests "tests/SimulatedTransportTests.cpp")
	target_link_libraries(gargantuan_simulated_transport_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_scheduler_contract_tests "tests/SchedulerContractTests.cpp")
	target_link_libraries(gargantuan_scheduler_contract_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_replication_tests "tests/ReplicationTests.cpp")
	target_link_libraries(gargantuan_replication_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_remote_tests "tests/RemoteTests.cpp")
	target_link_libraries(gargantuan_remote_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_remote_luau_tests "tests/RemoteLuauTests.cpp")
	target_link_libraries(gargantuan_remote_luau_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_replication_benchmark "tests/ReplicationBenchmark.cpp")
	target_link_libraries(gargantuan_replication_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_remote_benchmark "tests/RemoteBenchmark.cpp")
	target_link_libraries(gargantuan_remote_benchmark PRIVATE gargantuan_core)
	if(GARGANTUAN_WITH_GNS)
		add_executable(gargantuan_real_transport_tests "tests/GameNetworkingSocketsTransportTests.cpp")
		target_link_libraries(gargantuan_real_transport_tests PRIVATE
			gargantuan_core
			gargantuan_gns_transport
		)
		add_executable(gargantuan_remote_real_transport_tests "tests/RemoteGnsTests.cpp")
		target_link_libraries(gargantuan_remote_real_transport_tests PRIVATE
			gargantuan_core
			gargantuan_gns_transport
		)
	endif()
	if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
		add_executable(gargantuan_renderer_benchmark "tests/RendererBenchmark.cpp")
		target_link_libraries(gargantuan_renderer_benchmark PRIVATE gargantuan_core)
		add_executable(gargantuan_renderer_foundation2_benchmark "tests/RendererFoundation2Benchmark.cpp")
		target_link_libraries(gargantuan_renderer_foundation2_benchmark PRIVATE gargantuan_core)
		add_executable(gargantuan_renderer_foundation2_gpu_benchmark "tests/RendererFoundation2GpuBenchmark.cpp")
		target_link_libraries(gargantuan_renderer_foundation2_gpu_benchmark PRIVATE gargantuan_core)
		add_dependencies(gargantuan_renderer_foundation2_gpu_benchmark gargantuan_shaders)
		add_executable(gargantuan_first_complete_game_gpu_tests "tests/FirstCompleteGameGpuTests.cpp")
		target_link_libraries(gargantuan_first_complete_game_gpu_tests PRIVATE gargantuan_core)
		target_compile_definitions(gargantuan_first_complete_game_gpu_tests PRIVATE
			GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
			GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT="$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution"
		)
		add_dependencies(gargantuan_first_complete_game_gpu_tests
			gargantuan_shaders gargantuan_packager gargantuan_runtime_distribution)
		add_executable(gargantuan_editor_viewport_smoke "tests/EditorViewportSmoke.cpp")
		target_link_libraries(gargantuan_editor_viewport_smoke PRIVATE gargantuan_core)
		add_dependencies(gargantuan_editor_viewport_smoke gargantuan_shaders)
		add_executable(gargantuan_editor_viewport_presentation_benchmark
			"tests/EditorViewportPresentationBenchmark.cpp")
		target_link_libraries(gargantuan_editor_viewport_presentation_benchmark PRIVATE gargantuan_core)
		add_dependencies(gargantuan_editor_viewport_presentation_benchmark gargantuan_shaders)
		add_test(NAME gargantuan_renderer_projection_headless
			COMMAND gargantuan_renderer_benchmark
				--backend=projection --scenario=mixed --count=3 --frames=2 --warmup=1
				--width=320 --height=180 --shadows=off
		)
		add_test(NAME gargantuan_renderer_backend_headless
			COMMAND gargantuan_renderer_benchmark
				--backend=headless --scenario=mixed --count=3 --frames=2 --warmup=1
				--width=320 --height=180 --shadows=off
		)
	endif()
	if(GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS)
		add_executable(gargantuan_serialization_benchmark "tests/SerializationBenchmark.cpp")
		target_link_libraries(gargantuan_serialization_benchmark PRIVATE gargantuan_core)
		if(GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE)
			target_compile_definitions(gargantuan_serialization_benchmark PRIVATE GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE=1)
			target_link_libraries(gargantuan_serialization_benchmark PRIVATE gargantuan_glaze_serialization_prototype)
		endif()
		add_test(NAME SerializationBenchmarkSmoke COMMAND gargantuan_serialization_benchmark)
	endif()
	if(GARGANTUAN_WITH_FILAMENT)
		add_executable(gargantuan_filament_renderer_tests "tests/FilamentRendererTests.cpp")
		target_link_libraries(gargantuan_filament_renderer_tests PRIVATE gargantuan_core)
	endif()
	if(WIN32)
		add_custom_target(gargantuan_stage_sdl_runtime
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:SDL3::SDL3>"
				"$<TARGET_FILE_DIR:gargantuan>"
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${CMAKE_CURRENT_BINARY_DIR}/runtime"
				"$<TARGET_FILE_DIR:gargantuan>/runtime"
			COMMENT "Staging the shared SDL3 and Luau runtimes once for Windows executables"
			VERBATIM
		)
		set(GARGANTUAN_SDL_RUNTIME_TARGETS
			gargantuan
			gargantuan_foundation_tests
			gargantuan_asset_foundation_tests
			gargantuan_asset_foundation_benchmark
			gargantuan_animation_foundation_tests
			gargantuan_animation_foundation_benchmark
			gargantuan_audio_foundation_tests
			gargantuan_audio_foundation_benchmark
			gargantuan_renderer_publication_tests
			gargantuan_environment_lighting_benchmark
			gargantuan_prerun_bootstrap_tests
			gargantuan_attributes_tags_profile
			gargantuan_physics_backend_tests
			gargantuan_physics_query_benchmark
			gargantuan_soft_body_physics_tests
			gargantuan_soft_body_physics_benchmark
			gargantuan_platform_input_boundary_tests
			gargantuan_player_runtime_tests
			gargantuan_interaction_foundation_tests
			gargantuan_interaction_foundation_benchmark
			gargantuan_entitlement_service_tests
			gargantuan_entitlement_provider_benchmark
			gargantuan_gui_foundation_tests
			gargantuan_first_complete_game_tests
			gargantuan_packaging_tests
			gargantuan_packaging_editor_host_tests
			gargantuan_packaging_benchmark
			gargantuan_gui_foundation_benchmark
			gargantuan_networking_contract_tests
			gargantuan_simulated_transport_tests
			gargantuan_scheduler_contract_tests
			gargantuan_replication_tests
			gargantuan_remote_tests
			gargantuan_remote_luau_tests
			gargantuan_replication_benchmark
			gargantuan_remote_benchmark
		)
		if(GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS gargantuan_serialization_benchmark)
		endif()
		if(GARGANTUAN_WITH_GNS)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS
				gargantuan_real_transport_tests
				gargantuan_remote_real_transport_tests
			)
			set(GARGANTUAN_PROTOBUF_RUNTIMES "")
			if(TARGET protobuf::libprotobuf)
				list(APPEND GARGANTUAN_PROTOBUF_RUNTIMES "$<TARGET_FILE:protobuf::libprotobuf>")
			elseif(DEFINED Protobuf_DIR)
				get_filename_component(GARGANTUAN_PROTOBUF_PACKAGE_ROOT "${Protobuf_DIR}/../.." ABSOLUTE)
				if(EXISTS "${GARGANTUAN_PROTOBUF_PACKAGE_ROOT}/bin/libprotobuf.dll")
					list(APPEND GARGANTUAN_PROTOBUF_RUNTIMES "${GARGANTUAN_PROTOBUF_PACKAGE_ROOT}/bin/libprotobuf.dll")
				endif()
				if(EXISTS "${GARGANTUAN_PROTOBUF_PACKAGE_ROOT}/debug/bin/libprotobufd.dll")
					list(APPEND GARGANTUAN_PROTOBUF_RUNTIMES "${GARGANTUAN_PROTOBUF_PACKAGE_ROOT}/debug/bin/libprotobufd.dll")
				endif()
			endif()
			foreach(GARGANTUAN_PROTOBUF_RUNTIME IN LISTS GARGANTUAN_PROTOBUF_RUNTIMES)
				add_custom_command(TARGET gargantuan_real_transport_tests POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_real_transport_tests>"
				)
				add_custom_command(TARGET gargantuan_remote_real_transport_tests POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_remote_real_transport_tests>"
				)
			endforeach()
		endif()
		if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS
				gargantuan_renderer_benchmark
				gargantuan_renderer_foundation2_benchmark
				gargantuan_renderer_foundation2_gpu_benchmark
				gargantuan_first_complete_game_gpu_tests
				gargantuan_editor_viewport_smoke
				gargantuan_editor_viewport_presentation_benchmark
			)
		endif()
		if(GARGANTUAN_WITH_FILAMENT)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS gargantuan_filament_renderer_tests)
		endif()
		foreach(GARGANTUAN_SDL_RUNTIME_TARGET IN LISTS GARGANTUAN_SDL_RUNTIME_TARGETS)
			add_dependencies(${GARGANTUAN_SDL_RUNTIME_TARGET} gargantuan_stage_sdl_runtime)
		endforeach()
	endif()
	add_test(NAME gargantuan_foundation COMMAND gargantuan_foundation_tests)
	add_test(NAME gargantuan_asset_foundation COMMAND gargantuan_asset_foundation_tests)
	add_test(NAME gargantuan_animation_foundation COMMAND gargantuan_animation_foundation_tests)
	add_test(NAME gargantuan_animation_foundation_benchmark_smoke
		COMMAND gargantuan_animation_foundation_benchmark --quick)
	add_test(NAME gargantuan_audio_foundation COMMAND gargantuan_audio_foundation_tests)
	add_test(NAME gargantuan_audio_foundation_benchmark_smoke COMMAND gargantuan_audio_foundation_benchmark --quick)
	add_test(NAME gargantuan_renderer_publication COMMAND gargantuan_renderer_publication_tests)
	add_test(NAME gargantuan_environment_lighting_benchmark_smoke
		COMMAND gargantuan_environment_lighting_benchmark --quick)
	add_test(NAME gargantuan_optional_telemetry
		COMMAND gargantuan_optional_telemetry_tests
			"$<TARGET_FILE:gargantuan_fake_telemetry>"
			"$<TARGET_FILE:gargantuan_fake_telemetry_missing_export>"
	)
	add_test(NAME gargantuan_physics_backend COMMAND gargantuan_physics_backend_tests)
	add_test(NAME gargantuan_physics_query_benchmark_smoke COMMAND gargantuan_physics_query_benchmark --quick)
	add_test(NAME gargantuan_soft_body_physics COMMAND gargantuan_soft_body_physics_tests)
	add_test(NAME gargantuan_soft_body_physics_benchmark_smoke COMMAND gargantuan_soft_body_physics_benchmark --quick)
	add_test(NAME gargantuan_platform_input_boundary COMMAND gargantuan_platform_input_boundary_tests)
	add_test(NAME gargantuan_player_runtime COMMAND gargantuan_player_runtime_tests)
	add_test(NAME gargantuan_interaction_foundation COMMAND gargantuan_interaction_foundation_tests)
	add_test(NAME gargantuan_interaction_foundation_benchmark_smoke
		COMMAND gargantuan_interaction_foundation_benchmark --quick)
	add_test(NAME gargantuan_entitlement_service COMMAND gargantuan_entitlement_service_tests)
	add_test(NAME gargantuan_entitlement_provider_benchmark_smoke
		COMMAND gargantuan_entitlement_provider_benchmark --quick)
	add_test(NAME gargantuan_gui_foundation COMMAND gargantuan_gui_foundation_tests)
	add_test(NAME gargantuan_first_complete_game COMMAND gargantuan_first_complete_game_tests)
	add_test(NAME gargantuan_packaging COMMAND gargantuan_packaging_tests)
	add_test(NAME gargantuan_packaging_editor_host COMMAND gargantuan_packaging_editor_host_tests)
	add_test(NAME gargantuan_packaging_benchmark_smoke COMMAND gargantuan_packaging_benchmark --quick)
	add_test(NAME gargantuan_first_complete_game_package_smoke
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_PACKAGER=$<TARGET_FILE:gargantuan_packager>
			-DGARGANTUAN_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame
			-DGARGANTUAN_PACKAGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-tests
			-DGARGANTUAN_RUN_GRAPHICAL_SMOKE=$<BOOL:${GARGANTUAN_RUN_GRAPHICAL_PACKAGE_SMOKE}>
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/FirstCompleteGamePackageTests.cmake
	)
	add_test(NAME gargantuan_gui_foundation_benchmark_smoke COMMAND gargantuan_gui_foundation_benchmark --quick)
	add_test(NAME gargantuan_prerun_bootstrap COMMAND gargantuan_prerun_bootstrap_tests)
	add_test(NAME gargantuan_networking_contracts COMMAND gargantuan_networking_contract_tests)
	add_test(NAME gargantuan_simulated_transport COMMAND gargantuan_simulated_transport_tests)
	add_test(NAME gargantuan_scheduler_contract COMMAND gargantuan_scheduler_contract_tests)
	add_test(NAME gargantuan_replication COMMAND gargantuan_replication_tests)
	add_test(NAME gargantuan_remote COMMAND gargantuan_remote_tests)
	add_test(NAME gargantuan_remote_luau COMMAND gargantuan_remote_luau_tests)
	add_test(NAME gargantuan_replication_benchmark_smoke COMMAND gargantuan_replication_benchmark)
	add_test(NAME gargantuan_remote_benchmark_smoke COMMAND gargantuan_remote_benchmark)
	set_tests_properties(gargantuan_shader_tool_validation PROPERTIES LABELS "build;shader" TIMEOUT 60)
	set_tests_properties(gargantuan_foundation PROPERTIES
		LABELS "foundation;runtime;serialization;editorhost;protocol;renderer" TIMEOUT 300
	)
	set_tests_properties(gargantuan_asset_foundation PROPERTIES
		LABELS "assets;runtime;serialization;editorhost;renderer;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_audio_foundation PROPERTIES
		LABELS "audio;assets;runtime;serialization;headless;lifecycle" TIMEOUT 180
	)
	set_tests_properties(gargantuan_audio_foundation_benchmark_smoke PROPERTIES
		LABELS "audio;assets;runtime;benchmark;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_renderer_publication PROPERTIES LABELS "renderer;runtime;headless" TIMEOUT 120)
	set_tests_properties(gargantuan_environment_lighting_benchmark_smoke PROPERTIES
		LABELS "environment;renderer;benchmark;headless" TIMEOUT 120)
	set_tests_properties(gargantuan_optional_telemetry PROPERTIES LABELS "telemetry;runtime;fail-open" TIMEOUT 60)
	set_tests_properties(gargantuan_physics_backend PROPERTIES LABELS "physics;runtime" TIMEOUT 120)
	set_tests_properties(gargantuan_physics_query_benchmark_smoke PROPERTIES
		LABELS "physics;query;benchmark;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_soft_body_physics PROPERTIES LABELS "physics;deformable;renderer;serialization;headless" TIMEOUT 180)
	set_tests_properties(gargantuan_soft_body_physics_benchmark_smoke PROPERTIES LABELS "physics;deformable;benchmark;headless" TIMEOUT 120)
	set_tests_properties(gargantuan_platform_input_boundary PROPERTIES LABELS "platform;input;runtime" TIMEOUT 120)
	set_tests_properties(gargantuan_player_runtime PROPERTIES LABELS "player;runtime;headless" TIMEOUT 180)
	set_tests_properties(gargantuan_interaction_foundation PROPERTIES
		LABELS "interaction;player;input;gui;runtime;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_interaction_foundation_benchmark_smoke PROPERTIES
		LABELS "interaction;benchmark;runtime;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_entitlement_service PROPERTIES LABELS "entitlements;runtime;security;headless" TIMEOUT 120)
	set_tests_properties(gargantuan_entitlement_provider_benchmark_smoke PROPERTIES LABELS "entitlements;benchmark" TIMEOUT 120)
	set_tests_properties(gargantuan_first_complete_game PROPERTIES
		LABELS "game;sample;studio;assets;audio;player;physics;gui;interaction;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_packaging PROPERTIES
		LABELS "packaging;runtime;assets;audio;security;headless" TIMEOUT 240
	)
	set_tests_properties(gargantuan_packaging_editor_host PROPERTIES
		LABELS "packaging;editorhost;protocol;studio;runtime;headless" TIMEOUT 120
	)
	set_tests_properties(gargantuan_packaging_benchmark_smoke PROPERTIES
		LABELS "packaging;benchmark;runtime;headless" TIMEOUT 120
	)
	set_tests_properties(gargantuan_first_complete_game_package_smoke PROPERTIES
		LABELS "packaging;game;runtime;interaction;relocation;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_prerun_bootstrap PROPERTIES LABELS "prerun;runtime" TIMEOUT 120)
	set_tests_properties(gargantuan_networking_contracts gargantuan_simulated_transport gargantuan_scheduler_contract
		gargantuan_replication gargantuan_remote gargantuan_remote_luau gargantuan_replication_benchmark_smoke
		gargantuan_remote_benchmark_smoke PROPERTIES LABELS "networking;runtime" TIMEOUT 180
	)
	if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
		set_tests_properties(gargantuan_renderer_projection_headless gargantuan_renderer_backend_headless PROPERTIES
			LABELS "renderer;headless" TIMEOUT 120
		)
	endif()
	if(GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS)
		set_tests_properties(SerializationBenchmarkSmoke PROPERTIES LABELS "serialization;runtime" TIMEOUT 180)
	endif()
	if(GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE)
		set_tests_properties(GlazeSerializationTests PROPERTIES LABELS "serialization;experimental" TIMEOUT 180)
	endif()
	if(GARGANTUAN_WITH_GNS)
		add_test(NAME gargantuan_real_transport COMMAND gargantuan_real_transport_tests)
		add_test(NAME gargantuan_remote_real_transport COMMAND gargantuan_remote_real_transport_tests)
		set_tests_properties(gargantuan_real_transport gargantuan_remote_real_transport PROPERTIES
			LABELS "networking;transport;optional" TIMEOUT 180
		)
	endif()
	if(GARGANTUAN_WITH_FILAMENT)
		add_test(NAME gargantuan_filament_renderer COMMAND gargantuan_filament_renderer_tests)
		set_tests_properties(gargantuan_filament_renderer PROPERTIES LABELS "renderer;gpu;optional" TIMEOUT 180)
	endif()
endif()

add_custom_command(TARGET gargantuan POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_BINARY_DIR}/runtime"
		"$<TARGET_FILE_DIR:gargantuan>/runtime"
	COMMENT "Copying engine-shipped Luau runtime modules next to Gargantuan"
)

if(WIN32 AND NOT BUILD_TESTING)
	add_custom_command(TARGET gargantuan POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:SDL3::SDL3>"
			"$<TARGET_FILE_DIR:gargantuan>"
		COMMENT "Copying SDL3.dll next to gargantuan.exe"
	)

endif()
