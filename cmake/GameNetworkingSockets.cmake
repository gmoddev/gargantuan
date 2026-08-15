include(FetchContent)

set(GARGANTUAN_GNS_REVISION "2cb93a06350bb065db53abdb0d87cf297e0bfd34")

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

# MSVC's C frontend does not advertise CMake's c_std_99 feature even though the
# pinned GNS sources compile as C99. Preserve the upstream C++ requirement while
# avoiding a configure-time feature-probe failure on supported Windows builds.
if(MSVC)
	set_property(TARGET GameNetworkingSockets_s PROPERTY COMPILE_FEATURES cxx_std_17)
	set_property(TARGET GameNetworkingSockets_s PROPERTY INTERFACE_COMPILE_FEATURES cxx_std_17)
endif()
