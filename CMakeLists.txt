# CMakeLists.txt for Gargantuan Engine:
# https://github.com/teamfireworks/gargantuan/

cmake_minimum_required(VERSION 3.25)
project(gargantuan CXX)

option(GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION
	"Configure focused native geometry/asset/physics tests without shader tooling; graphical/package targets fail closed"
	OFF)

# On Windows, vcpkg appends z-applocal to each Ninja link rule. Those commands
# copy dependencies into the same output directory, so parallel links can race
# on a DLL even when the link inputs themselves are independent. Keep compile
# jobs parallel while serializing the combined link and app-local copy rules.
if(WIN32 AND CMAKE_GENERATOR MATCHES "^Ninja" AND VCPKG_APPLOCAL_DEPS)
	set_property(GLOBAL APPEND PROPERTY JOB_POOLS GargantuanVcpkgApplocalLink=1)
	set(CMAKE_JOB_POOL_LINK GargantuanVcpkgApplocalLink)
endif()

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
	"DefaultCharacterRuntime.luau"
	"DefaultCharacterAssembly.luau"
	"DefaultCharacterAnimation.luau"
	"DefaultLocomotion.luau"
	"DefaultNetworkLocomotion.luau"
	"DefaultNetworkCharacterRuntime.luau"
	"DefaultCamera.luau"
	"DefaultPlayerRuntime.luau"
	"DefaultNetworkClientRuntime.luau"
	"DefaultNetworkServerRuntime.luau"
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
if(NOT GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION)
	GargantuanFindGlslc(GARGANTUAN_GLSLC_EXECUTABLE)
endif()
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
	if(GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION)
		continue()
	endif()
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

if(GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION)
	add_custom_target(gargantuan_shaders
		COMMAND ${CMAKE_COMMAND} -E echo "[Build:HeadlessGeometry] Graphical and packaged targets require the normal shader-qualified build profile"
		COMMAND ${CMAKE_COMMAND} -E false
		VERBATIM)
else()
	add_custom_target(gargantuan_shaders DEPENDS ${SHADER_OUTPUTS})
