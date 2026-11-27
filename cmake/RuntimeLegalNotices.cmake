# Keep both generated documents on existing required Notice paths in the closed
# runtime format. Player and Server have separate intermediate directories.
function(GargantuanWriteRuntimeNotices Directory)
	file(READ "${GARGANTUAN_SOURCE_DIR}/docs/src/content/docs/meta/license.mdx" License)
	string(FIND "${License}" "Mozilla Public License Version 2.0" Start)
	if(Start LESS 0 OR NOT License MATCHES "Exhibit B")
		message(FATAL_ERROR "The full Engine MPL 2.0 license is missing")
	endif()
	string(SUBSTRING "${License}" ${Start} -1 License)
	string(SHA256 LicenseDigest "${License}")
	if(NOT LicenseDigest STREQUAL "e6b6566f085df5746515c6d7e2edcaec0d2b77d527ac40d91409d783fb6c8508")
		message(FATAL_ERROR "Engine MPL text differs from the reviewed edition")
	endif()
	file(MAKE_DIRECTORY "${Directory}")
	if(GARGANTUAN_EXTERNAL_DISTRIBUTION AND NOT GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS AND NOT GARGANTUAN_COVERED_SOURCE_ARCHIVE)
		message(FATAL_ERROR "Supply a reviewed source archive or recipient retrieval instructions for this external runtime")
	endif()
	if(GARGANTUAN_COVERED_SOURCE_ARCHIVE)
		file(WRITE "${Directory}/Engine-MPL-2.0.txt" "${License}")
		find_program(GARGANTUAN_PYTHON NAMES python python3 REQUIRED)
		execute_process(COMMAND "${GARGANTUAN_PYTHON}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/distribution/runtime_source.py"
			"${Directory}/Engine-MPL-2.0.txt" "${GARGANTUAN_COVERED_SOURCE_ARCHIVE}"
			"${GARGANTUAN_RUNTIME_SOURCE_REVISION}" "${Directory}/Gargantuan.txt"
			COMMAND_ERROR_IS_FATAL ANY)
	elseif(GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS)
		file(READ "${GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS}" Directions)
		string(STRIP "${Directions}" Directions)
		if(NOT Directions)
			message(FATAL_ERROR "The supplied covered-source instructions are empty")
		endif()
	else()
		execute_process(COMMAND git rev-parse --show-toplevel WORKING_DIRECTORY "${GARGANTUAN_SOURCE_DIR}"
			OUTPUT_VARIABLE RepositoryRoot OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
		file(REAL_PATH "${GARGANTUAN_SOURCE_DIR}" SourceRoot)
		execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${GARGANTUAN_SOURCE_DIR}"
			OUTPUT_VARIABLE Revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE GitResult)
		execute_process(COMMAND git status --porcelain --untracked-files=normal --ignore-submodules=none
			WORKING_DIRECTORY "${GARGANTUAN_SOURCE_DIR}" OUTPUT_VARIABLE Changes
			OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE StatusResult)
		if(RepositoryRoot STREQUAL SourceRoot AND GitResult STREQUAL "0" AND StatusResult STREQUAL "0" AND NOT Changes)
			string(CONCAT Directions
				"Obtain the Engine source under MPL 2.0 from https://github.com/gmoddev/Gargantuan\n"
				"Clone that repository, check out ${Revision}, then run git submodule update --init --recursive.\n"
				"Revision: ${Revision}")
		else()
			set(Directions "Internal build only. No matching source route has been supplied for this working tree.")
		endif()
	endif()
	if(NOT GARGANTUAN_COVERED_SOURCE_ARCHIVE)
		file(WRITE "${Directory}/Gargantuan.txt" "Matching covered source\n\n${Directions}\n\n${License}")
	endif()

	set(TextStack "${GARGANTUAN_SOURCE_DIR}/vendor/sdl_ttf")
	file(READ "${TextStack}/LICENSE.txt" Sdl)
	file(READ "${TextStack}/external/freetype/docs/FTL.TXT" FreeType)
	file(READ "${TextStack}/external/harfbuzz/COPYING" HarfBuzz)
	# The pinned Manifold license is the complete Apache 2.0 license already
	# included with this distribution. Include its text with the font credit.
	file(READ "${GARGANTUAN_SOURCE_DIR}/vendor/manifold/LICENSE" Apache)
	if(NOT Apache MATCHES "Apache License" OR NOT Apache MATCHES "END OF TERMS AND CONDITIONS"
		OR NOT FreeType MATCHES "FreeType Project LICENSE" OR NOT HarfBuzz MATCHES "HarfBuzz")
		message(FATAL_ERROR "Review the changed text-stack licenses before staging this runtime")
	endif()
	file(WRITE "${Directory}/SDL3_ttf.txt" "SDL_ttf\n\n${Sdl}\n\n"
		"Portions of this software are copyright © 1996-2024 The FreeType Project (www.freetype.org). All rights reserved.\n\n"
		"${FreeType}\n\nHarfBuzz\n\n${HarfBuzz}\n\n"
		"Roboto Regular (GargantuanSans.ttf), Copyright Google. Distributed under Apache License 2.0.\n\n${Apache}")
endfunction()
