# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0

function(GargantuanAddSanitizerSource Name Directory)
	if(GargantuanSanitizerAliasDirectory)
		file(CREATE_LINK "${Directory}" "${GargantuanSanitizerAliasDirectory}/${Name}" SYMBOLIC)
	endif()
endfunction()

block(SCOPE_FOR VARIABLES PROPAGATE CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER GargantuanSanitizerAliasDirectory GargantuanSanitizerPython)
set(GargantuanSanitizerAliasDirectory "")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
	foreach(Language IN ITEMS C CXX)
		string(TOUPPER "${CMAKE_BUILD_TYPE}" Configuration)
		set(Flags "${CMAKE_${Language}_FLAGS} ${CMAKE_${Language}_FLAGS_${Configuration}}")
		if(CMAKE_${Language}_COMPILER_ID STREQUAL "Clang" AND Flags MATCHES "-fsanitize=([^ ,]+,)*(address|undefined)(,| |$)")
			if(NOT GargantuanSanitizerAliasDirectory)
				find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
				set(GargantuanSanitizerPython "${Python3_EXECUTABLE}")
				set(GargantuanSanitizerAliasDirectory "${CMAKE_BINARY_DIR}/CMakeFiles/GargantuanSources")
				file(MAKE_DIRECTORY "${GargantuanSanitizerAliasDirectory}")
				GargantuanAddSanitizerSource(Engine "${CMAKE_CURRENT_SOURCE_DIR}")
				# A launcher edit must invalidate previous object commands even on
				# generators which do not track the launcher's source as an input.
				set(Script "${CMAKE_CURRENT_LIST_DIR}/ClangSanitizerPaths.py")
				file(SHA256 "${Script}" Hash)
				set(Launcher "${GargantuanSanitizerAliasDirectory}/${Hash}.py")
				configure_file("${Script}" "${Launcher}" COPYONLY)
			endif()
			set(CMAKE_${Language}_COMPILER_LAUNCHER
				"${Python3_EXECUTABLE}" "${Launcher}" "${GargantuanSanitizerAliasDirectory}"
				${CMAKE_${Language}_COMPILER_LAUNCHER} --gargantuan-compiler)
		endif()
	endforeach()
endif()
endblock()
