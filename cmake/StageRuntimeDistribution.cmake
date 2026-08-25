cmake_minimum_required(VERSION 3.25)

foreach(GARGANTUAN_REQUIRED_VARIABLE
	GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT
	GARGANTUAN_PLAYER_FILE
	GARGANTUAN_SDL_FILE
	GARGANTUAN_BINARY_DIR
	GARGANTUAN_SOURCE_DIR
	GARGANTUAN_PLATFORM
)
	if(NOT DEFINED ${GARGANTUAN_REQUIRED_VARIABLE} OR "${${GARGANTUAN_REQUIRED_VARIABLE}}" STREQUAL "")
		message(FATAL_ERROR "Missing ${GARGANTUAN_REQUIRED_VARIABLE}")
	endif()
endforeach()

if(NOT EXISTS "${GARGANTUAN_PLAYER_FILE}" OR NOT EXISTS "${GARGANTUAN_SDL_FILE}")
	message(FATAL_ERROR "The player or SDL runtime is unavailable")
endif()

file(MAKE_DIRECTORY "${GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT}")

function(GargantuanStageRuntimeFile Source RelativePath Category)
	if(NOT EXISTS "${Source}" OR IS_DIRECTORY "${Source}")
		message(FATAL_ERROR "Required runtime input is missing: ${Source}")
	endif()
	get_filename_component(DestinationDirectory
		"${GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT}/${RelativePath}" DIRECTORY)
	file(MAKE_DIRECTORY "${DestinationDirectory}")
	file(COPY_FILE "${Source}" "${GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT}/${RelativePath}" ONLY_IF_DIFFERENT)
	string(SHA256 GARGANTUAN_RUNTIME_PATH_KEY "${RelativePath}")
	get_property(GARGANTUAN_RUNTIME_CATEGORY_SET GLOBAL
		PROPERTY "GARGANTUAN_RUNTIME_CATEGORY_${GARGANTUAN_RUNTIME_PATH_KEY}" SET)
	if(GARGANTUAN_RUNTIME_CATEGORY_SET)
		message(FATAL_ERROR "Duplicate runtime distribution path: ${RelativePath}")
	endif()
	set_property(GLOBAL APPEND PROPERTY GARGANTUAN_RUNTIME_PATHS "${RelativePath}")
	set_property(GLOBAL PROPERTY "GARGANTUAN_RUNTIME_CATEGORY_${GARGANTUAN_RUNTIME_PATH_KEY}" "${Category}")
endfunction()

get_filename_component(GARGANTUAN_PLAYER_NAME "${GARGANTUAN_PLAYER_FILE}" NAME)
get_filename_component(GARGANTUAN_SDL_NAME "${GARGANTUAN_SDL_FILE}" NAME)
GargantuanStageRuntimeFile("${GARGANTUAN_PLAYER_FILE}" "${GARGANTUAN_PLAYER_NAME}" Runtime)
GargantuanStageRuntimeFile("${GARGANTUAN_SDL_FILE}" "${GARGANTUAN_SDL_NAME}" Runtime)
if(DEFINED GARGANTUAN_SDL_SONAME_FILE AND NOT "${GARGANTUAN_SDL_SONAME_FILE}" STREQUAL "" AND
	EXISTS "${GARGANTUAN_SDL_SONAME_FILE}")
	get_filename_component(GARGANTUAN_SDL_SONAME "${GARGANTUAN_SDL_SONAME_FILE}" NAME)
	if(NOT GARGANTUAN_SDL_SONAME STREQUAL GARGANTUAN_SDL_NAME)
		GargantuanStageRuntimeFile("${GARGANTUAN_SDL_SONAME_FILE}" "${GARGANTUAN_SDL_SONAME}" Runtime)
	endif()
endif()
if(DEFINED GARGANTUAN_SYSTEM_RUNTIME_COUNT AND GARGANTUAN_SYSTEM_RUNTIME_COUNT GREATER 0)
	math(EXPR GARGANTUAN_SYSTEM_RUNTIME_LAST "${GARGANTUAN_SYSTEM_RUNTIME_COUNT} - 1")
	foreach(GARGANTUAN_SYSTEM_RUNTIME_INDEX RANGE 0 ${GARGANTUAN_SYSTEM_RUNTIME_LAST})
		set(GARGANTUAN_SYSTEM_RUNTIME_SOURCE
			"${GARGANTUAN_SYSTEM_RUNTIME_FILE_${GARGANTUAN_SYSTEM_RUNTIME_INDEX}}")
		get_filename_component(GARGANTUAN_SYSTEM_RUNTIME_NAME "${GARGANTUAN_SYSTEM_RUNTIME_SOURCE}" NAME)
		GargantuanStageRuntimeFile(
			"${GARGANTUAN_SYSTEM_RUNTIME_SOURCE}"
			"${GARGANTUAN_SYSTEM_RUNTIME_NAME}"
			Runtime
		)
	endforeach()
