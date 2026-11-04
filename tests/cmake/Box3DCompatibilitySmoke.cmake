cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED GARGANTUAN_SOURCE_DIR OR NOT DEFINED BOX3D_SOURCE_DIR)
	message(FATAL_ERROR "GARGANTUAN_SOURCE_DIR and BOX3D_SOURCE_DIR are required")
endif()

include("${GARGANTUAN_SOURCE_DIR}/cmake/Box3DCompatibility.cmake")
GargantuanPrepareBox3DCompatibility("${BOX3D_SOURCE_DIR}" PrivateSource)
file(READ "${PrivateSource}/src/simd.h" PatchedSource)
if(NOT PatchedSource MATCHES "memcpy\\( &xyBits, src, sizeof\\( xyBits \\) \\);" OR
	PatchedSource MATCHES "_mm_load_sd\\( \\(const double\\*\\)\\( src \\) \\)")
	message(FATAL_ERROR "Private Box3D SIMD copy does not contain the checked alignment correction")
endif()
message(STATUS "Pinned Box3D source copied and SIMD correction applied only in ${PrivateSource}")