endif()

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
# Keep the bundled static FreeType closure independent of whatever optional
# package-manager libraries happen to be visible to a host-support superbuild.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(vendor/argparse SYSTEM)
enable_language(C)
include(cmake/Box3DCompatibility.cmake)
GargantuanPrepareBox3DCompatibility("${CMAKE_CURRENT_SOURCE_DIR}/vendor/box3d" GARGANTUAN_BOX3D_SOURCE_DIR)
add_subdirectory("${GARGANTUAN_BOX3D_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-build" SYSTEM)
# Only the private implementation header changes. Keep the identical public API
# include path stable for consumers and avoid recompiling unrelated core sources.
set_property(TARGET box3d PROPERTY INTERFACE_INCLUDE_DIRECTORIES
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/box3d/include>;$<INSTALL_INTERFACE:include>")
add_subdirectory(vendor/glm SYSTEM)
# The optional codec prototype is outside focused headless geometry qualification.
# Its CMake 3.31 requirement must not become an unused native-test dependency.
if(NOT GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION OR GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE)
	add_subdirectory(vendor/glaze SYSTEM)
endif()
add_subdirectory(vendor/json SYSTEM)
add_subdirectory(vendor/luau SYSTEM)
add_subdirectory(vendor/sdl SYSTEM)
add_subdirectory(vendor/sdl_image SYSTEM)
# A parent superbuild may already have found a package-manager zlib for gRPC.
# Do not let that cached result change the bundled static FreeType ABI or leak a
# Node-only DLL into GargantuanPlayer.
if(FT_DISABLE_ZLIB)
	set(ZLIB_FOUND OFF)
endif()
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
option(GARGANTUAN_WORK_TRACE "Enable bounded local Engine work traces" OFF)
option(GARGANTUAN_BUILD_RENDERER_BENCHMARKS "Build the manual renderer feasibility benchmark" ON)
option(GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS "Build the serialization baseline benchmark" ON)
option(GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE "Build the private Glaze codec evaluation targets" OFF)
option(GARGANTUAN_WITH_GNS "Build the optional GameNetworkingSockets transport adapter" OFF)
option(GARGANTUAN_WITH_NODE_CONTENT "Build the trusted GargantuanServer Node ContentStreaming adapter" OFF)
option(GARGANTUAN_WITH_NODE_PERSISTENCE "Build the trusted GargantuanServer Node DataStore adapter" OFF)
option(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT "Compose the private Studio Node development provider" OFF)
if(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT AND NOT GARGANTUAN_WITH_NODE_PERSISTENCE)
	message(FATAL_ERROR "Private Studio Node development requires the existing Node DataStore adapter")
endif()
option(GARGANTUAN_WITH_LOCAL_SQLITE "Build durable SQLite storage for local development profiles" OFF)
set(GARGANTUAN_NODE_PROTOCOL_ROOT "" CACHE PATH "Path to gargantuan-node/protocol/proto for official Server Node support")
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

if(GARGANTUAN_WITH_NODE_CONTENT OR GARGANTUAN_WITH_NODE_PERSISTENCE)
	find_package(Protobuf CONFIG QUIET)
	if(NOT Protobuf_FOUND)
		find_package(Protobuf REQUIRED)
	endif()
	find_package(gRPC CONFIG REQUIRED)
endif()

if(GARGANTUAN_WITH_NODE_CONTENT)
	if(NOT EXISTS "${GARGANTUAN_NODE_PROTOCOL_ROOT}/content/v1/content.proto")
		message(FATAL_ERROR
			"GARGANTUAN_WITH_NODE_CONTENT requires GARGANTUAN_NODE_PROTOCOL_ROOT to name the canonical gargantuan-node protocol/proto directory")
	endif()
	set(GARGANTUAN_NODE_CONTENT_PROTO "${GARGANTUAN_NODE_PROTOCOL_ROOT}/content/v1/content.proto")
	set(GARGANTUAN_NODE_CONTENT_GENERATED_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated/node-content")
	set(GARGANTUAN_NODE_CONTENT_GENERATED_SOURCES
		"${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}/content/v1/content.pb.cc"
		"${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}/content/v1/content.grpc.pb.cc")
	set(GARGANTUAN_NODE_CONTENT_GENERATED_HEADERS
		"${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}/content/v1/content.pb.h"
		"${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}/content/v1/content.grpc.pb.h")
	add_custom_command(
		OUTPUT ${GARGANTUAN_NODE_CONTENT_GENERATED_SOURCES} ${GARGANTUAN_NODE_CONTENT_GENERATED_HEADERS}
		COMMAND ${CMAKE_COMMAND} -E make_directory "${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}/content/v1"
		COMMAND protobuf::protoc
			--proto_path "${GARGANTUAN_NODE_PROTOCOL_ROOT}"
			--cpp_out "${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}"
			--grpc_out "${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}"
			--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
			"${GARGANTUAN_NODE_CONTENT_PROTO}"
		DEPENDS "${GARGANTUAN_NODE_CONTENT_PROTO}"
		VERBATIM
	)
endif()

if(GARGANTUAN_WITH_NODE_PERSISTENCE)
	set(GARGANTUAN_NODE_PERSISTENCE_PROTO "${GARGANTUAN_NODE_PROTOCOL_ROOT}/datastore/v1/datastore.proto")
	set(GARGANTUAN_NODE_COMMON_PROTO "${GARGANTUAN_NODE_PROTOCOL_ROOT}/common/v1/common.proto")
	if(NOT EXISTS "${GARGANTUAN_NODE_PERSISTENCE_PROTO}" OR NOT EXISTS "${GARGANTUAN_NODE_COMMON_PROTO}")
		message(FATAL_ERROR "Node persistence requires the canonical datastore/v1 and common/v1 protocols")
	endif()
	get_target_property(GARGANTUAN_PROTOBUF_INCLUDES protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
	find_path(GARGANTUAN_PROTOBUF_WELL_KNOWN_TYPES google/protobuf/struct.proto
		HINTS ${GARGANTUAN_PROTOBUF_INCLUDES} ${Protobuf_INCLUDE_DIRS} REQUIRED)
	set(GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated/node-persistence")
	set(GARGANTUAN_NODE_PERSISTENCE_GENERATED_SOURCES
		"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/v1/datastore.pb.cc"
		"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/v1/datastore.grpc.pb.cc"
		"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/common/v1/common.pb.cc")
	set(GARGANTUAN_NODE_PERSISTENCE_GENERATED_HEADERS
		"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/v1/datastore.pb.h"
		"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/v1/datastore.grpc.pb.h"
		"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/common/v1/common.pb.h")
	if(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		set(GARGANTUAN_STUDIO_NODE_DEVELOPMENT_PROTO
			"${GARGANTUAN_NODE_PROTOCOL_ROOT}/datastore/development/v1/admission.proto")
		if(NOT EXISTS "${GARGANTUAN_STUDIO_NODE_DEVELOPMENT_PROTO}")
			message(FATAL_ERROR "Private Studio Node development requires the canonical development admission v1 protocol")
		endif()
		list(APPEND GARGANTUAN_NODE_PERSISTENCE_GENERATED_SOURCES
			"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/development/v1/admission.pb.cc"
			"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/development/v1/admission.grpc.pb.cc")
		list(APPEND GARGANTUAN_NODE_PERSISTENCE_GENERATED_HEADERS
			"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/development/v1/admission.pb.h"
			"${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}/datastore/development/v1/admission.grpc.pb.h")
	endif()
	add_custom_command(
		OUTPUT ${GARGANTUAN_NODE_PERSISTENCE_GENERATED_SOURCES} ${GARGANTUAN_NODE_PERSISTENCE_GENERATED_HEADERS}
		COMMAND ${CMAKE_COMMAND} -E make_directory "${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}"
		COMMAND protobuf::protoc
			--proto_path "${GARGANTUAN_NODE_PROTOCOL_ROOT}"
			--proto_path "${GARGANTUAN_PROTOBUF_WELL_KNOWN_TYPES}"
			--cpp_out "${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}"
			"${GARGANTUAN_NODE_PERSISTENCE_PROTO}" "${GARGANTUAN_NODE_COMMON_PROTO}" ${GARGANTUAN_STUDIO_NODE_DEVELOPMENT_PROTO}
		COMMAND protobuf::protoc
			--proto_path "${GARGANTUAN_NODE_PROTOCOL_ROOT}"
			--proto_path "${GARGANTUAN_PROTOBUF_WELL_KNOWN_TYPES}"
			--grpc_out "${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}"
			--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
			"${GARGANTUAN_NODE_PERSISTENCE_PROTO}" ${GARGANTUAN_STUDIO_NODE_DEVELOPMENT_PROTO}
		DEPENDS "${GARGANTUAN_NODE_PERSISTENCE_PROTO}" "${GARGANTUAN_NODE_COMMON_PROTO}" ${GARGANTUAN_STUDIO_NODE_DEVELOPMENT_PROTO}
		VERBATIM
	)
endif()

if(GARGANTUAN_WITH_LOCAL_SQLITE)
	include(FetchContent)
	enable_language(C)
	FetchContent_Declare(gargantuan_sqlite_source
		URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
		URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
		DOWNLOAD_EXTRACT_TIMESTAMP FALSE
		TLS_VERIFY TRUE)
	FetchContent_MakeAvailable(gargantuan_sqlite_source)
	add_library(gargantuan_sqlite STATIC "${gargantuan_sqlite_source_SOURCE_DIR}/sqlite3.c")
	target_include_directories(gargantuan_sqlite PUBLIC "${gargantuan_sqlite_source_SOURCE_DIR}")
	target_compile_definitions(gargantuan_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
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
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/server/ServerMain.cpp")
list(FILTER GARGANTUAN_SOURCES EXCLUDE REGEX "/src/host/")
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/serialization/GlazePrototype.cpp")
set(GARGANTUAN_TELEMETRY_HOST_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/OptionalTelemetry.cpp"
)
list(REMOVE_ITEM GARGANTUAN_SOURCES ${GARGANTUAN_TELEMETRY_HOST_SOURCES})
if(NOT GARGANTUAN_WITH_FILAMENT)
	list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/render/FilamentRenderer.cpp")
endif()
list(REMOVE_ITEM GARGANTUAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/network/GameNetworkingSocketsTransport.cpp")

include(cmake/Manifold.cmake)
add_library(gargantuan_core OBJECT ${GARGANTUAN_SOURCES})
target_link_libraries(gargantuan_core PUBLIC GargantuanManifold)
find_package(OpenSSL 3.0...<4.0 QUIET COMPONENTS Crypto)
set(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_VERSION "${OpenSSL_VERSION}")
if(NOT GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_VERSION)
	set(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_VERSION "${OPENSSL_VERSION}")
endif()
set(GARGANTUAN_RUNTIME_PACKAGE_STAGE_ARGUMENTS)
if(OpenSSL_FOUND AND TARGET OpenSSL::Crypto AND
	GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_VERSION VERSION_GREATER_EQUAL 3.0 AND
	GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_VERSION VERSION_LESS 4.0)
	target_link_libraries(gargantuan_core PUBLIC OpenSSL::Crypto)
	target_compile_definitions(gargantuan_core PRIVATE GARGANTUAN_RUNTIME_PACKAGE_OPENSSL=1)
	include(cmake/RuntimePackageOpenSSL.cmake)
	list(APPEND GARGANTUAN_RUNTIME_PACKAGE_STAGE_ARGUMENTS
		"-DGARGANTUAN_RUNTIME_PACKAGE_OPENSSL_NOTICE=${GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_NOTICE}"
		"-DGARGANTUAN_RUNTIME_PACKAGE_OPENSSL_DLL=${GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_DLL}")
endif()
if(GARGANTUAN_WITH_LOCAL_SQLITE)
	target_link_libraries(gargantuan_core PUBLIC gargantuan_sqlite)
	target_compile_definitions(gargantuan_core PUBLIC GARGANTUAN_WITH_LOCAL_SQLITE=1)
endif()
if(GARGANTUAN_WORK_TRACE)
	target_compile_definitions(gargantuan_core PUBLIC GARGANTUAN_WORK_TRACE=1)
endif()
add_executable(gargantuan "src/Main.cpp" ${GARGANTUAN_TELEMETRY_HOST_SOURCES} ${SHADER_OUTPUTS})
target_link_libraries(gargantuan PRIVATE gargantuan_core ${CMAKE_DL_LIBS})
if(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_DLL)
	add_custom_target(gargantuan_stage_package_openssl
		COMMAND ${CMAKE_COMMAND}
			"-DGARGANTUAN_RUNTIME_PACKAGE_OPENSSL_DLL=${GARGANTUAN_RUNTIME_PACKAGE_OPENSSL_DLL}"
			"-DGARGANTUAN_RUNTIME_PACKAGE_APP_LOCAL=$<TARGET_FILE_DIR:gargantuan>"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimePackageLibrary.cmake"
		COMMENT "Staging OpenSSL Crypto next to Gargantuan executables"
		VERBATIM)
	add_dependencies(gargantuan_core gargantuan_stage_package_openssl)
endif()
add_library(gargantuan_runtime_host STATIC "src/host/common/PackagedHost.cpp")
target_link_libraries(gargantuan_runtime_host PUBLIC gargantuan_core)
target_include_directories(gargantuan_runtime_host PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
add_library(gargantuan_player_host STATIC "src/host/player/PlayerHost.cpp")
target_link_libraries(gargantuan_player_host PUBLIC gargantuan_runtime_host)
add_executable(gargantuan_player "src/player/PlayerMain.cpp")
set_target_properties(gargantuan_player PROPERTIES OUTPUT_NAME "GargantuanPlayer")
if(APPLE)
	set_target_properties(gargantuan_player PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
	set_target_properties(gargantuan_player PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
target_link_libraries(gargantuan_player PRIVATE gargantuan_player_host gargantuan_core ${CMAKE_DL_LIBS})
add_library(gargantuan_server_host STATIC "src/host/server/ServerHost.cpp")
target_link_libraries(gargantuan_server_host PUBLIC gargantuan_runtime_host)
if(GARGANTUAN_WITH_NODE_CONTENT)
	add_library(gargantuan_node_content_provider STATIC
		"src/host/server/NodeContentProvider.cpp"
		${GARGANTUAN_NODE_CONTENT_GENERATED_SOURCES}
		${GARGANTUAN_NODE_CONTENT_GENERATED_HEADERS}
	)
	target_include_directories(gargantuan_node_content_provider
		PRIVATE "${GARGANTUAN_NODE_CONTENT_GENERATED_ROOT}")
	target_link_libraries(gargantuan_node_content_provider
		PUBLIC gargantuan_core
		PRIVATE gRPC::grpc++ protobuf::libprotobuf)
	target_link_libraries(gargantuan_server_host PRIVATE gargantuan_node_content_provider)
	target_compile_definitions(gargantuan_server_host PRIVATE GARGANTUAN_WITH_NODE_CONTENT=1)
endif()
if(GARGANTUAN_WITH_NODE_PERSISTENCE)
	add_library(gargantuan_node_persistence_provider STATIC
		"src/host/server/NodePersistenceProvider.cpp"
		${GARGANTUAN_NODE_PERSISTENCE_GENERATED_SOURCES}
		${GARGANTUAN_NODE_PERSISTENCE_GENERATED_HEADERS})
	target_include_directories(gargantuan_node_persistence_provider PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src" "${GARGANTUAN_NODE_PERSISTENCE_GENERATED_ROOT}")
	target_link_libraries(gargantuan_node_persistence_provider PUBLIC gargantuan_core
		PRIVATE gRPC::grpc++ protobuf::libprotobuf)
	target_link_libraries(gargantuan_server_host PRIVATE gargantuan_node_persistence_provider)
	target_compile_definitions(gargantuan_server_host PRIVATE GARGANTUAN_WITH_NODE_PERSISTENCE=1)
	if(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		target_compile_definitions(gargantuan_node_persistence_provider PUBLIC GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT=1)
		target_link_libraries(gargantuan PRIVATE gargantuan_node_persistence_provider nlohmann_json)
		target_sources(gargantuan PRIVATE "src/host/editor/NodeDevelopmentProfile.cpp")
		target_include_directories(gargantuan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	endif()
endif()
add_executable(gargantuan_server "src/server/ServerMain.cpp")
set_target_properties(gargantuan_server PROPERTIES OUTPUT_NAME "GargantuanServer")
if(APPLE)
	set_target_properties(gargantuan_server PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
	set_target_properties(gargantuan_server PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
target_link_libraries(gargantuan_server PRIVATE gargantuan_server_host gargantuan_core ${CMAKE_DL_LIBS})
add_executable(gargantuan_packager "tools/PackagerMain.cpp")
set_target_properties(gargantuan_packager PROPERTIES OUTPUT_NAME "gargantuan-packager")
target_link_libraries(gargantuan_packager PRIVATE gargantuan_core nlohmann_json ${CMAKE_DL_LIBS})

# Shader generation and executable-local staging are separate dependencies.
if(GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION)
	foreach(GARGANTUAN_GRAPHICAL_TARGET IN ITEMS gargantuan gargantuan_player gargantuan_server)
		add_dependencies(${GARGANTUAN_GRAPHICAL_TARGET} gargantuan_shaders)
	endforeach()
endif()
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
	# Microsoft supplies the centrally serviced prerequisite directly to recipients.
	if(CMAKE_CXX_COMPILER MATCHES "/[Tt]ools/MSVC/(14[.][0-9]+[.][0-9]+)/")
		set(GARGANTUAN_MSVC_RUNTIME_MINIMUM "${CMAKE_MATCH_1}")
	elseif(DEFINED ENV{VCToolsVersion} AND "$ENV{VCToolsVersion}" MATCHES "^(14[.][0-9]+[.][0-9]+)")
		set(GARGANTUAN_MSVC_RUNTIME_MINIMUM "${CMAKE_MATCH_1}")
	else()
		string(REGEX REPLACE "^19[.]" "14." GARGANTUAN_MSVC_RUNTIME_MINIMUM "${CMAKE_CXX_COMPILER_VERSION}")
	endif()
	list(APPEND GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS
		"-DGARGANTUAN_MSVC_RUNTIME_MINIMUM=${GARGANTUAN_MSVC_RUNTIME_MINIMUM}")
endif()
set(GARGANTUAN_NETWORK_RUNTIME_ARGUMENTS)
set(GARGANTUAN_IMAGE_NOTICE_CODECS)
foreach(Codec GIF QOI SVG BACKEND_STB JPG_SAVE)
	if(SDLIMAGE_${Codec})
		list(APPEND GARGANTUAN_IMAGE_NOTICE_CODECS "${Codec}")
	endif()
endforeach()
list(JOIN GARGANTUAN_IMAGE_NOTICE_CODECS "|" GARGANTUAN_IMAGE_NOTICE_CODECS)
set(GARGANTUAN_SDL_NOTICE_FEATURES)
foreach(Feature HIDAPI OPENGL OPENGLES VULKAN)
	if(SDL_${Feature})
		list(APPEND GARGANTUAN_SDL_NOTICE_FEATURES "${Feature}")
	endif()
endforeach()
list(JOIN GARGANTUAN_SDL_NOTICE_FEATURES "|" GARGANTUAN_SDL_NOTICE_FEATURES)
if(GARGANTUAN_WITH_GNS)
	list(APPEND GARGANTUAN_NETWORK_RUNTIME_ARGUMENTS
		-DGARGANTUAN_GNS_REFERENCE_DONNA=$<STREQUAL:${USE_CRYPTO25519},Reference>
		-DGARGANTUAN_GNS_LICENSE_FILE=${GameNetworkingSockets_SOURCE_DIR}/LICENSE
		-DGARGANTUAN_PROTOBUF_LICENSE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/notices/ProtocolBuffers.txt
	)
	if(TARGET protobuf::libprotobuf)
		list(APPEND GARGANTUAN_NETWORK_RUNTIME_ARGUMENTS
			-DGARGANTUAN_PROTOBUF_RUNTIME_FILE=$<TARGET_FILE:protobuf::libprotobuf>)
	elseif(WIN32 AND DEFINED Protobuf_DIR)
		get_filename_component(GARGANTUAN_PROTOBUF_PACKAGE_ROOT "${Protobuf_DIR}/../.." ABSOLUTE)
		if(CMAKE_BUILD_TYPE STREQUAL "Debug")
			set(GARGANTUAN_PROTOBUF_RUNTIME_FILE
				"${GARGANTUAN_PROTOBUF_PACKAGE_ROOT}/debug/bin/libprotobufd.dll")
		else()
			set(GARGANTUAN_PROTOBUF_RUNTIME_FILE
				"${GARGANTUAN_PROTOBUF_PACKAGE_ROOT}/bin/libprotobuf.dll")
		endif()
		if(NOT EXISTS "${GARGANTUAN_PROTOBUF_RUNTIME_FILE}")
			message(FATAL_ERROR "The GameNetworkingSockets Protocol Buffers runtime is unavailable")
		endif()
		list(APPEND GARGANTUAN_NETWORK_RUNTIME_ARGUMENTS
			-DGARGANTUAN_PROTOBUF_RUNTIME_FILE=${GARGANTUAN_PROTOBUF_RUNTIME_FILE})
	endif()
endif()
set(GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS)
if((GARGANTUAN_WITH_NODE_CONTENT OR GARGANTUAN_WITH_NODE_PERSISTENCE) AND WIN32)
	list(APPEND GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS
		"-DGARGANTUAN_OPTIONAL_RUNTIME_FILES=$<JOIN:$<TARGET_RUNTIME_DLLS:gargantuan_server>,|>")
	if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
		set(GARGANTUAN_NODE_REQUIRED_RUNTIME_COUNT 0)
		foreach(GARGANTUAN_NODE_RUNTIME_NAMES IN ITEMS
			"libcrypto-3-x64.dll|libcrypto-3-x64.dll"
			"libssl-3-x64.dll|libssl-3-x64.dll"
			"zlib1.dll|zlibd1.dll")
			string(REPLACE "|" ";" GARGANTUAN_NODE_RUNTIME_NAME_PAIR
				"${GARGANTUAN_NODE_RUNTIME_NAMES}")
			list(GET GARGANTUAN_NODE_RUNTIME_NAME_PAIR 0 GARGANTUAN_NODE_RELEASE_RUNTIME_NAME)
			list(GET GARGANTUAN_NODE_RUNTIME_NAME_PAIR 1 GARGANTUAN_NODE_DEBUG_RUNTIME_NAME)
			set(GARGANTUAN_NODE_RELEASE_RUNTIME_FILE
				"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/${GARGANTUAN_NODE_RELEASE_RUNTIME_NAME}")
			set(GARGANTUAN_NODE_DEBUG_RUNTIME_FILE
				"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/${GARGANTUAN_NODE_DEBUG_RUNTIME_NAME}")
			if(NOT EXISTS "${GARGANTUAN_NODE_RELEASE_RUNTIME_FILE}" OR
				NOT EXISTS "${GARGANTUAN_NODE_DEBUG_RUNTIME_FILE}")
				message(FATAL_ERROR
					"A required Node provider runtime is unavailable: ${GARGANTUAN_NODE_RUNTIME_NAMES}")
			endif()
			list(APPEND GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS
				"-DGARGANTUAN_NODE_REQUIRED_RUNTIME_FILE_${GARGANTUAN_NODE_REQUIRED_RUNTIME_COUNT}=$<IF:$<CONFIG:Debug>,${GARGANTUAN_NODE_DEBUG_RUNTIME_FILE},${GARGANTUAN_NODE_RELEASE_RUNTIME_FILE}>")
			math(EXPR GARGANTUAN_NODE_REQUIRED_RUNTIME_COUNT
				"${GARGANTUAN_NODE_REQUIRED_RUNTIME_COUNT} + 1")
		endforeach()
		list(APPEND GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS
			-DGARGANTUAN_NODE_REQUIRED_RUNTIME_COUNT=${GARGANTUAN_NODE_REQUIRED_RUNTIME_COUNT})
		set(GARGANTUAN_NODE_NOTICE_COUNT 0)
		foreach(GARGANTUAN_NODE_NOTICE_PACKAGE IN ITEMS abseil c-ares grpc openssl re2 upb zlib)
			set(GARGANTUAN_NODE_NOTICE_FILE
				"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${GARGANTUAN_NODE_NOTICE_PACKAGE}/copyright")
			if(EXISTS "${GARGANTUAN_NODE_NOTICE_FILE}")
				list(APPEND GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS
					"-DGARGANTUAN_OPTIONAL_NOTICE_FILE_${GARGANTUAN_NODE_NOTICE_COUNT}=${GARGANTUAN_NODE_NOTICE_FILE}"
					"-DGARGANTUAN_OPTIONAL_NOTICE_NAME_${GARGANTUAN_NODE_NOTICE_COUNT}=${GARGANTUAN_NODE_NOTICE_PACKAGE}.txt")
				math(EXPR GARGANTUAN_NODE_NOTICE_COUNT "${GARGANTUAN_NODE_NOTICE_COUNT} + 1")
			endif()
		endforeach()
		list(APPEND GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS
			-DGARGANTUAN_OPTIONAL_NOTICE_COUNT=${GARGANTUAN_NODE_NOTICE_COUNT})
	endif()
endif()
set(GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS "" CACHE FILEPATH
    "Recipient instructions for the exact covered source, including local changes (optional for Git builds)")
set(GARGANTUAN_COVERED_SOURCE_ARCHIVE "" CACHE FILEPATH "Reviewed matching source ZIP to embed in each export")
set(GARGANTUAN_RUNTIME_SOURCE_REVISION "" CACHE STRING "Full Engine build revision recorded in the reviewed ZIP")
option(GARGANTUAN_EXTERNAL_DISTRIBUTION "Require a release-specific source route when staging runtimes" OFF)
if(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT AND GARGANTUAN_EXTERNAL_DISTRIBUTION)
	message(FATAL_ERROR "The private Studio Node development composition cannot be staged for external distribution")
endif()
add_custom_target(gargantuan_runtime_distribution ALL
	COMMAND ${CMAKE_COMMAND}
		-DGARGANTUAN_RUNTIME_DISTRIBUTION_ROOT=$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution
		-DGARGANTUAN_PLAYER_FILE=$<TARGET_FILE:gargantuan_player>
		-DGARGANTUAN_STAGE_DISTRIBUTED_PLAYER=${GARGANTUAN_WITH_GNS}
		"-DGARGANTUAN_IMAGE_NOTICE_CODECS=${GARGANTUAN_IMAGE_NOTICE_CODECS}"
		"-DGARGANTUAN_SDL_NOTICE_FEATURES=${GARGANTUAN_SDL_NOTICE_FEATURES}"
		-DGARGANTUAN_EXTERNAL_DISTRIBUTION=${GARGANTUAN_EXTERNAL_DISTRIBUTION}
		-DGARGANTUAN_COVERED_SOURCE_INSTRUCTIONS=${GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS}
		-DGARGANTUAN_COVERED_SOURCE_ARCHIVE=${GARGANTUAN_COVERED_SOURCE_ARCHIVE}
		-DGARGANTUAN_RUNTIME_SOURCE_REVISION=${GARGANTUAN_RUNTIME_SOURCE_REVISION}
		-DGARGANTUAN_SDL_FILE=$<TARGET_FILE:SDL3::SDL3>
		${GARGANTUAN_SDL_SONAME_ARGUMENT}
		${GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS}
		${GARGANTUAN_NETWORK_RUNTIME_ARGUMENTS}
		${GARGANTUAN_RUNTIME_PACKAGE_STAGE_ARGUMENTS}
		-DGARGANTUAN_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		-DGARGANTUAN_PLATFORM=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeDistribution.cmake
	DEPENDS gargantuan_player gargantuan_shaders
	COMMENT "Staging the canonical Gargantuan player runtime distribution"
	VERBATIM
)
add_dependencies(gargantuan_packager gargantuan_runtime_distribution)
add_custom_target(gargantuan_server_runtime_distribution ALL
	COMMAND ${CMAKE_COMMAND}
		-DGARGANTUAN_RUNTIME_DISTRIBUTION_ROOT=$<TARGET_FILE_DIR:gargantuan_server>/ServerRuntimeDistribution
		-DGARGANTUAN_PLAYER_FILE=$<TARGET_FILE:gargantuan_server>
		"-DGARGANTUAN_IMAGE_NOTICE_CODECS=${GARGANTUAN_IMAGE_NOTICE_CODECS}"
		"-DGARGANTUAN_SDL_NOTICE_FEATURES=${GARGANTUAN_SDL_NOTICE_FEATURES}"
		-DGARGANTUAN_EXTERNAL_DISTRIBUTION=${GARGANTUAN_EXTERNAL_DISTRIBUTION}
		-DGARGANTUAN_COVERED_SOURCE_INSTRUCTIONS=${GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS}
		-DGARGANTUAN_COVERED_SOURCE_ARCHIVE=${GARGANTUAN_COVERED_SOURCE_ARCHIVE}
		-DGARGANTUAN_RUNTIME_SOURCE_REVISION=${GARGANTUAN_RUNTIME_SOURCE_REVISION}
		-DGARGANTUAN_SDL_FILE=$<TARGET_FILE:SDL3::SDL3>
		${GARGANTUAN_SDL_SONAME_ARGUMENT}
		${GARGANTUAN_SYSTEM_RUNTIME_ARGUMENTS}
		${GARGANTUAN_NETWORK_RUNTIME_ARGUMENTS}
		${GARGANTUAN_NODE_SERVER_RUNTIME_ARGUMENTS}
		${GARGANTUAN_RUNTIME_PACKAGE_STAGE_ARGUMENTS}
		-DGARGANTUAN_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		-DGARGANTUAN_PLATFORM=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeDistribution.cmake
	DEPENDS gargantuan_server gargantuan_shaders
	COMMENT "Staging the Gargantuan dedicated-server runtime distribution"
	VERBATIM
)
add_dependencies(gargantuan_packager gargantuan_server_runtime_distribution)

if(MSVC)
	target_compile_options(gargantuan_core PUBLIC /EHsc)
endif()

# GCC 13+ needs this option for Engine headers that expose established member names.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(gargantuan_core PUBLIC -Wno-changes-meaning)
endif()

target_include_directories(gargantuan_core PUBLIC
	"src"
	"include"
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(GARGANTUAN_WITH_FILAMENT)
	set(FILAMENT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/filament")
	file(MAKE_DIRECTORY "${FILAMENT_GENERATED_DIR}")
	function(gargantuan_filament_material FILAMENT_MATERIAL_NAME FILAMENT_BLEND_MODE)
		set(FILAMENT_PREMULTIPLY "")
		if(FILAMENT_BLEND_MODE STREQUAL "transparent")
			set(FILAMENT_PREMULTIPLY "material.baseColor.rgb *= material.baseColor.a;")
		endif()
		set(MaterialInput "${FILAMENT_GENERATED_DIR}/${FILAMENT_MATERIAL_NAME}.mat")
		set(MaterialHeader "${FILAMENT_GENERATED_DIR}/${FILAMENT_MATERIAL_NAME}.h")
		configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/filament/benchmark.mat.in" "${MaterialInput}" @ONLY)
		add_custom_command(
			OUTPUT "${MaterialHeader}"
			COMMAND "${GARGANTUAN_FILAMENT_ROOT}/bin/matc.exe"
				--platform desktop
				--api vulkan
				--variant-filter=dynamicLighting,skinning,vsm,fog,ssr,stereo
				--output-format header
				--output "${MaterialHeader}"
				"${MaterialInput}"
			DEPENDS "${MaterialInput}" "${GARGANTUAN_FILAMENT_ROOT}/bin/matc.exe"
			COMMENT "Compiling the experimental Filament ${FILAMENT_BLEND_MODE} material"
			VERBATIM
		)
		target_sources(gargantuan_core PRIVATE "${MaterialHeader}")
	endfunction()
	gargantuan_filament_material(GargantuanBenchmarkMaterial opaque)
	gargantuan_filament_material(GargantuanMaskedMaterial masked)
	gargantuan_filament_material(GargantuanTransparentMaterial transparent)
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
		Luau.VM.Internals
		nlohmann_json
)

if(GARGANTUAN_WITH_GNS)
	add_library(gargantuan_gns_transport STATIC
		"src/network/GameNetworkingSocketsTransport.cpp"
	)
	target_compile_features(gargantuan_gns_transport PUBLIC cxx_std_23)
	target_compile_definitions(gargantuan_gns_transport PUBLIC GARGANTUAN_WITH_GNS=1)
	if(GARGANTUAN_WORK_TRACE)
		target_compile_definitions(gargantuan_gns_transport PRIVATE GARGANTUAN_WORK_TRACE=1)
	endif()
	target_include_directories(gargantuan_gns_transport PUBLIC "include")
	target_include_directories(gargantuan_gns_transport PRIVATE "cmake/gns")
	target_link_libraries(gargantuan_gns_transport PUBLIC GameNetworkingSockets::static)
	target_link_libraries(gargantuan_player_host PUBLIC gargantuan_gns_transport)
	target_link_libraries(gargantuan_server_host PUBLIC gargantuan_gns_transport)
	if(MSVC)
		target_compile_options(gargantuan_gns_transport PRIVATE /EHsc)
	endif()
endif()

include(CTest)
if(BUILD_TESTING)
	foreach(TerrainTest IN ITEMS
		"tests/terrain/TerrainFieldTests.cpp"
		"tests/terrain/TerrainCoordinatorTests.cpp"
		"tests/terrain/TerrainRenderTests.cpp"
		"tests/terrain/TerrainRuntimeTests.cpp"
		"tests/terrain/lod/TerrainVisualDensityRender.cpp"
		"tests/terrain/storage/HeightColumnQualification.cpp"
		"tests/terrain/surface/TerrainSurfaceTests.cpp"
		"tests/terrain/physics/TerrainPhysicsStageTests.cpp"
		"tests/terrain/metadata/ActiveChunkRegistryTests.cpp"
		"tests/terrain/codec/ChunkCodecTests.cpp"
		"tests/terrain/codec/TerrainManifestTests.cpp"
		"tests/terrain/codec/TerrainPersistenceTests.cpp")
		get_filename_component(TerrainName "${TerrainTest}" NAME_WE)
		string(TOLOWER "gargantuan_${TerrainName}" TerrainTarget)
		add_executable("${TerrainTarget}" "${TerrainTest}")
		target_include_directories("${TerrainTarget}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
		target_link_libraries("${TerrainTarget}" PRIVATE gargantuan_core)
		add_test(NAME "${TerrainTarget}" COMMAND "${TerrainTarget}")
		set_tests_properties("${TerrainTarget}" PROPERTIES LABELS "terrain;headless" TIMEOUT 60)
	endforeach()
	add_executable(gargantuan_terrainlodqualification "tests/terrain/lod/TerrainLodQualification.cpp")
	target_include_directories(gargantuan_terrainlodqualification PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_libraries(gargantuan_terrainlodqualification PRIVATE gargantuan_core)
	add_test(NAME gargantuan_terrainlodqualification COMMAND gargantuan_terrainlodqualification --compare-indexed)
	set_tests_properties(gargantuan_terrainlodqualification PROPERTIES LABELS "terrain;headless" TIMEOUT 60)
	add_executable(gargantuan_terrain_lod_offscreen EXCLUDE_FROM_ALL "tests/terrain/lod/TerrainLodOffscreen.cpp")
	target_include_directories(gargantuan_terrain_lod_offscreen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_libraries(gargantuan_terrain_lod_offscreen PRIVATE gargantuan_core)
	add_dependencies(gargantuan_terrain_lod_offscreen gargantuan_shaders)
	add_executable(gargantuan_runtime_package_crypto_tests "tests/RuntimePackageCryptoTests.cpp")
	target_link_libraries(gargantuan_runtime_package_crypto_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_runtime_package_crypto COMMAND gargantuan_runtime_package_crypto_tests)
	set_tests_properties(gargantuan_runtime_package_crypto PROPERTIES LABELS "runtime;packaging;security")
	add_executable(gargantuan_gas_document_tests "tests/GasDocumentTests.cpp")
	target_link_libraries(gargantuan_gas_document_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_gas_document COMMAND gargantuan_gas_document_tests)
	set_tests_properties(gargantuan_gas_document PROPERTIES LABELS "persistence;serialization;sanitizer-safety")
	add_executable(gargantuan_gas_authoring_tests "tests/GasAuthoringTests.cpp")
	target_link_libraries(gargantuan_gas_authoring_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_gas_authoring COMMAND gargantuan_gas_authoring_tests)
	set_tests_properties(gargantuan_gas_authoring PROPERTIES LABELS "persistence;editor;serialization")
	add_test(NAME gargantuan_gas_animated_model COMMAND gargantuan_gas_authoring_tests --model-fixture
		"${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame" AnimatedBeacon)
	set_tests_properties(gargantuan_gas_animated_model PROPERTIES LABELS "persistence;editor;assets")
	if(GARGANTUAN_WORK_TRACE)
		add_executable(gargantuan_work_trace_tests "tests/RuntimeWorkTraceTests.cpp")
		target_link_libraries(gargantuan_work_trace_tests PRIVATE gargantuan_core)
		add_test(NAME gargantuan_work_trace COMMAND gargantuan_work_trace_tests)
	endif()
	add_test(NAME gargantuan_runtime_host_roles
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_PLAYER=$<TARGET_FILE:gargantuan_player>
			-DGARGANTUAN_SERVER=$<TARGET_FILE:gargantuan_server>
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/RuntimeHostRoleTests.cmake
	)
	set_tests_properties(gargantuan_runtime_host_roles PROPERTIES
		LABELS "runtime;host;cli;security" TIMEOUT 60
	)
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
	add_test(NAME gargantuan_distribution_shaders
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_SHADER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
			-DGARGANTUAN_SHADER_RUNTIME_DIR=$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution/shaders
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRuntimeShaders.cmake
	)
	set_tests_properties(gargantuan_distribution_shaders PROPERTIES LABELS "build;shader;packaging")
	add_test(NAME gargantuan_server_distribution_shaders
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_SHADER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
			-DGARGANTUAN_SHADER_RUNTIME_DIR=$<TARGET_FILE_DIR:gargantuan_server>/ServerRuntimeDistribution/shaders
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRuntimeShaders.cmake
	)
	set_tests_properties(gargantuan_server_distribution_shaders PROPERTIES LABELS "build;shader;packaging")
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
	add_executable(gargantuan_blockgrid_field_tests "tests/blockgrid/BlockGridFieldTests.cpp"
		"src/blockgrid/Field.cpp" "src/blockgrid/Codec.cpp" "src/blockgrid/Picking.cpp" "src/blockgrid/Edit.cpp"
		"src/blockgrid/Geometry.cpp" "src/blockgrid/Definitions.cpp" "src/blockgrid/State.cpp" "src/blockgrid/Traversal.cpp" "src/assets/AssetTypes.cpp")
	target_include_directories(gargantuan_blockgrid_field_tests PRIVATE "include")
	target_link_libraries(gargantuan_blockgrid_field_tests PRIVATE glm::glm)
	add_test(NAME gargantuan_blockgrid_field COMMAND gargantuan_blockgrid_field_tests)
	set_tests_properties(gargantuan_blockgrid_field PROPERTIES LABELS "blockgrid;storage;persistence")
	add_executable(gargantuan_blockgrid_collision_catalog_tests "tests/blockgrid/BlockGridCollisionCatalogTests.cpp" "src/blockgrid/Geometry.cpp")
	target_include_directories(gargantuan_blockgrid_collision_catalog_tests PRIVATE "include")
	target_link_libraries(gargantuan_blockgrid_collision_catalog_tests PRIVATE box3d::box3d)
	add_test(NAME gargantuan_blockgrid_collision_catalog COMMAND gargantuan_blockgrid_collision_catalog_tests)
	set_tests_properties(gargantuan_blockgrid_collision_catalog PROPERTIES LABELS "blockgrid;physics")
	add_executable(gargantuan_blockgrid_render_edit_tests "tests/blockgrid/BlockGridRenderEditTests.cpp")
	target_link_libraries(gargantuan_blockgrid_render_edit_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_blockgrid_render_edit COMMAND gargantuan_blockgrid_render_edit_tests)
	add_executable(gargantuan_blockgrid_binding_tests "tests/blockgrid/BlockGridBindingTests.cpp")
	target_compile_definitions(gargantuan_blockgrid_binding_tests PRIVATE GARGANTUAN_BLOCKGRID_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/blockgrid/gameplay/PoweredGate.luau")
	target_link_libraries(gargantuan_blockgrid_binding_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_blockgrid_binding COMMAND gargantuan_blockgrid_binding_tests)
	set_tests_properties(gargantuan_blockgrid_binding PROPERTIES LABELS "blockgrid;runtime;schema;luau;editorhost")
	add_executable(gargantuan_instance_reference_persistence_tests "tests/InstanceReferencePersistenceTests.cpp")
	target_link_libraries(gargantuan_instance_reference_persistence_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_instance_reference_persistence COMMAND gargantuan_instance_reference_persistence_tests)
	set_tests_properties(gargantuan_instance_reference_persistence PROPERTIES LABELS "persistence;identity;references")
	add_executable(gargantuan_spatial_construction_integration_tests "tests/SpatialConstructionIntegrationTests.cpp")
	target_link_libraries(gargantuan_spatial_construction_integration_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_spatial_construction_benchmark EXCLUDE_FROM_ALL "tests/SpatialConstructionPerformance.cpp")
	target_link_libraries(gargantuan_spatial_construction_benchmark PRIVATE gargantuan_core nlohmann_json)
	if(WIN32)
		add_dependencies(gargantuan_spatial_construction_benchmark gargantuan_stage_sdl_runtime)
	endif()
	add_test(NAME gargantuan_spatial_construction_integration COMMAND gargantuan_spatial_construction_integration_tests)
	set_tests_properties(gargantuan_spatial_construction_integration PROPERTIES LABELS "spatial;blockgrid;integration")
	find_program(GARGANTUAN_SPATIAL_TEST_PYTHON NAMES python3 python)
	if(GARGANTUAN_SPATIAL_TEST_PYTHON)
		add_test(NAME gargantuan_spatial_construction_yard
			COMMAND "${GARGANTUAN_SPATIAL_TEST_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpatialConstructionYardInput.py"
				--engine $<TARGET_FILE:gargantuan>)
		set_tests_properties(gargantuan_spatial_construction_yard PROPERTIES LABELS "spatial;blockgrid;headless;sample" TIMEOUT 120)
		if(GARGANTUAN_WITH_LOCAL_SQLITE)
			add_test(NAME gargantuan_spatial_construction_yard_restart
				COMMAND "${GARGANTUAN_SPATIAL_TEST_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpatialConstructionYardInput.py"
					--engine $<TARGET_FILE:gargantuan> --durable)
			set_tests_properties(gargantuan_spatial_construction_yard_restart PROPERTIES LABELS "spatial;blockgrid;persistence;headless;sample" TIMEOUT 150)
		endif()
	endif()
	if(NOT GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION)
		add_dependencies(gargantuan_spatial_construction_integration_tests gargantuan_shaders)
		if(GARGANTUAN_RUN_GRAPHICAL_PACKAGE_SMOKE)
			add_test(NAME gargantuan_spatial_construction_vulkan COMMAND gargantuan_spatial_construction_integration_tests --vulkan)
			set_tests_properties(gargantuan_spatial_construction_vulkan PROPERTIES LABELS "spatial;blockgrid;graphical")
		endif()
	endif()
	add_executable(gargantuan_prepared_property_tests "tests/PreparedPropertyTests.cpp")
	add_executable(gargantuan_physics_editorhost_tests "tests/PhysicsEditorHostTests.cpp")
	target_link_libraries(gargantuan_physics_editorhost_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_physics_editorhost COMMAND gargantuan_physics_editorhost_tests)
	set_tests_properties(gargantuan_physics_editorhost PROPERTIES LABELS "physics;editorhost;schema;persistence;headless" TIMEOUT 120)
	add_executable(gargantuan_concrete_property_default_tests "tests/ConcretePropertyDefaultTests.cpp")
	target_link_libraries(gargantuan_concrete_property_default_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_concrete_property_defaults COMMAND gargantuan_concrete_property_default_tests)
	set_tests_properties(gargantuan_concrete_property_defaults PROPERTIES LABELS "schema;construction;persistence;editorhost")
	target_link_libraries(gargantuan_prepared_property_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_prepared_property COMMAND gargantuan_prepared_property_tests)
	set_tests_properties(gargantuan_prepared_property PROPERTIES LABELS "runtime;property;history;journal;editorhost")
	target_link_libraries(gargantuan_foundation_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_asset_foundation_tests "tests/AssetFoundationTests.cpp")
	add_executable(gargantuan_solid_geometry_tests "tests/SolidGeometryTests.cpp")
	target_link_libraries(gargantuan_solid_geometry_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_solid_geometry COMMAND gargantuan_solid_geometry_tests)
	set_tests_properties(gargantuan_solid_geometry PROPERTIES LABELS "geometry;assets;headless" TIMEOUT 120)
	add_executable(gargantuan_solid_geometry_benchmark "tests/SolidGeometryBenchmark.cpp")
	target_link_libraries(gargantuan_solid_geometry_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_mesh_collision_benchmark "tests/MeshCollisionBenchmark.cpp")
	target_link_libraries(gargantuan_mesh_collision_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_solid_allocation_probe "tests/SolidGeometryAllocationProbe.cpp")
	target_link_libraries(gargantuan_solid_allocation_probe PRIVATE gargantuan_core)
	add_test(NAME gargantuan_solid_allocation_probe COMMAND gargantuan_solid_allocation_probe --scenario simple)
	set_tests_properties(gargantuan_solid_allocation_probe PROPERTIES LABELS "geometry;allocation;headless" TIMEOUT 30)
	add_executable(gargantuan_box3d_compatibility_tests "tests/Box3DCompatibilityTests.c")
	target_compile_features(gargantuan_box3d_compatibility_tests PRIVATE c_std_17)
	target_include_directories(gargantuan_box3d_compatibility_tests PRIVATE
		"${GARGANTUAN_BOX3D_SOURCE_DIR}/src" "${GARGANTUAN_BOX3D_SOURCE_DIR}/include")
	target_link_libraries(gargantuan_box3d_compatibility_tests PRIVATE box3d::box3d)
	add_test(NAME gargantuan_box3d_compatibility COMMAND gargantuan_box3d_compatibility_tests)
	add_executable(gargantuan_box3d_mesh_chunk_tests "tests/Box3DMeshChunkTests.cpp")
	target_link_libraries(gargantuan_box3d_mesh_chunk_tests PRIVATE box3d::box3d)
	add_test(NAME gargantuan_box3d_mesh_chunks COMMAND gargantuan_box3d_mesh_chunk_tests)
	set_tests_properties(gargantuan_box3d_mesh_chunks PROPERTIES LABELS "physics;geometry;dependency;headless" TIMEOUT 60)
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-guard-test")
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-smoke-test")
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-incremental-test")
	add_test(NAME gargantuan_box3d_source_guard COMMAND ${CMAKE_COMMAND}
		"-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/Box3DCompatibilityGuard.cmake")
	add_test(NAME gargantuan_box3d_source_smoke COMMAND ${CMAKE_COMMAND}
		"-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
		"-DBOX3D_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/box3d"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/Box3DCompatibilitySmoke.cmake")
	add_test(NAME gargantuan_box3d_incremental_guard COMMAND ${CMAKE_COMMAND}
		"-DGARGANTUAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
		"-DBOX3D_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/box3d"
		"-DTEST_GENERATOR=${CMAKE_GENERATOR}"
		"-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/Box3DCompatibilityIncremental.cmake")
	set_tests_properties(gargantuan_box3d_source_guard PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-guard-test")
	set_tests_properties(gargantuan_box3d_source_smoke PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-smoke-test")
	set_tests_properties(gargantuan_box3d_incremental_guard PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/box3d-compat-incremental-test")
	set_tests_properties(gargantuan_box3d_compatibility gargantuan_box3d_source_guard gargantuan_box3d_source_smoke
		gargantuan_box3d_incremental_guard
		PROPERTIES LABELS "physics;dependency;headless" TIMEOUT 30)
	if(WIN32)
		target_link_libraries(gargantuan_solid_geometry_benchmark PRIVATE Psapi)
		target_link_libraries(gargantuan_mesh_collision_benchmark PRIVATE Psapi)
	endif()
	target_link_libraries(gargantuan_asset_foundation_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_asset_foundation_tests PRIVATE
		GARGANTUAN_ASSET_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl_ttf/external/harfbuzz/perf/fonts/Roboto-Regular.ttf"
		GARGANTUAN_ASSET_TEST_PNG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/github/ross.png"
	)
	add_custom_command(TARGET gargantuan_asset_foundation_tests POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gargantuan_asset_foundation_tests>/test-fixtures"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl_ttf/external/harfbuzz/perf/fonts/Roboto-Regular.ttf"
			"$<TARGET_FILE_DIR:gargantuan_asset_foundation_tests>/test-fixtures/Roboto-Regular.ttf"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/github/ross.png"
			"$<TARGET_FILE_DIR:gargantuan_asset_foundation_tests>/test-fixtures/ross.png"
	)
	add_executable(gargantuan_asset_foundation_benchmark "tests/AssetFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_asset_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_asset_foundation_benchmark PRIVATE
		GARGANTUAN_ASSET_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl_ttf/external/harfbuzz/perf/fonts/Roboto-Regular.ttf"
	)
	add_executable(gargantuan_animation_foundation_tests "tests/AnimationFoundationTests.cpp")
	target_link_libraries(gargantuan_animation_foundation_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_animation_preview_editorhost_tests "tests/AnimationPreviewEditorHostTests.cpp")
	target_link_libraries(gargantuan_animation_preview_editorhost_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_animation_preview_editorhost_tests PRIVATE
		GARGANTUAN_ANIMATION_PREVIEW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation-preview-rig.gltf")
	add_executable(gargantuan_character_composition_inspection_tests "tests/CharacterCompositionInspectionTests.cpp")
	target_link_libraries(gargantuan_character_composition_inspection_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_character_composition_inspection_tests PRIVATE
		GARGANTUAN_ANIMATION_PREVIEW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation-preview-rig.gltf")
	add_executable(gargantuan_attachment_mesh_tests "tests/AttachmentMeshTests.cpp")
	target_link_libraries(gargantuan_attachment_mesh_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_attachment_mesh_tests PRIVATE
		GARGANTUAN_ANIMATION_PREVIEW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation-preview-rig.gltf")
	add_executable(gargantuan_character_authoring_editorhost_tests "tests/CharacterAuthoringEditorHostTests.cpp")
	target_link_libraries(gargantuan_character_authoring_editorhost_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_character_authoring_editorhost_tests PRIVATE
		GARGANTUAN_ANIMATION_PREVIEW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation-preview-rig.gltf")
	add_executable(gargantuan_animation_foundation_benchmark "tests/AnimationFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_animation_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_audio_foundation_tests "tests/AudioFoundationTests.cpp")
	target_link_libraries(gargantuan_audio_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_audio_foundation_benchmark "tests/AudioFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_audio_foundation_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_renderer_publication_tests "tests/RendererPublicationTests.cpp")
	target_link_libraries(gargantuan_renderer_publication_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_renderer_instance_record_tests "tests/RendererInstanceRecordCacheTests.cpp")
	target_include_directories(gargantuan_renderer_instance_record_tests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_libraries(gargantuan_renderer_instance_record_tests PRIVATE glm::glm)
	add_test(NAME gargantuan_renderer_instance_records COMMAND gargantuan_renderer_instance_record_tests)
	set_tests_properties(gargantuan_renderer_instance_records PROPERTIES LABELS "renderer;runtime;headless" TIMEOUT 30)
	add_executable(gargantuan_environment_lighting_benchmark "tests/EnvironmentLightingBenchmark.cpp")
	target_link_libraries(gargantuan_environment_lighting_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_ibl_reference_tests "tests/EnvironmentLightingReferenceTests.cpp")
	add_executable(gargantuan_reflection_probe_tests "tests/ReflectionProbeSemanticsTests.cpp")
	target_link_libraries(gargantuan_reflection_probe_tests PRIVATE glm::glm)
	target_include_directories(gargantuan_reflection_probe_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
	add_test(NAME gargantuan_reflection_probe_reference COMMAND gargantuan_reflection_probe_tests)
	add_executable(gargantuan_directional_cascade_numerics_tests "tests/DirectionalCascadeNumericsTests.cpp")
	target_link_libraries(gargantuan_directional_cascade_numerics_tests PRIVATE glm::glm)
	target_include_directories(gargantuan_directional_cascade_numerics_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
	add_test(NAME gargantuan_directional_cascade_numerics COMMAND gargantuan_directional_cascade_numerics_tests)
	add_executable(gargantuan_spot_shadow_atlas_tests "tests/SpotShadowAtlasTests.cpp")
	target_link_libraries(gargantuan_spot_shadow_atlas_tests PRIVATE gargantuan_core)
	target_include_directories(gargantuan_spot_shadow_atlas_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl/include")
	add_test(NAME gargantuan_spot_shadow_atlas COMMAND gargantuan_spot_shadow_atlas_tests)
	set_tests_properties(gargantuan_reflection_probe_reference PROPERTIES LABELS "renderer;environment")
	target_link_libraries(gargantuan_ibl_reference_tests PRIVATE glm::glm)
	add_test(NAME gargantuan_ibl_reference COMMAND gargantuan_ibl_reference_tests)
	set_tests_properties(gargantuan_ibl_reference PROPERTIES LABELS "renderer;environment")
	add_executable(gargantuan_renderer_material_mip_tests "tests/RendererMaterialMipTests.cpp")
	target_include_directories(gargantuan_renderer_material_mip_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	add_test(NAME gargantuan_renderer_material_mips COMMAND gargantuan_renderer_material_mip_tests)
	set_tests_properties(gargantuan_renderer_material_mips PROPERTIES LABELS "renderer;assets")
	add_executable(gargantuan_directional_shadow_cache_tests "tests/DirectionalShadowCacheTests.cpp")
	target_link_libraries(gargantuan_directional_shadow_cache_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_directional_shadow_cache COMMAND gargantuan_directional_shadow_cache_tests)
	set_tests_properties(gargantuan_directional_shadow_cache PROPERTIES LABELS "renderer;environment")
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
	add_executable(gargantuan_runtime6_readback_fault_tests "tests/Runtime6ReadbackFaultTests.cpp")
	target_link_libraries(gargantuan_runtime6_readback_fault_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_runtime6_readback_fault COMMAND gargantuan_runtime6_readback_fault_tests)
	add_executable(gargantuan_runtime6_dirty_insert_fault_tests "tests/Runtime6DirtyInsertFaultTests.cpp")
	target_link_libraries(gargantuan_runtime6_dirty_insert_fault_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_runtime6_dirty_insert_fault COMMAND gargantuan_runtime6_dirty_insert_fault_tests)
	add_executable(gargantuan_runtime6_physics_listener_probe "tests/Runtime6PhysicsListenerProbe.cpp")
	target_link_libraries(gargantuan_runtime6_physics_listener_probe PRIVATE gargantuan_core)
	add_test(NAME gargantuan_runtime6_physics_listener_native
		COMMAND gargantuan_runtime6_physics_listener_probe --mode=native --count=100 --frames=2)
	add_test(NAME gargantuan_runtime6_physics_listener_native_drain_profiled
		COMMAND gargantuan_runtime6_physics_listener_probe --mode=native --count=100 --frames=2
			--drain-physics --profile=on)
	add_test(NAME gargantuan_runtime6_physics_listener_native_drain_unprofiled
		COMMAND gargantuan_runtime6_physics_listener_probe --mode=native --count=100 --frames=2
			--drain-physics --profile=off)
	add_test(NAME gargantuan_runtime6_physics_listener_renderer
		COMMAND gargantuan_runtime6_physics_listener_probe --mode=renderer-only --count=100 --frames=2)
	add_test(NAME gargantuan_runtime6_physics_listener_counterfactual
		COMMAND gargantuan_runtime6_physics_listener_probe --mode=renderer-only-no-listener --count=100 --frames=2)
	add_executable(gargantuan_physics_imported_mesh_tests "tests/PhysicsImportedMeshTests.cpp")
	target_link_libraries(gargantuan_physics_imported_mesh_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_physics_imported_mesh COMMAND gargantuan_physics_imported_mesh_tests)
	add_executable(gargantuan_physics_query_benchmark "tests/PhysicsQueryBenchmark.cpp")
	target_link_libraries(gargantuan_physics_query_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_physics_activation_benchmark "tests/PhysicsActivationBenchmark.cpp")
	target_link_libraries(gargantuan_physics_activation_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_physics_foundation_benchmark "tests/PhysicsFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_physics_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_physics_expansion_benchmark "tests/PhysicsExpansionBenchmark.cpp")
	target_link_libraries(gargantuan_physics_expansion_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_soft_body_physics_tests "tests/SoftBodyPhysicsTests.cpp")
	target_link_libraries(gargantuan_soft_body_physics_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_soft_body_physics_benchmark "tests/SoftBodyPhysicsBenchmark.cpp")
	target_link_libraries(gargantuan_soft_body_physics_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_platform_input_boundary_tests "tests/PlatformInputBoundaryTests.cpp")
	target_link_libraries(gargantuan_platform_input_boundary_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_player_runtime_tests "tests/PlayerRuntimeTests.cpp")
	target_link_libraries(gargantuan_player_runtime_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_input_action_tests "tests/InputActionTests.cpp")
	target_link_libraries(gargantuan_input_action_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_script_document_binding_tests "tests/ScriptDocumentBindingTests.cpp")
	target_link_libraries(gargantuan_script_document_binding_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_root_debug_vm_tests "tests/RootScriptDebugVmTests.cpp" "src/scripting/RootScriptDebug.cpp")
	target_include_directories(gargantuan_root_debug_vm_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
	target_link_libraries(gargantuan_root_debug_vm_tests PRIVATE Luau.Compiler Luau.VM)
	add_executable(gargantuan_input_action_benchmark "tests/InputActionBenchmark.cpp" "tests/ActionMapBaseline.cpp")
	target_link_libraries(gargantuan_input_action_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_interaction_foundation_tests "tests/InteractionFoundationTests.cpp")
	target_link_libraries(gargantuan_interaction_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_interaction_foundation_benchmark "tests/InteractionFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_interaction_foundation_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_entitlement_service_tests "tests/EntitlementServiceTests.cpp")
	target_link_libraries(gargantuan_entitlement_service_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_persistence_runtime_tests
		"tests/PersistenceRuntimeTests.cpp" "tests/BenchmarkAllocationProbe.cpp")
	target_link_libraries(gargantuan_persistence_runtime_tests PRIVATE gargantuan_core)
	target_compile_definitions(gargantuan_persistence_runtime_tests PRIVATE
		GARGANTUAN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
	add_executable(gargantuan_local_persistence_provider_tests "tests/LocalPersistenceProviderTests.cpp")
	target_link_libraries(gargantuan_local_persistence_provider_tests PRIVATE gargantuan_core)
	if(GARGANTUAN_WITH_NODE_PERSISTENCE)
		target_compile_definitions(gargantuan_persistence_runtime_tests PRIVATE GARGANTUAN_WITH_NODE_PERSISTENCE=1)
		target_include_directories(gargantuan_persistence_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
		target_link_libraries(gargantuan_persistence_runtime_tests PRIVATE gargantuan_node_persistence_provider)
		add_executable(gargantuan_node_persistence_provider_tests "tests/NodePersistenceProviderTests.cpp")
		target_include_directories(gargantuan_node_persistence_provider_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
		target_link_libraries(gargantuan_node_persistence_provider_tests PRIVATE gargantuan_node_persistence_provider)
		add_test(NAME gargantuan_node_persistence_provider COMMAND gargantuan_node_persistence_provider_tests)
		set_tests_properties(gargantuan_node_persistence_provider PROPERTIES LABELS "persistence;node;headless" TIMEOUT 30)
	endif()
	target_compile_definitions(gargantuan_entitlement_service_tests PRIVATE
		GARGANTUAN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
	)
	add_executable(gargantuan_entitlement_provider_benchmark "tests/EntitlementProviderBenchmark.cpp")
	target_link_libraries(gargantuan_entitlement_provider_benchmark PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_gui_foundation_tests "tests/GuiFoundationTests.cpp" "tests/BenchmarkAllocationProbe.cpp")
	target_link_libraries(gargantuan_gui_foundation_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_viewport_frame_tests "tests/ViewportFrameTests.cpp")
	target_link_libraries(gargantuan_viewport_frame_tests PRIVATE gargantuan_core)
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
	add_executable(gargantuan_qualified_scale_package "tests/QualifiedScalePackage.cpp")
	add_executable(gargantuan_workload_timing_evidence_tests "tests/WorkloadTimingEvidenceTests.cpp")
	target_compile_features(gargantuan_workload_timing_evidence_tests PRIVATE cxx_std_20)
	add_test(NAME gargantuan_workload_timing_evidence COMMAND gargantuan_workload_timing_evidence_tests)
	target_link_libraries(gargantuan_qualified_scale_package PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_qualified_scale_package PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
		GARGANTUAN_QUALIFIED_SCALE_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Foundation3LQualifiedScale"
	)
	add_dependencies(gargantuan_qualified_scale_package
		gargantuan_runtime_distribution gargantuan_server_runtime_distribution)
	add_test(NAME gargantuan_qualified_scale_fixture
		COMMAND gargantuan_qualified_scale_package --verify-fixture)
	set_tests_properties(gargantuan_qualified_scale_fixture PROPERTIES
		LABELS "content;package;foundation3l" TIMEOUT 120)
	if(WIN32)
		find_program(GARGANTUAN_PWSH_EXECUTABLE NAMES pwsh)
		find_program(GARGANTUAN_GO_EXECUTABLE NAMES go)
		if(GARGANTUAN_PWSH_EXECUTABLE)
			add_test(NAME gargantuan_physical_game_session_farm_pipe_drain
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalFarmPipeDrainTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_evidence
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_cleanup
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmCleanupTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_endpoint
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmEndpointTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_manifest
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/NewPhysicalGameSessionFarmManifestTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_reconciliation
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmReconcileTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_acceptance_observation
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmAcceptanceTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_logical_lifecycle
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmLifecycleTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_f1_evidence
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmF1Tests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_remote_ownership
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmRemoteOwnershipTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_analysis_inventory
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalFarmAnalysisInventoryTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_fairness_evidence
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/AdmissionFairnessEvidenceTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_recovery_evidence
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmRecoveryTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_causal_replay
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/RecoveryCausalEvidenceTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_preflight
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmPreflightTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_node_evidence
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmNodeEvidenceTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_node_resources
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalFarmNodeResourcesTests.ps1")
			add_test(NAME gargantuan_physical_game_session_farm_node_tls_generation
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/NewPhysicalGameSessionFarmNodeTlsTests.ps1")
			if(GARGANTUAN_GO_EXECUTABLE)
				add_test(NAME gargantuan_physical_game_session_farm_node
					COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
					"${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicalGameSessionFarmNodeTests.ps1"
					-GoExecutable "${GARGANTUAN_GO_EXECUTABLE}")
			endif()
			add_test(NAME gargantuan_farm32_worker_capture_hook
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tools/physical-qualifier/tests/TestCaptureHooks.ps1"
				-HookPath "${CMAKE_CURRENT_SOURCE_DIR}/tools/physical-qualifier/worker/PktMonFarm32Capture.ps1"
				-Farm32)
			add_test(NAME gargantuan_farm32_client_capture_source
				COMMAND "${GARGANTUAN_PWSH_EXECUTABLE}" -NoProfile -File
				"${CMAKE_CURRENT_SOURCE_DIR}/tools/physical-qualifier/tests/TestDumpcapFarm32Capture.ps1")
		endif()
	endif()
	add_executable(gargantuan_gui_foundation_benchmark "tests/GuiFoundationBenchmark.cpp")
	target_link_libraries(gargantuan_gui_foundation_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_prerun_bootstrap_tests "tests/PreRunBootstrapTests.cpp")
	target_link_libraries(gargantuan_prerun_bootstrap_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_prerun_editor_host_tests "tests/PreRunEditorHostTests.cpp")
	target_link_libraries(gargantuan_prerun_editor_host_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_prerun_editor_host COMMAND gargantuan_prerun_editor_host_tests)
	set_tests_properties(gargantuan_prerun_editor_host PROPERTIES LABELS "prerun;editorhost;schema;security;headless" TIMEOUT 120)
	add_executable(gargantuan_replicated_storage_tests "tests/ReplicatedStorageTests.cpp")
	target_link_libraries(gargantuan_replicated_storage_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_replicated_storage_editor_host_tests "tests/ReplicatedStorageEditorHostTests.cpp")
	target_link_libraries(gargantuan_replicated_storage_editor_host_tests PRIVATE gargantuan_core nlohmann_json)
	add_dependencies(gargantuan_replicated_storage_editor_host_tests gargantuan_runtime_distribution)
	add_test(NAME gargantuan_replicated_storage_editor_host COMMAND gargantuan_replicated_storage_editor_host_tests)
	set_tests_properties(gargantuan_replicated_storage_editor_host PROPERTIES LABELS "replication;editor;packaging;headless" TIMEOUT 120)
	add_executable(gargantuan_replicated_storage_session_tests "tests/ReplicatedStorageSessionTests.cpp")
	target_link_libraries(gargantuan_replicated_storage_session_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_replicated_storage_session COMMAND gargantuan_replicated_storage_session_tests)
	set_tests_properties(gargantuan_replicated_storage_session PROPERTIES LABELS "replication;runtime;security;headless" TIMEOUT 120)
	add_test(NAME gargantuan_replicated_storage COMMAND gargantuan_replicated_storage_tests)
	set_tests_properties(gargantuan_replicated_storage PROPERTIES LABELS "replication;schema;security;headless" TIMEOUT 120)
	add_executable(gargantuan_attributes_tags_profile "tests/AttributesTagsProfile.cpp")
	target_link_libraries(gargantuan_attributes_tags_profile PRIVATE gargantuan_core)
	add_executable(gargantuan_networking_contract_tests "tests/NetworkingContractsTests.cpp")
	target_link_libraries(gargantuan_networking_contract_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_farm_admission_evidence_tests "tests/FarmAdmissionEvidenceTests.cpp")
	target_include_directories(gargantuan_farm_admission_evidence_tests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
	add_executable(gargantuan_simulated_transport_tests "tests/SimulatedTransportTests.cpp")
	target_link_libraries(gargantuan_simulated_transport_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_scheduler_contract_tests "tests/SchedulerContractTests.cpp")
	target_link_libraries(gargantuan_scheduler_contract_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_replication_tests "tests/ReplicationTests.cpp")
	target_link_libraries(gargantuan_replication_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_replication_relevance_tests "tests/ReplicationRelevanceTests.cpp")
	target_link_libraries(gargantuan_replication_relevance_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_spatial_domain_tests "tests/SpatialDomainTests.cpp")
	target_link_libraries(gargantuan_spatial_domain_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_spatial_transfer_tests "tests/SpatialTransferTests.cpp")
	target_link_libraries(gargantuan_spatial_transfer_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_spatial_region_index_tests "tests/SpatialRegionIndexTests.cpp")
	target_link_libraries(gargantuan_spatial_region_index_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_content_availability_tests "tests/ContentAvailabilityTests.cpp")
	target_link_libraries(gargantuan_content_availability_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_content_availability COMMAND gargantuan_content_availability_tests)
	add_executable(gargantuan_content_coarse_policy_tests "tests/PackageContentCoarsePolicyTests.cpp")
	target_link_libraries(gargantuan_content_coarse_policy_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_content_coarse_policy COMMAND gargantuan_content_coarse_policy_tests)
	add_executable(gargantuan_residency_demand_policy_tests "tests/ResidencyDemandPolicyTests.cpp")
	target_link_libraries(gargantuan_residency_demand_policy_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_residency_demand_policy COMMAND gargantuan_residency_demand_policy_tests)
	add_executable(gargantuan_residency_demand_integration_tests "tests/ResidencyDemandIntegrationTests.cpp")
	target_link_libraries(gargantuan_residency_demand_integration_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_residency_demand_integration COMMAND gargantuan_residency_demand_integration_tests)
	add_executable(gargantuan_residency_demand_benchmark "tests/ResidencyDemandBenchmark.cpp")
	target_link_libraries(gargantuan_residency_demand_benchmark PRIVATE gargantuan_core)
	add_test(NAME gargantuan_residency_demand_benchmark_smoke COMMAND gargantuan_residency_demand_benchmark --quick)
	add_test(NAME gargantuan_residency_readiness_smoke COMMAND gargantuan_residency_demand_benchmark --readiness-quick)
	add_executable(gargantuan_residency_demand_startup_tests "tests/ResidencyDemandStartupTests.cpp")
	target_link_libraries(gargantuan_residency_demand_startup_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_residency_demand_startup COMMAND gargantuan_residency_demand_startup_tests)
	add_executable(gargantuan_residency_streaming_scenario_tests "tests/ResidencyStreamingScenarioTests.cpp")
	target_link_libraries(gargantuan_residency_streaming_scenario_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_residency_streaming_scenarios COMMAND gargantuan_residency_streaming_scenario_tests)
	add_executable(gargantuan_residency_spatial_metadata_tests "tests/ResidencySpatialMetadataTests.cpp")
	target_link_libraries(gargantuan_residency_spatial_metadata_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_residency_spatial_metadata COMMAND gargantuan_residency_spatial_metadata_tests)
	add_executable(gargantuan_streaming_diagnostics_tests "tests/StreamingDiagnosticsTests.cpp")
	target_link_libraries(gargantuan_streaming_diagnostics_tests PRIVATE gargantuan_core nlohmann_json)
	target_include_directories(gargantuan_streaming_diagnostics_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	add_test(NAME gargantuan_streaming_diagnostics COMMAND gargantuan_streaming_diagnostics_tests)
	add_executable(gargantuan_package_spatial_metadata_tests "tests/PackageSpatialMetadataTests.cpp")
	target_link_libraries(gargantuan_package_spatial_metadata_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_package_spatial_metadata_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame")
	add_test(NAME gargantuan_package_spatial_metadata COMMAND gargantuan_package_spatial_metadata_tests)
	add_executable(gargantuan_cross_foundation_streaming_tests "tests/CrossFoundationStreamingTests.cpp")
	target_link_libraries(gargantuan_cross_foundation_streaming_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_cross_foundation_streaming_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
		GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT="$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution")
	add_dependencies(gargantuan_cross_foundation_streaming_tests gargantuan_runtime_distribution)
	add_test(NAME gargantuan_cross_foundation_streaming COMMAND gargantuan_cross_foundation_streaming_tests)
	set_tests_properties(gargantuan_package_spatial_metadata gargantuan_cross_foundation_streaming
		PROPERTIES LABELS "content;geometry;integration;runtime" TIMEOUT 180)
	foreach(GARGANTUAN_CROSS_FOUNDATION_KIND bounds consumer performance)
		set(GARGANTUAN_CROSS_FOUNDATION_TARGET "gargantuan_cross_foundation_independent_${GARGANTUAN_CROSS_FOUNDATION_KIND}_tests")
		if(GARGANTUAN_CROSS_FOUNDATION_KIND STREQUAL "bounds")
			set(GARGANTUAN_CROSS_FOUNDATION_SOURCE "tests/CrossFoundationIndependentBoundsTests.cpp")
		elseif(GARGANTUAN_CROSS_FOUNDATION_KIND STREQUAL "consumer")
			set(GARGANTUAN_CROSS_FOUNDATION_SOURCE "tests/CrossFoundationIndependentConsumerTests.cpp")
		else()
			set(GARGANTUAN_CROSS_FOUNDATION_SOURCE "tests/CrossFoundationIndependentPerformanceTests.cpp")
		endif()
		add_executable(${GARGANTUAN_CROSS_FOUNDATION_TARGET} "${GARGANTUAN_CROSS_FOUNDATION_SOURCE}")
		target_link_libraries(${GARGANTUAN_CROSS_FOUNDATION_TARGET} PRIVATE gargantuan_core nlohmann_json)
		target_compile_definitions(${GARGANTUAN_CROSS_FOUNDATION_TARGET} PRIVATE
			GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
			GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT="$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution")
		add_dependencies(${GARGANTUAN_CROSS_FOUNDATION_TARGET} gargantuan_packager gargantuan_runtime_distribution)
		add_test(NAME "gargantuan_cross_foundation_independent_${GARGANTUAN_CROSS_FOUNDATION_KIND}"
			COMMAND ${GARGANTUAN_CROSS_FOUNDATION_TARGET})
		set_tests_properties("gargantuan_cross_foundation_independent_${GARGANTUAN_CROSS_FOUNDATION_KIND}"
			PROPERTIES LABELS "content;geometry;integration;independent" TIMEOUT 180)
	endforeach()
	add_executable(gargantuan_cross_foundation_independent_lifecycle_tests "tests/CrossFoundationIndependentLifecycleTests.cpp")
	target_link_libraries(gargantuan_cross_foundation_independent_lifecycle_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_cross_foundation_independent_lifecycle COMMAND gargantuan_cross_foundation_independent_lifecycle_tests)
	set_tests_properties(gargantuan_cross_foundation_independent_lifecycle
		PROPERTIES LABELS "content;residency;physics;render;network;lifecycle" TIMEOUT 180)
	add_executable(gargantuan_cross_foundation_independent_authoring_tests "tests/CrossFoundationIndependentAuthoringTests.cpp")
	target_link_libraries(gargantuan_cross_foundation_independent_authoring_tests PRIVATE gargantuan_core nlohmann_json)
	target_compile_definitions(gargantuan_cross_foundation_independent_authoring_tests PRIVATE
		GARGANTUAN_ANIMATION_PREVIEW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation-preview-rig.gltf")
	add_test(NAME gargantuan_cross_foundation_independent_authoring COMMAND gargantuan_cross_foundation_independent_authoring_tests)
	set_tests_properties(gargantuan_cross_foundation_independent_authoring
		PROPERTIES LABELS "geometry;editor;integration;authority" TIMEOUT 120)
	add_executable(gargantuan_team_create_authority_tests "tests/TeamCreateAuthorityTests.cpp")
	target_link_libraries(gargantuan_team_create_authority_tests PRIVATE gargantuan_core nlohmann_json)
	add_test(NAME gargantuan_team_create_authority COMMAND gargantuan_team_create_authority_tests)
	set_tests_properties(gargantuan_team_create_authority PROPERTIES LABELS "editor;team-create;authority" TIMEOUT 120)
	add_executable(gargantuan_mesh_geometry_relevance_tests "tests/MeshGeometryRelevanceTests.cpp")
	target_link_libraries(gargantuan_mesh_geometry_relevance_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_mesh_geometry_relevance COMMAND gargantuan_mesh_geometry_relevance_tests)
	set_tests_properties(gargantuan_mesh_geometry_relevance PROPERTIES LABELS "geometry;relevance;runtime" TIMEOUT 120)
	set_tests_properties(gargantuan_residency_readiness_smoke gargantuan_residency_demand_startup
		gargantuan_residency_streaming_scenarios gargantuan_residency_spatial_metadata gargantuan_streaming_diagnostics
		PROPERTIES LABELS "content;residency;streaming;runtime" TIMEOUT 120)
	set_tests_properties(gargantuan_content_coarse_policy gargantuan_residency_demand_policy
		gargantuan_residency_demand_integration gargantuan_residency_demand_benchmark_smoke
		PROPERTIES LABELS "content;residency;runtime" TIMEOUT 180)
	add_executable(gargantuan_content_ownership_tests "tests/ContentAvailabilityTests.cpp")
	target_compile_definitions(gargantuan_content_ownership_tests PRIVATE GARGANTUAN_TEST_REUSE_ALLOCATION=1)
	target_link_libraries(gargantuan_content_ownership_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_content_ownership COMMAND gargantuan_content_ownership_tests)
	set_tests_properties(gargantuan_content_ownership PROPERTIES LABELS "content;runtime;lifecycle;security" TIMEOUT 180)
	add_executable(gargantuan_content_availability_benchmark "tests/ContentAvailabilityBenchmark.cpp")
	target_link_libraries(gargantuan_content_availability_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_remote_tests "tests/RemoteTests.cpp")
	target_link_libraries(gargantuan_remote_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_remote_luau_tests "tests/RemoteLuauTests.cpp")
	target_link_libraries(gargantuan_remote_luau_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_runtime6_luau_mutation_benchmark "tests/Runtime6LuauMutationBenchmark.cpp")
	target_link_libraries(gargantuan_runtime6_luau_mutation_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_character_networking_tests "tests/CharacterNetworkingTests.cpp")
	target_link_libraries(gargantuan_character_networking_tests PRIVATE gargantuan_core nlohmann_json)
	add_executable(gargantuan_game_session_tests "tests/GameSessionTests.cpp")
	target_link_libraries(gargantuan_game_session_tests PRIVATE gargantuan_core)
	add_executable(gargantuan_blockgrid_session_tests "tests/blockgrid/BlockGridSessionTests.cpp")
	target_link_libraries(gargantuan_blockgrid_session_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_blockgrid_session COMMAND gargantuan_blockgrid_session_tests)
	set_tests_properties(gargantuan_blockgrid_session PROPERTIES LABELS "spatial;network;headless" TIMEOUT 30)
	target_compile_definitions(gargantuan_game_session_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
	)
	add_test(NAME GameSessionTests COMMAND gargantuan_game_session_tests)
	# Use an isolated Release executable so allocation instrumentation is absent from the gates.
	add_executable(gargantuan_gameplay_allocation_workload EXCLUDE_FROM_ALL "tests/GameSessionTests.cpp")
	target_link_libraries(gargantuan_gameplay_allocation_workload PRIVATE gargantuan_core)
	target_compile_definitions(gargantuan_gameplay_allocation_workload PRIVATE
		GARGANTUAN_GAMEPLAY_WORKLOAD_ALLOCATIONS=1
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame")
	add_test(NAME gargantuan_gameplay_possession_session COMMAND gargantuan_game_session_tests --generic-possession)
	set_tests_properties(gargantuan_gameplay_possession_session PROPERTIES LABELS "networking;runtime;security" TIMEOUT 45)
	add_executable(gargantuan_gameplay_sample_tests "tests/GameplaySampleTests.cpp")
	target_link_libraries(gargantuan_gameplay_sample_tests PRIVATE gargantuan_core)
	target_compile_definitions(gargantuan_gameplay_sample_tests PRIVATE
		GARGANTUAN_GAMEPLAY_SAMPLE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/GameplayFoundation7")
	add_test(NAME gargantuan_gameplay_relay_sample COMMAND gargantuan_gameplay_sample_tests)
	set_tests_properties(gargantuan_gameplay_relay_sample PROPERTIES LABELS "networking;runtime;sample" TIMEOUT 45)
	add_test(NAME gargantuan_gameplay_relay_custom_character COMMAND gargantuan_gameplay_sample_tests --custom-character)
	set_tests_properties(gargantuan_gameplay_relay_custom_character PROPERTIES LABELS "networking;runtime;sample" TIMEOUT 45)
	add_test(NAME gargantuan_gas_relay_rescue
		COMMAND ${CMAKE_COMMAND}
			-DPACKAGER=$<TARGET_FILE:gargantuan_packager>
			-DMODEL_TEST=$<TARGET_FILE:gargantuan_gas_authoring_tests>
			-DMISSION=$<TARGET_FILE:gargantuan_gameplay_sample_tests>
			-DPLAYER_DISTRIBUTION=$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution
			-DSERVER_DISTRIBUTION=$<TARGET_FILE_DIR:gargantuan_server>/ServerRuntimeDistribution
			-DSOURCE_PROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/GameplayFoundation7
			-DWORKSPACE_ROOT=${CMAKE_CURRENT_BINARY_DIR}
			-DGNS=$<BOOL:${GARGANTUAN_WITH_GNS}>
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/GasRelayRescueQualification.cmake)
	set_tests_properties(gargantuan_gas_relay_rescue PROPERTIES
		LABELS "persistence;packaging;networking;sample" RUN_SERIAL TRUE TIMEOUT 360)
	if(GARGANTUAN_WITH_GNS)
		target_link_libraries(gargantuan_game_session_tests PRIVATE gargantuan_gns_transport)
		target_link_libraries(gargantuan_blockgrid_session_tests PRIVATE gargantuan_gns_transport)
		target_compile_definitions(gargantuan_blockgrid_session_tests PRIVATE GARGANTUAN_WITH_GNS=1)
		add_test(NAME gargantuan_blockgrid_session_gns COMMAND gargantuan_blockgrid_session_tests --gns)
		set_tests_properties(gargantuan_blockgrid_session_gns PROPERTIES LABELS "spatial;network;headless" TIMEOUT 45)
		target_link_libraries(gargantuan_gameplay_allocation_workload PRIVATE gargantuan_gns_transport)
		target_link_libraries(gargantuan_gameplay_sample_tests PRIVATE gargantuan_gns_transport)
		add_test(NAME gargantuan_gameplay_possession_gns COMMAND gargantuan_game_session_tests --generic-possession-gns)
		add_test(NAME gargantuan_terrain_session_gns COMMAND gargantuan_game_session_tests --terrain-gns)
		set_tests_properties(gargantuan_terrain_session_gns PROPERTIES LABELS "terrain;networking;transport" TIMEOUT 20 RUN_SERIAL TRUE)
		set_tests_properties(gargantuan_gameplay_possession_gns PROPERTIES LABELS "networking;runtime;security;gns" TIMEOUT 45)
		add_test(NAME gargantuan_gameplay_relay_sample_gns COMMAND gargantuan_gameplay_sample_tests --gns)
		set_tests_properties(gargantuan_gameplay_relay_sample_gns PROPERTIES LABELS "networking;runtime;sample;gns" TIMEOUT 45)
	endif()
	add_executable(gargantuan_gameplay_input_tests "tests/GameplayInputTests.cpp")
	target_link_libraries(gargantuan_gameplay_input_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_gameplay_input COMMAND gargantuan_gameplay_input_tests)
	set_tests_properties(gargantuan_gameplay_input PROPERTIES LABELS "networking;runtime;security" TIMEOUT 180)
	add_executable(gargantuan_gameplay_pose_tests "tests/GameplayPoseTests.cpp")
	target_link_libraries(gargantuan_gameplay_pose_tests PRIVATE gargantuan_core)
	add_test(NAME gargantuan_gameplay_pose COMMAND gargantuan_gameplay_pose_tests)
	set_tests_properties(gargantuan_gameplay_pose PROPERTIES LABELS "networking;runtime;security" TIMEOUT 180)
	add_executable(gargantuan_frozen_recovery_quote_tests "tests/FrozenRecoveryQuoteTests.cpp")
	target_link_libraries(gargantuan_frozen_recovery_quote_tests PRIVATE gargantuan_core)
	add_test(NAME FrozenRecoveryQuoteTests COMMAND gargantuan_frozen_recovery_quote_tests)
	set_tests_properties(FrozenRecoveryQuoteTests PROPERTIES TIMEOUT 420 LABELS "network;qualification;lifecycle")
	add_executable(gargantuan_game_session_benchmark "tests/GameSessionBenchmark.cpp")
	target_link_libraries(gargantuan_game_session_benchmark PRIVATE gargantuan_core)
	target_compile_definitions(gargantuan_game_session_benchmark PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame")
	add_test(NAME ContentScaleUnwindTests COMMAND ${CMAKE_COMMAND}
		-DGARGANTUAN_BENCHMARK=$<TARGET_FILE:gargantuan_game_session_benchmark>
		-DGARGANTUAN_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/ContentScaleUnwindTests.cmake)
	set_tests_properties(ContentScaleUnwindTests PROPERTIES TIMEOUT 360 LABELS "content;lifecycle")
	add_executable(gargantuan_spatial_region_index_benchmark "tests/SpatialRegionIndexBenchmark.cpp")
	target_link_libraries(gargantuan_spatial_region_index_benchmark PRIVATE gargantuan_core)
	target_compile_definitions(gargantuan_character_networking_tests PRIVATE
		GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
	)
	add_executable(gargantuan_character_networking_benchmark "tests/CharacterNetworkingBenchmark.cpp")
	target_link_libraries(gargantuan_character_networking_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_replication_benchmark "tests/ReplicationBenchmark.cpp")
	target_link_libraries(gargantuan_replication_benchmark PRIVATE gargantuan_core)
	add_executable(gargantuan_remote_benchmark "tests/RemoteBenchmark.cpp")
	target_link_libraries(gargantuan_remote_benchmark PRIVATE gargantuan_core)
	if(GARGANTUAN_WITH_GNS)
		add_executable(gargantuan_real_transport_tests "tests/GameNetworkingSocketsTransportTests.cpp")
		add_executable(gargantuan_gns_service_fairness_tests "tests/GnsServiceFairnessTests.cpp")
		# Investigation-only executable: retain the explicit target, not an ALL build.
		add_executable(gargantuan_gns_service_reproducer EXCLUDE_FROM_ALL "tests/GnsServiceStarvationReproducer.cpp")
		target_compile_features(gargantuan_gns_service_reproducer PRIVATE cxx_std_17)
		target_link_libraries(gargantuan_gns_service_reproducer PRIVATE GameNetworkingSockets::static)
		target_compile_features(gargantuan_gns_service_fairness_tests PRIVATE cxx_std_17)
		target_link_libraries(gargantuan_gns_service_fairness_tests PRIVATE GameNetworkingSockets::static)
		target_include_directories(gargantuan_gns_service_fairness_tests PRIVATE
			"${CMAKE_CURRENT_BINARY_DIR}"
			"${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets"
			$<TARGET_PROPERTY:GameNetworkingSockets_s,INCLUDE_DIRECTORIES>)
		target_compile_definitions(gargantuan_gns_service_fairness_tests PRIVATE
			$<TARGET_PROPERTY:GameNetworkingSockets_s,COMPILE_DEFINITIONS>)
		if(MSVC)
			target_compile_options(gargantuan_gns_service_fairness_tests PRIVATE $<IF:$<CONFIG:Debug>,/GR,/GR->)
		endif()
		add_test(NAME gargantuan_gns_service_fairness COMMAND gargantuan_gns_service_fairness_tests)
		if(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS AND WIN32)
			add_executable(gargantuan_gns_native_pending_ownership_tests "tests/GnsNativePendingOwnershipTests.cpp")
			target_compile_features(gargantuan_gns_native_pending_ownership_tests PRIVATE cxx_std_17)
			target_link_libraries(gargantuan_gns_native_pending_ownership_tests PRIVATE GameNetworkingSockets::static)
			target_include_directories(gargantuan_gns_native_pending_ownership_tests PRIVATE
				"${CMAKE_CURRENT_SOURCE_DIR}/cmake/gns"
				"${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets"
				$<TARGET_PROPERTY:GameNetworkingSockets_s,INCLUDE_DIRECTORIES>)
			target_compile_definitions(gargantuan_gns_native_pending_ownership_tests PRIVATE
				$<TARGET_PROPERTY:GameNetworkingSockets_s,COMPILE_DEFINITIONS>)
			if(MSVC)
				target_compile_options(gargantuan_gns_native_pending_ownership_tests PRIVATE $<IF:$<CONFIG:Debug>,/GR,/GR->)
			endif()
			add_test(NAME gargantuan_gns_native_pending_ownership COMMAND gargantuan_gns_native_pending_ownership_tests)
			set_tests_properties(gargantuan_gns_native_pending_ownership PROPERTIES TIMEOUT 30 LABELS "network;qualification;native-egress")
			add_executable(gargantuan_gns_native_egress_worker_tests "tests/GnsNativeEgressWorkerTests.cpp")
			target_compile_features(gargantuan_gns_native_egress_worker_tests PRIVATE cxx_std_17)
			target_link_libraries(gargantuan_gns_native_egress_worker_tests PRIVATE GameNetworkingSockets::static)
			target_include_directories(gargantuan_gns_native_egress_worker_tests PRIVATE
				"${CMAKE_CURRENT_BINARY_DIR}"
				"${CMAKE_CURRENT_SOURCE_DIR}/cmake/gns"
				"${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets"
				$<TARGET_PROPERTY:GameNetworkingSockets_s,INCLUDE_DIRECTORIES>)
			target_compile_definitions(gargantuan_gns_native_egress_worker_tests PRIVATE
				$<TARGET_PROPERTY:GameNetworkingSockets_s,COMPILE_DEFINITIONS>)
			if(MSVC)
				target_compile_options(gargantuan_gns_native_egress_worker_tests PRIVATE $<IF:$<CONFIG:Debug>,/GR,/GR->)
			endif()
			add_test(NAME gargantuan_gns_native_egress_worker COMMAND gargantuan_gns_native_egress_worker_tests)
			set_tests_properties(gargantuan_gns_native_egress_worker PROPERTIES TIMEOUT 30 LABELS "network;qualification;native-egress")
		endif()
		add_executable(gargantuan_gns_capacity_benchmark "tests/GnsCapacityBenchmark.cpp")
		target_compile_features(gargantuan_gns_capacity_benchmark PRIVATE cxx_std_23)
		target_link_libraries(gargantuan_gns_capacity_benchmark PRIVATE gargantuan_core GameNetworkingSockets::static)
		target_link_libraries(gargantuan_real_transport_tests PRIVATE
			gargantuan_core
			gargantuan_gns_transport
		)
		add_executable(gargantuan_remote_real_transport_tests "tests/RemoteGnsTests.cpp")
		target_link_libraries(gargantuan_remote_real_transport_tests PRIVATE
			gargantuan_core
			gargantuan_gns_transport
		)
		add_executable(gargantuan_character_real_transport_tests "tests/CharacterNetworkGnsTests.cpp")
		target_link_libraries(gargantuan_character_real_transport_tests PRIVATE
			gargantuan_core
			gargantuan_gns_transport
		)
		add_executable(gargantuan_game_session_real_transport_tests "tests/GameSessionGnsTests.cpp")
		# Manual physical Phase 1 probe. Only its socket-free self-test is CTest.
		add_executable(gargantuan_physical_gns_funding_probe "tests/PhysicalGnsFundingProbe.cpp")
		target_link_libraries(gargantuan_physical_gns_funding_probe PRIVATE gargantuan_core gargantuan_gns_transport)
		set(GARGANTUAN_PROBE_SOURCE "unlabelled-source-see-external-manifest" CACHE STRING
			"Physical probe source identity, confirmed by the staging manifest")
		target_compile_definitions(gargantuan_physical_gns_funding_probe PRIVATE
			GARGANTUAN_PROBE_SOURCE="${GARGANTUAN_PROBE_SOURCE}")
		foreach(GARGANTUAN_PROBE_MODULE IN LISTS GARGANTUAN_RUNTIME_LUAU_MODULES)
			add_custom_command(TARGET gargantuan_physical_gns_funding_probe POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gargantuan_physical_gns_funding_probe>/runtime"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"${CMAKE_CURRENT_SOURCE_DIR}/assets/runtime/${GARGANTUAN_PROBE_MODULE}"
					"$<TARGET_FILE_DIR:gargantuan_physical_gns_funding_probe>/runtime/${GARGANTUAN_PROBE_MODULE}"
			)
		endforeach()
		target_link_libraries(gargantuan_game_session_real_transport_tests PRIVATE
			gargantuan_core
			gargantuan_gns_transport
		)
		target_compile_definitions(gargantuan_game_session_real_transport_tests PRIVATE
			GARGANTUAN_FIRST_COMPLETE_GAME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
		)
	endif()
	if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
		add_executable(gargantuan_renderer_benchmark "tests/RendererBenchmark.cpp")
		target_link_libraries(gargantuan_renderer_benchmark PRIVATE gargantuan_core)
		option(GARGANTUAN_BUILD_RENDERER_ALLOCATION_PROBE "Build opt-in renderer C++ allocation attribution executable" OFF)
		if(GARGANTUAN_BUILD_RENDERER_ALLOCATION_PROBE)
			add_executable(gargantuan_renderer_allocation_benchmark
				"tests/RendererBenchmark.cpp" "tests/BenchmarkAllocationProbe.cpp")
			target_compile_definitions(gargantuan_renderer_allocation_benchmark PRIVATE
				GARGANTUAN_BENCHMARK_ALLOCATION_PROBE=1)
			target_link_libraries(gargantuan_renderer_allocation_benchmark PRIVATE gargantuan_core)
			add_executable(gargantuan_runtime6_allocation_benchmark
				"tests/RendererFoundation2Benchmark.cpp" "tests/BenchmarkAllocationProbe.cpp")
			target_compile_definitions(gargantuan_runtime6_allocation_benchmark PRIVATE GARGANTUAN_RUNTIME6_ALLOCATION_PROBE=1)
			target_link_libraries(gargantuan_runtime6_allocation_benchmark PRIVATE gargantuan_core)
			add_executable(gargantuan_renderer_allocation_probe_tests
				"tests/BenchmarkAllocationProbe.cpp" "tests/BenchmarkAllocationProbeTests.cpp")
			target_compile_features(gargantuan_renderer_allocation_probe_tests PRIVATE cxx_std_17)
			add_test(NAME gargantuan_renderer_allocation_probe
				COMMAND gargantuan_renderer_allocation_probe_tests)
		endif()
		add_executable(gargantuan_renderer_foundation2_benchmark "tests/RendererFoundation2Benchmark.cpp")
		target_link_libraries(gargantuan_renderer_foundation2_benchmark PRIVATE gargantuan_core)
		add_executable(gargantuan_runtime_foundation6_benchmark "tests/RendererFoundation2Benchmark.cpp")
		target_link_libraries(gargantuan_runtime_foundation6_benchmark PRIVATE gargantuan_core)
		add_dependencies(gargantuan_runtime_foundation6_benchmark gargantuan_shaders)
		add_test(NAME gargantuan_runtime_foundation6_smoke COMMAND gargantuan_runtime_foundation6_benchmark
			1 --count=1000 --density-matrix --group-width=256 --split-construction --diagnostic-marks)
		add_test(NAME gargantuan_runtime6_live_render_headless_smoke
			COMMAND gargantuan_runtime_foundation6_benchmark
				3 --count=1000 --changes=1000 --draw-backend=headless)
		set_tests_properties(gargantuan_runtime6_live_render_headless_smoke PROPERTIES
			LABELS "renderer;runtime;headless" TIMEOUT 120)
		add_test(NAME gargantuan_runtime6_live_render_invalid_backend
			COMMAND gargantuan_runtime_foundation6_benchmark 1 --count=1 --draw-backend=invalid)
		add_test(NAME gargantuan_runtime6_live_render_missing_backend
			COMMAND gargantuan_runtime_foundation6_benchmark 1 --count=1 --draw-backend=)
		add_test(NAME gargantuan_runtime6_live_render_invalid_instancing
			COMMAND gargantuan_runtime_foundation6_benchmark 1 --count=1 --draw-instancing=contiguous)
		set_tests_properties(gargantuan_runtime6_live_render_invalid_backend
			gargantuan_runtime6_live_render_missing_backend
			gargantuan_runtime6_live_render_invalid_instancing PROPERTIES
			WILL_FAIL TRUE LABELS "renderer;runtime;argument-rejection" TIMEOUT 20)
		add_executable(gargantuan_runtime6_extraction_alternatives "tests/Runtime6ExtractionAlternativesProbe.cpp")
		target_link_libraries(gargantuan_runtime6_extraction_alternatives PRIVATE gargantuan_core)
		add_test(NAME gargantuan_runtime6_extraction_alternatives COMMAND gargantuan_runtime6_extraction_alternatives
			128 32 2 random 1)
		add_executable(gargantuan_runtime6_physics_sync_benchmark "tests/Runtime6PhysicsSyncBenchmark.cpp")
		target_link_libraries(gargantuan_runtime6_physics_sync_benchmark PRIVATE gargantuan_core)
		add_test(NAME gargantuan_runtime6_physics_sync_smoke COMMAND gargantuan_runtime6_physics_sync_benchmark
			--static-count 128 --dynamic-count 16 --frames 3 --warmup 1 --control-density 0.25 --lifecycle-per-frame 1 --publication)
		add_executable(gargantuan_runtime6_realistic_publication_probe
			"tests/Runtime6RealisticPublicationProbe.cpp")
		target_link_libraries(gargantuan_runtime6_realistic_publication_probe PRIVATE gargantuan_core)
		add_test(NAME gargantuan_runtime6_realistic_publication_probe_smoke
			COMMAND gargantuan_runtime6_realistic_publication_probe
				--count=16 --changed=4 --frames=2 --mode=both)
		set_tests_properties(gargantuan_runtime6_realistic_publication_probe_smoke PROPERTIES
			LABELS "renderer;runtime;headless" TIMEOUT 120)
		add_test(NAME gargantuan_runtime6_realistic_publication_probe_quiet_smoke
			COMMAND gargantuan_runtime6_realistic_publication_probe
				--count=16 --changed=0 --frames=2 --mode=both)
		set_tests_properties(gargantuan_runtime6_realistic_publication_probe_quiet_smoke PROPERTIES
			LABELS "renderer;runtime;headless" TIMEOUT 120)
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
		add_executable(gargantuan_ibl_gpu_tests "tests/ImageBasedLightingGpuTests.cpp")
		add_executable(gargantuan_reflection_probe_gpu_tests "tests/ReflectionProbeGpuTests.cpp")
		target_link_libraries(gargantuan_reflection_probe_gpu_tests PRIVATE gargantuan_core)
		add_dependencies(gargantuan_reflection_probe_gpu_tests gargantuan_shaders)
		add_executable(gargantuan_shadow_foundation5_gpu_tests "tests/ShadowFoundation5GpuTests.cpp")
		target_link_libraries(gargantuan_shadow_foundation5_gpu_tests PRIVATE gargantuan_core)
		add_dependencies(gargantuan_shadow_foundation5_gpu_tests gargantuan_shaders)
		target_link_libraries(gargantuan_ibl_gpu_tests PRIVATE gargantuan_core)
		add_dependencies(gargantuan_ibl_gpu_tests gargantuan_shaders)
		add_executable(gargantuan_renderer_depth_tests "tests/RendererDepthTests.cpp")
		target_link_libraries(gargantuan_renderer_depth_tests PRIVATE gargantuan_core)
		add_dependencies(gargantuan_renderer_depth_tests gargantuan_shaders)
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
		add_test(NAME gargantuan_gas_serialization_benchmark COMMAND gargantuan_serialization_benchmark --gas)
		set_tests_properties(gargantuan_gas_serialization_benchmark PROPERTIES LABELS "serialization;benchmark" TIMEOUT 180)
		if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
			add_executable(gargantuan_gas_benchmark_profile
				"tests/SerializationBenchmark.cpp" "tests/GasBenchmarkAllocations.cpp" "tests/BenchmarkAllocationProbe.cpp")
			target_compile_definitions(gargantuan_gas_benchmark_profile PRIVATE GARGANTUAN_GAS_BENCHMARK_PROFILE=1)
			target_link_libraries(gargantuan_gas_benchmark_profile PRIVATE gargantuan_core)
			if(WIN32)
				target_link_libraries(gargantuan_gas_benchmark_profile PRIVATE Psapi)
			endif()
			foreach(GasProfileBackend JSON GAS)
				foreach(GasProfileObjects 100 1000 5000)
					set(GasProfileTest "gargantuan_gas_profile_${GasProfileBackend}_${GasProfileObjects}")
					add_test(NAME "${GasProfileTest}" COMMAND gargantuan_gas_benchmark_profile
						--gas-profile "${GasProfileBackend}" "${GasProfileObjects}")
					set_tests_properties("${GasProfileTest}" PROPERTIES LABELS "serialization;benchmark" TIMEOUT 180)
				endforeach()
			endforeach()
		endif()
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
			gargantuan_spatial_construction_integration_tests
			gargantuan_foundation_tests
			gargantuan_runtime_package_crypto_tests
			gargantuan_gas_document_tests
			gargantuan_gas_authoring_tests
			gargantuan_prepared_property_tests
			gargantuan_concrete_property_default_tests
			gargantuan_asset_foundation_tests
			gargantuan_asset_foundation_benchmark
			gargantuan_animation_foundation_tests
			gargantuan_animation_preview_editorhost_tests
			gargantuan_character_composition_inspection_tests
			gargantuan_attachment_mesh_tests
			gargantuan_character_authoring_editorhost_tests
			gargantuan_animation_foundation_benchmark
			gargantuan_audio_foundation_tests
			gargantuan_audio_foundation_benchmark
			gargantuan_renderer_publication_tests
			gargantuan_environment_lighting_benchmark
			gargantuan_prerun_bootstrap_tests
			gargantuan_prerun_editor_host_tests
			gargantuan_replicated_storage_tests
			gargantuan_replicated_storage_editor_host_tests
			gargantuan_replicated_storage_session_tests
			gargantuan_attributes_tags_profile
			gargantuan_physics_backend_tests
			gargantuan_runtime6_readback_fault_tests
			gargantuan_runtime6_dirty_insert_fault_tests
			gargantuan_runtime6_physics_listener_probe
			gargantuan_physics_query_benchmark
			gargantuan_physics_activation_benchmark
			gargantuan_soft_body_physics_tests
			gargantuan_soft_body_physics_benchmark
			gargantuan_platform_input_boundary_tests
			gargantuan_player_runtime_tests
			gargantuan_input_action_tests
			gargantuan_script_document_binding_tests
			gargantuan_root_debug_vm_tests
			gargantuan_input_action_benchmark
			gargantuan_interaction_foundation_tests
			gargantuan_interaction_foundation_benchmark
			gargantuan_entitlement_service_tests
			gargantuan_persistence_runtime_tests
			gargantuan_local_persistence_provider_tests
			gargantuan_entitlement_provider_benchmark
			gargantuan_gui_foundation_tests
			gargantuan_viewport_frame_tests
			gargantuan_first_complete_game_tests
			gargantuan_packaging_tests
			gargantuan_packaging_editor_host_tests
			gargantuan_packaging_benchmark
			gargantuan_qualified_scale_package
			gargantuan_gui_foundation_benchmark
			gargantuan_networking_contract_tests
			gargantuan_simulated_transport_tests
			gargantuan_scheduler_contract_tests
			gargantuan_replication_tests
			gargantuan_remote_tests
			gargantuan_remote_luau_tests
			gargantuan_runtime6_luau_mutation_benchmark
			gargantuan_character_networking_tests
			gargantuan_game_session_tests
			gargantuan_gameplay_allocation_workload
			gargantuan_gameplay_sample_tests
			gargantuan_gameplay_input_tests
			gargantuan_gameplay_pose_tests
			gargantuan_character_networking_benchmark
			gargantuan_content_availability_tests
			gargantuan_content_coarse_policy_tests
			gargantuan_residency_demand_policy_tests
			gargantuan_residency_demand_integration_tests
			gargantuan_residency_demand_benchmark
			gargantuan_residency_demand_startup_tests
			gargantuan_residency_streaming_scenario_tests
			gargantuan_residency_spatial_metadata_tests
			gargantuan_streaming_diagnostics_tests
			gargantuan_package_spatial_metadata_tests
			gargantuan_cross_foundation_streaming_tests
			gargantuan_cross_foundation_independent_bounds_tests
			gargantuan_cross_foundation_independent_consumer_tests
			gargantuan_cross_foundation_independent_performance_tests
			gargantuan_cross_foundation_independent_lifecycle_tests
			gargantuan_cross_foundation_independent_authoring_tests
			gargantuan_mesh_geometry_relevance_tests
			gargantuan_content_ownership_tests
			gargantuan_content_availability_benchmark
			gargantuan_game_session_benchmark
			gargantuan_replication_benchmark
			gargantuan_remote_benchmark
		)
		if(GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS gargantuan_serialization_benchmark gargantuan_gas_benchmark_profile)
		endif()
		if(GARGANTUAN_WITH_GNS)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS
				gargantuan_real_transport_tests
				gargantuan_remote_real_transport_tests
				gargantuan_character_real_transport_tests
				gargantuan_game_session_real_transport_tests
				gargantuan_physical_gns_funding_probe
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
				add_custom_command(TARGET gargantuan_gameplay_sample_tests POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_gameplay_sample_tests>"
				)
				add_custom_command(TARGET gargantuan_game_session_tests POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_game_session_tests>"
				)
				add_custom_command(TARGET gargantuan_gns_capacity_benchmark POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_gns_capacity_benchmark>"
				)
				add_custom_command(TARGET gargantuan_game_session_real_transport_tests POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_game_session_real_transport_tests>"
				)
				add_custom_command(TARGET gargantuan_physical_gns_funding_probe POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_physical_gns_funding_probe>"
				)
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
				add_custom_command(TARGET gargantuan_character_real_transport_tests POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${GARGANTUAN_PROTOBUF_RUNTIME}"
						"$<TARGET_FILE_DIR:gargantuan_character_real_transport_tests>"
				)
			endforeach()
		endif()
		if(GARGANTUAN_BUILD_RENDERER_BENCHMARKS)
			list(APPEND GARGANTUAN_SDL_RUNTIME_TARGETS
				gargantuan_renderer_benchmark
				gargantuan_renderer_foundation2_benchmark
				gargantuan_runtime_foundation6_benchmark
				gargantuan_renderer_foundation2_gpu_benchmark
				gargantuan_first_complete_game_gpu_tests
				gargantuan_editor_viewport_smoke
				gargantuan_ibl_gpu_tests
				gargantuan_renderer_depth_tests
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
	add_test(NAME gargantuan_animation_preview_editorhost COMMAND gargantuan_animation_preview_editorhost_tests)
	add_test(NAME gargantuan_character_composition_inspection COMMAND gargantuan_character_composition_inspection_tests)
	add_test(NAME gargantuan_attachment_mesh COMMAND gargantuan_attachment_mesh_tests)
	add_test(NAME gargantuan_character_authoring_editorhost COMMAND gargantuan_character_authoring_editorhost_tests)
	set_tests_properties(gargantuan_character_composition_inspection gargantuan_character_authoring_editorhost
		PROPERTIES LABELS "editor;character;animation;authoring" TIMEOUT 120)
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
	add_test(NAME gargantuan_input_action COMMAND gargantuan_input_action_tests)
	add_test(NAME gargantuan_script_document_binding COMMAND gargantuan_script_document_binding_tests)
	add_test(NAME gargantuan_root_debug_vm COMMAND gargantuan_root_debug_vm_tests)
	add_test(NAME gargantuan_input_action_benchmark_smoke COMMAND gargantuan_input_action_benchmark --quick)
	add_test(NAME gargantuan_interaction_foundation COMMAND gargantuan_interaction_foundation_tests)
	add_test(NAME gargantuan_interaction_foundation_benchmark_smoke
		COMMAND gargantuan_interaction_foundation_benchmark --quick)
	add_test(NAME gargantuan_entitlement_service COMMAND gargantuan_entitlement_service_tests)
	add_test(NAME gargantuan_persistence_runtime COMMAND gargantuan_persistence_runtime_tests)
	add_test(NAME gargantuan_local_persistence_provider COMMAND gargantuan_local_persistence_provider_tests)
	set_tests_properties(gargantuan_persistence_runtime gargantuan_local_persistence_provider
		PROPERTIES LABELS "persistence;runtime;headless" TIMEOUT 60)
	add_test(NAME gargantuan_entitlement_provider_benchmark_smoke
		COMMAND gargantuan_entitlement_provider_benchmark --quick)
	add_test(NAME gargantuan_gui_foundation COMMAND gargantuan_gui_foundation_tests)
	add_test(NAME gargantuan_viewport_frame COMMAND gargantuan_viewport_frame_tests)
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
	add_test(NAME gargantuan_dedicated_server_package_smoke
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_PACKAGER=$<TARGET_FILE:gargantuan_packager>
			-DGARGANTUAN_SERVER_RUNTIME_DISTRIBUTION=$<TARGET_FILE_DIR:gargantuan_server>/ServerRuntimeDistribution
			-DGARGANTUAN_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame
			-DGARGANTUAN_PACKAGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-tests
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/DedicatedServerPackageTests.cmake
	)
	add_test(NAME gargantuan_gas_package_input
		COMMAND ${CMAKE_COMMAND}
			-DGARGANTUAN_PACKAGER=$<TARGET_FILE:gargantuan_packager>
			-DGARGANTUAN_RUNTIME_DISTRIBUTION=$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution
			-DGARGANTUAN_SERVER_RUNTIME_DISTRIBUTION=$<TARGET_FILE_DIR:gargantuan_server>/ServerRuntimeDistribution
			-DGARGANTUAN_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame
			-DGARGANTUAN_PACKAGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-tests
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/GasPackageInputTests.cmake
	)
	set_tests_properties(gargantuan_gas_package_input PROPERTIES LABELS "persistence;packaging" TIMEOUT 300)
	add_test(NAME gargantuan_gui_foundation_benchmark_smoke COMMAND gargantuan_gui_foundation_benchmark --quick)
	add_test(NAME gargantuan_prerun_bootstrap COMMAND gargantuan_prerun_bootstrap_tests)
	add_test(NAME gargantuan_networking_contracts COMMAND gargantuan_networking_contract_tests)
	add_test(NAME gargantuan_farm_admission_evidence COMMAND gargantuan_farm_admission_evidence_tests)
	add_test(NAME gargantuan_simulated_transport COMMAND gargantuan_simulated_transport_tests)
	add_test(NAME gargantuan_scheduler_contract COMMAND gargantuan_scheduler_contract_tests)
	add_test(NAME gargantuan_replication COMMAND gargantuan_replication_tests)
	add_test(NAME gargantuan_replication_relevance COMMAND gargantuan_replication_relevance_tests)
	add_test(NAME gargantuan_spatial_domain COMMAND gargantuan_spatial_domain_tests)
	set_tests_properties(gargantuan_spatial_domain PROPERTIES LABELS "spatial;physics;rendering;headless" TIMEOUT 30)
	add_test(NAME gargantuan_spatial_transfer COMMAND gargantuan_spatial_transfer_tests)
	set_tests_properties(gargantuan_spatial_transfer PROPERTIES LABELS "spatial;physics;network;headless" TIMEOUT 30)
	add_test(NAME gargantuan_spatial_region_index COMMAND gargantuan_spatial_region_index_tests)
	add_test(NAME gargantuan_spatial_region_index_benchmark_smoke
		COMMAND gargantuan_spatial_region_index_benchmark --quick)
	add_test(NAME gargantuan_remote COMMAND gargantuan_remote_tests)
	add_test(NAME gargantuan_remote_luau COMMAND gargantuan_remote_luau_tests)
	add_test(NAME gargantuan_runtime6_luau_mutation_cframe_smoke
		COMMAND gargantuan_runtime6_luau_mutation_benchmark --count=16 --frames=2 --listeners=2 --property=cframe)
	add_test(NAME gargantuan_runtime6_luau_mutation_color_smoke
		COMMAND gargantuan_runtime6_luau_mutation_benchmark --count=16 --frames=2 --listeners=2 --property=color)
	set_tests_properties(gargantuan_runtime6_luau_mutation_cframe_smoke gargantuan_runtime6_luau_mutation_color_smoke
		PROPERTIES LABELS "runtime;script;headless" TIMEOUT 120)
	add_test(NAME gargantuan_character_networking COMMAND gargantuan_character_networking_tests)
	add_test(NAME gargantuan_character_networking_benchmark_smoke COMMAND gargantuan_character_networking_benchmark)
	add_test(NAME gargantuan_content_availability_benchmark_smoke
		COMMAND gargantuan_content_availability_benchmark --quick)
	add_test(NAME gargantuan_replication_benchmark_smoke COMMAND gargantuan_replication_benchmark)
	add_test(NAME gargantuan_remote_benchmark_smoke COMMAND gargantuan_remote_benchmark)
	set_tests_properties(gargantuan_shader_tool_validation PROPERTIES LABELS "build;shader" TIMEOUT 60)
	set_tests_properties(gargantuan_content_availability PROPERTIES
		LABELS "content;packaging;runtime;lifecycle;security" TIMEOUT 180)
	set_tests_properties(gargantuan_content_availability_benchmark_smoke PROPERTIES
		LABELS "content;packaging;runtime;benchmark" TIMEOUT 180)
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
	set_tests_properties(gargantuan_input_action gargantuan_input_action_benchmark_smoke PROPERTIES LABELS "input;runtime;headless" TIMEOUT 180)
	set_tests_properties(gargantuan_script_document_binding PROPERTIES LABELS "editor;persistence;headless" TIMEOUT 180)
	set_tests_properties(gargantuan_root_debug_vm PROPERTIES LABELS "editor;scripting;headless" TIMEOUT 20)
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
	set_tests_properties(gargantuan_dedicated_server_package_smoke PROPERTIES
		LABELS "packaging;server;runtime;lifecycle;security;headless" TIMEOUT 180
	)
	set_tests_properties(gargantuan_prerun_bootstrap PROPERTIES LABELS "prerun;runtime" TIMEOUT 120)
	set_tests_properties(gargantuan_networking_contracts gargantuan_simulated_transport gargantuan_scheduler_contract
		gargantuan_replication gargantuan_replication_relevance gargantuan_spatial_region_index
		gargantuan_spatial_region_index_benchmark_smoke
		gargantuan_remote gargantuan_remote_luau gargantuan_character_networking
		GameSessionTests
		gargantuan_character_networking_benchmark_smoke
		gargantuan_replication_benchmark_smoke
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
		add_test(NAME gargantuan_gns_failed_send_retry_safety COMMAND gargantuan_real_transport_tests --ack-retry-safety)
		set_tests_properties(gargantuan_gns_failed_send_retry_safety
			PROPERTIES LABELS "networking;optional;sanitizer-safety" TIMEOUT 45 RUN_SERIAL TRUE)
		add_test(NAME gargantuan_gns_funded_ack COMMAND gargantuan_real_transport_tests --ack-cycle-funded-hybrid 1348 --non-realtime)
		add_test(NAME gargantuan_gns_funded_ack_compatibility COMMAND gargantuan_real_transport_tests --ack-funded-compatibility-hybrid --non-realtime)
		add_test(NAME gargantuan_gns_funded_ack_four_grant_hybrid COMMAND gargantuan_real_transport_tests --ack-funded-four-grant-hybrid --non-realtime)
		set_tests_properties(gargantuan_gns_funded_ack gargantuan_gns_funded_ack_compatibility
			gargantuan_gns_funded_ack_four_grant_hybrid PROPERTIES LABELS "networking;optional;f1;funded-ack" TIMEOUT 60 RUN_SERIAL TRUE)
		add_test(NAME gargantuan_four_grant_contract COMMAND gargantuan_real_transport_tests --four-grant-contract)
		set_tests_properties(gargantuan_four_grant_contract PROPERTIES LABELS "networking;optional;contract" TIMEOUT 10)
		# Actual pinned 5/20-second control paths: one control/funded pair, not
		# repeated in each workload fixture or the sanitizer timing matrix.
		add_test(NAME gargantuan_gns_funded_ack_stats COMMAND gargantuan_real_transport_tests --ack-stats-boundary --non-realtime)
		set_tests_properties(gargantuan_gns_funded_ack_stats
			PROPERTIES LABELS "networking;optional;funded-ack" TIMEOUT 100 RUN_SERIAL TRUE)
		add_test(NAME gargantuan_hybrid_packet_tail COMMAND gargantuan_real_transport_tests --packet-tail-hybrid --non-realtime)
		# Standard releases qualify D03; --four-grant remains an explicit legacy probe.
		add_test(NAME gargantuan_hybrid_four_grant COMMAND gargantuan_real_transport_tests --four-grant-hybrid --non-realtime)
		add_test(NAME gargantuan_f1_four_grant_sanitizer_safety
			COMMAND gargantuan_real_transport_tests --four-grant-sanitizer-safety)
		add_test(NAME gargantuan_hybrid_mixed_traffic COMMAND gargantuan_real_transport_tests --mixed-traffic-hybrid --non-realtime)
		set_tests_properties(gargantuan_hybrid_packet_tail gargantuan_hybrid_four_grant
			gargantuan_f1_four_grant_sanitizer_safety gargantuan_hybrid_mixed_traffic
			PROPERTIES LABELS "networking;optional;f1" TIMEOUT 30 RUN_SERIAL TRUE)
		add_test(NAME gargantuan_pooled_service_source_contract
			COMMAND ${CMAKE_COMMAND}
				"-DGnsRoot=${gamenetworkingsockets_SOURCE_DIR}"
				"-DGargantuanRoot=${CMAKE_CURRENT_SOURCE_DIR}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/PooledServiceSourceChecks.cmake")
		set_tests_properties(gargantuan_pooled_service_source_contract
			PROPERTIES LABELS "networking;optional" TIMEOUT 10)
		add_test(NAME gargantuan_gns_observe_source_contract
			COMMAND ${CMAKE_COMMAND}
				"-DGargantuanRoot=${CMAKE_CURRENT_SOURCE_DIR}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/GnsObserveSourceChecks.cmake")
		set_tests_properties(gargantuan_gns_observe_source_contract
			PROPERTIES LABELS "networking;optional;f1" TIMEOUT 10)
		add_test(NAME gargantuan_remote_real_transport COMMAND gargantuan_remote_real_transport_tests)
		add_test(NAME gargantuan_character_real_transport COMMAND gargantuan_character_real_transport_tests)
		add_test(NAME gargantuan_game_session_real_transport COMMAND gargantuan_game_session_real_transport_tests)
		add_test(NAME gargantuan_pooled_feedback_refresh
			COMMAND gargantuan_game_session_real_transport_tests --pooled-feedback-refresh)
		set_tests_properties(gargantuan_pooled_feedback_refresh PROPERTIES
			LABELS "networking;transport;optional;pooled;feedback" TIMEOUT 180 RUN_SERIAL TRUE)
		add_test(NAME gargantuan_physical_gns_probe_self_test COMMAND gargantuan_physical_gns_funding_probe --self-test)
		add_test(NAME gargantuan_physical_gns_probe_invalid_arguments COMMAND gargantuan_physical_gns_funding_probe server 10.253.3.2 39450 3)
		set_tests_properties(gargantuan_physical_gns_probe_self_test gargantuan_physical_gns_probe_invalid_arguments
			PROPERTIES LABELS "networking;headless;optional" TIMEOUT 10)
		set_tests_properties(gargantuan_physical_gns_probe_invalid_arguments PROPERTIES WILL_FAIL TRUE)
		if(WIN32)
			add_test(NAME gargantuan_packaged_game_session
				COMMAND powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
					-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackagedGameSession.ps1"
					-Packager "$<TARGET_FILE:gargantuan_packager>"
					-PlayerRuntimeDistribution "$<TARGET_FILE_DIR:gargantuan_player>/RuntimeDistribution"
					-ServerRuntimeDistribution "$<TARGET_FILE_DIR:gargantuan_server>/ServerRuntimeDistribution"
					-ProjectRoot "${CMAKE_CURRENT_SOURCE_DIR}/samples/FirstCompleteGame"
					-GraphicalClient $<IF:$<BOOL:${GARGANTUAN_RUN_GRAPHICAL_PACKAGE_SMOKE}>,ON,OFF>
			)
			set_tests_properties(gargantuan_packaged_game_session PROPERTIES
				LABELS "networking;transport;packaging;optional" TIMEOUT 60 RUN_SERIAL TRUE
			)
		endif()
		set_tests_properties(gargantuan_real_transport gargantuan_remote_real_transport
			gargantuan_character_real_transport gargantuan_game_session_real_transport PROPERTIES
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