endif()

foreach(GARGANTUAN_RUNTIME_MODULE
	DefaultActionMap.luau
	DefaultCamera.luau
	DefaultPlayerController.luau
	DefaultPlayerRuntime.luau
	GargantuanSans.ttf
)
	GargantuanStageRuntimeFile(
		"${GARGANTUAN_BINARY_DIR}/runtime/${GARGANTUAN_RUNTIME_MODULE}"
		"runtime/${GARGANTUAN_RUNTIME_MODULE}"
		Runtime
	)
endforeach()

foreach(GARGANTUAN_SHADER
	gui.frag.spv
	gui.vert.spv
	opaque.frag.spv
	opaque.vert.spv
	shadow.frag.spv
	shadow.vert.spv
)
	GargantuanStageRuntimeFile(
		"${GARGANTUAN_BINARY_DIR}/shaders/${GARGANTUAN_SHADER}"
		"shaders/${GARGANTUAN_SHADER}"
		Shader
	)
endforeach()

GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/LICENSE.md" "notices/Gargantuan.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/argparse/LICENSE" "notices/argparse.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/box3d/LICENSE" "notices/Box3D.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/glm/copying.txt" "notices/GLM.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/json/LICENSE.MIT" "notices/nlohmann-json.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/luau/LICENSE.txt" "notices/Luau.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/magicenum/LICENSE" "notices/magic-enum.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/sdl/LICENSE.txt" "notices/SDL3.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/sdl_image/LICENSE.txt" "notices/SDL3_image.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/sdl_ttf/LICENSE.txt" "notices/SDL3_ttf.txt" Notice)
GargantuanStageRuntimeFile("${GARGANTUAN_SOURCE_DIR}/vendor/tracy/LICENSE" "notices/Tracy.txt" Notice)
if(DEFINED GARGANTUAN_SYSTEM_RUNTIME_COUNT AND GARGANTUAN_SYSTEM_RUNTIME_COUNT GREATER 0)
	GargantuanStageRuntimeFile(
		"${GARGANTUAN_SOURCE_DIR}/cmake/notices/MSVC-Runtime.txt"
		"notices/MSVC-Runtime.txt"
		Notice
	)
endif()

get_property(GARGANTUAN_RUNTIME_PATHS GLOBAL PROPERTY GARGANTUAN_RUNTIME_PATHS)
list(SORT GARGANTUAN_RUNTIME_PATHS)
set(GARGANTUAN_MANIFEST_FILES "")
set(GARGANTUAN_SEPARATOR "")
foreach(GARGANTUAN_RUNTIME_PATH IN LISTS GARGANTUAN_RUNTIME_PATHS)
	string(SHA256 GARGANTUAN_RUNTIME_PATH_KEY "${GARGANTUAN_RUNTIME_PATH}")
	get_property(GARGANTUAN_RUNTIME_CATEGORY GLOBAL
		PROPERTY "GARGANTUAN_RUNTIME_CATEGORY_${GARGANTUAN_RUNTIME_PATH_KEY}")
	string(APPEND GARGANTUAN_MANIFEST_FILES
		"${GARGANTUAN_SEPARATOR}    {\"Path\":\"${GARGANTUAN_RUNTIME_PATH}\",\"Category\":\"${GARGANTUAN_RUNTIME_CATEGORY}\"}")
	set(GARGANTUAN_SEPARATOR ",\n")
endforeach()

file(WRITE "${GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT}/runtime-distribution.json"
"{\n  \"Format\": \"GargantuanRuntimeDistribution\",\n  \"Version\": 1,\n  \"RuntimeCompatibility\": 1,\n  \"Platform\": \"${GARGANTUAN_PLATFORM}\",\n  \"Player\": \"${GARGANTUAN_PLAYER_NAME}\",\n  \"Files\": [\n${GARGANTUAN_MANIFEST_FILES}\n  ]\n}\n")
