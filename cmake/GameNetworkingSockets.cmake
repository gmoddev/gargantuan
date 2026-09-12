include(FetchContent)

# Gargantuan's root project is C++-only, but pinned GNS publishes a c_std_99
# requirement for its C sources. Enable C in the parent GNS integration scope so
# CMake retains the compiler feature table when the fetched target is consumed.
enable_language(C)

set(GARGANTUAN_GNS_REVISION "2cb93a06350bb065db53abdb0d87cf297e0bfd34")

option(
	GARGANTUAN_GNS_UPSTREAM_UBSAN_COMPAT
	"Disable Clang's function-type UBSan check only for pinned GNS callback type erasure"
	OFF
)

set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC OFF CACHE BOOL "" FORCE)
set(USE_CRYPTO25519 Reference CACHE STRING "" FORCE)
if(WIN32)
	set(USE_CRYPTO BCrypt CACHE STRING "" FORCE)
else()
	set(USE_CRYPTO OpenSSL CACHE STRING "" FORCE)
endif()

FetchContent_Declare(
	GameNetworkingSockets
	GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
	GIT_TAG "${GARGANTUAN_GNS_REVISION}"
	GIT_SHALLOW FALSE
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(GameNetworkingSockets)

if(NOT TARGET GameNetworkingSockets::static)
	message(FATAL_ERROR "Pinned GameNetworkingSockets source did not provide GameNetworkingSockets::static")
endif()

# The pinned GNS low-level callback wrapper deliberately erases a typed callback
# through a void* function signature. Clang's function UBSan check diagnoses that
# third-party boundary before Gargantuan's adapter can execute. This opt-in switch
# is sanitizer-test infrastructure only: it disables that one sub-check on the
# upstream GNS C++ target while Gargantuan targets and all other sanitizer checks
# remain unchanged.
if(GARGANTUAN_GNS_UPSTREAM_UBSAN_COMPAT)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "GARGANTUAN_GNS_UPSTREAM_UBSAN_COMPAT is supported only with Clang")
	endif()
	target_compile_options(GameNetworkingSockets_s PRIVATE
		"$<$<COMPILE_LANGUAGE:CXX>:-fno-sanitize=function>"
	)
endif()

# MSVC's C frontend does not advertise CMake's c_std_99 feature even though the
# pinned GNS sources compile as C99. Preserve the upstream C++ requirement while
# avoiding a configure-time feature-probe failure on supported Windows builds.
if(MSVC)
	set_property(TARGET GameNetworkingSockets_s PROPERTY COMPILE_FEATURES cxx_std_17)
	set_property(TARGET GameNetworkingSockets_s PROPERTY INTERFACE_COMPILE_FEATURES cxx_std_17)
endif()
