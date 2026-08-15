# CMakeLists.txt for Gargantuan Engine:
# https://github.com/teamfireworks/gargantuan/

cmake_minimum_required(VERSION 3.25)
project(gargantuan CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

################################################################################
# Shaders
################################################################################

find_program(GLSL_COMPILER glslc REQUIRED)
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

set(SHADER_OUTPUTS "")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(SHADER IN LISTS SHADER_SOURCES)
	get_filename_component(SHADER_NAME ${SHADER} NAME)

	set(SPIRV_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
	add_custom_command(
		OUTPUT ${SPIRV_OUTPUT}
		COMMAND ${GLSL_COMPILER} ${SHADER} -o ${SPIRV_OUTPUT}
		DEPENDS ${SHADER}
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

################################################################################
# Tracy
################################################################################

option(GARGANTUAN_TRACY "Compile in the Tracy client" OFF)
option(GARGANTUAN_BUILD_RENDERER_BENCHMARKS "Build the manual renderer feasibility benchmark" ON)
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
if(NOT GARGANTUAN_WITH_FILAMENT)
	list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/render/FilamentRenderer.cpp")
endif()

add_library(gargantuan_core OBJECT ${GARGANTUAN_SOURCES})
add_executable(gargantuan "src/Main.cpp" ${SHADER_OUTPUTS})
target_link_libraries(gargantuan PRIVATE gargantuan_core)

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
	"vendor/glaze/include"
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
		glaze::glaze
		nlohmann_json
		Luau.Compiler
		Luau.CodeGen
		Luau.Ast
		Luau.VM
		Luau.Require
		Luau.Config
		Tracy::TracyClient
)

include(CTest)
if(BUILD_TESTING)
	add_executable(gargantuan_foundation_tests "tests/FoundationTests.cpp")
	target_link_libraries(gargantuan_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_prerun_bootstrap_tests "tests/PreRunBootstrapTests.cpp")
	target_link_libraries(gargantuan_prerun_bootstrap_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_attributes_tags_profile "tests/AttributesTagsProfile.cpp")
	target_link_libraries(gargantuan_attributes_tags_profile PRIVATE gargantuan_core)
	add_executable(gargantuan_networking_contract_tests "tests/NetworkingContractsTests.cpp")
	target_link_libraries(gargantuan_networking_contract_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_simulated_transport_tests "tests/SimulatedTransportTests.cpp")
	target_link_libraries(gargantuan_simulated_transport_tests PRIVATE gargantuan_core)
	if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
		add_executable(gargantuan_renderer_benchmark "tests/RendererBenchmark.cpp")
		target_link_libraries(gargantuan_renderer_benchmark PRIVATE gargantuan_core)
	endif()
	if(GARGANTUAN_WITH_FILAMENT)
		add_executable(gargantuan_filament_renderer_tests "tests/FilamentRendererTests.cpp")
		target_link_libraries(gargantuan_filament_renderer_tests PRIVATE gargantuan_core)
	endif()
	if(WIN32)
		add_custom_command(TARGET gargantuan_foundation_tests POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:SDL3::SDL3>"
				"$<TARGET_FILE_DIR:gargantuan_foundation_tests>"
		)
		add_custom_command(TARGET gargantuan_prerun_bootstrap_tests POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:SDL3::SDL3>"
				"$<TARGET_FILE_DIR:gargantuan_prerun_bootstrap_tests>"
		)
		add_custom_command(TARGET gargantuan_attributes_tags_profile POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:SDL3::SDL3>"
				"$<TARGET_FILE_DIR:gargantuan_attributes_tags_profile>"
		)
		add_custom_command(TARGET gargantuan_networking_contract_tests POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:SDL3::SDL3>"
				"$<TARGET_FILE_DIR:gargantuan_networking_contract_tests>"
		)
		add_custom_command(TARGET gargantuan_simulated_transport_tests POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:SDL3::SDL3>"
				"$<TARGET_FILE_DIR:gargantuan_simulated_transport_tests>"
		)
		if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
			add_custom_command(TARGET gargantuan_renderer_benchmark POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"$<TARGET_FILE:SDL3::SDL3>"
					"$<TARGET_FILE_DIR:gargantuan_renderer_benchmark>"
			)
		endif()
		if(GARGANTUAN_WITH_FILAMENT)
			add_custom_command(TARGET gargantuan_filament_renderer_tests POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"$<TARGET_FILE:SDL3::SDL3>"
					"$<TARGET_FILE_DIR:gargantuan_filament_renderer_tests>"
			)
		endif()
	endif()
	add_test(NAME gargantuan_foundation COMMAND gargantuan_foundation_tests)
	add_test(NAME gargantuan_prerun_bootstrap COMMAND gargantuan_prerun_bootstrap_tests)
	add_test(NAME gargantuan_networking_contracts COMMAND gargantuan_networking_contract_tests)
	add_test(NAME gargantuan_simulated_transport COMMAND gargantuan_simulated_transport_tests)
	if(GARGANTUAN_WITH_FILAMENT)
		add_test(NAME gargantuan_filament_renderer COMMAND gargantuan_filament_renderer_tests)
	endif()
endif()

if(WIN32)
	add_custom_command(TARGET gargantuan POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:SDL3::SDL3>"
			"$<TARGET_FILE_DIR:gargantuan>"
		COMMENT "Copying SDL3.dll next to gargantuan.exe"
	)

	add_custom_command(TARGET gargantuan POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_newer 
			"${CMAKE_CURRENT_BINARY_DIR}/shaders"
			"$<TARGET_FILE_DIR:gargantuan>/shaders"
		COMMENT "Copying shaders next to gargantuan.exe"
	)
endif()
