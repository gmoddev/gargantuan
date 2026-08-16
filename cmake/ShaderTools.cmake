function(GargantuanValidateGlslc Executable ResultVariable ErrorVariable)
	if(NOT Executable)
		set(${ResultVariable} FALSE PARENT_SCOPE)
		set(${ErrorVariable} "No glslc executable was provided." PARENT_SCOPE)
		return()
	endif()

	get_filename_component(ExecutableName "${Executable}" NAME)
	string(TOLOWER "${ExecutableName}" ExecutableNameLower)
	if(NOT ExecutableNameLower STREQUAL "glslc" AND NOT ExecutableNameLower STREQUAL "glslc.exe")
		set(${ResultVariable} FALSE PARENT_SCOPE)
		set(${ErrorVariable}
			"The SDL/Vulkan shader compiler must be glslc, but '${Executable}' names '${ExecutableName}'. matc is only valid for the optional Filament material build."
			PARENT_SCOPE
		)
		return()
	endif()

	if(NOT EXISTS "${Executable}" OR IS_DIRECTORY "${Executable}")
		set(${ResultVariable} FALSE PARENT_SCOPE)
		set(${ErrorVariable} "The configured glslc executable does not exist: '${Executable}'." PARENT_SCOPE)
		return()
	endif()

	execute_process(
		COMMAND "${Executable}" --version
		RESULT_VARIABLE VersionResult
		OUTPUT_VARIABLE VersionOutput
		ERROR_VARIABLE VersionError
		TIMEOUT 10
	)
	if(NOT VersionResult EQUAL 0)
		string(STRIP "${VersionError}" VersionError)
		set(${ResultVariable} FALSE PARENT_SCOPE)
		set(${ErrorVariable}
			"The configured glslc executable could not report its version: '${Executable}' (${VersionError})."
			PARENT_SCOPE
		)
		return()
	endif()

	string(STRIP "${VersionOutput}" VersionOutput)
	set(${ResultVariable} TRUE PARENT_SCOPE)
	set(${ErrorVariable} "${VersionOutput}" PARENT_SCOPE)
endfunction()

function(GargantuanFindGlslc OutputVariable)
	set(CachedCandidate "${${OutputVariable}}")
	if(CachedCandidate AND NOT CachedCandidate MATCHES "-NOTFOUND$")
		get_filename_component(CachedName "${CachedCandidate}" NAME)
		string(TOLOWER "${CachedName}" CachedNameLower)
		if(NOT CachedNameLower STREQUAL "glslc" AND NOT CachedNameLower STREQUAL "glslc.exe")
			message(FATAL_ERROR
				"${OutputVariable} must identify glslc for the default SDL/Vulkan renderer. "
				"The cached value '${CachedCandidate}' is '${CachedName}'. matc is only used by the optional Filament configuration. "
				"Remove the invalid cache entry or pass -D${OutputVariable}=<path-to-glslc.exe>."
			)
		endif()
		if(NOT EXISTS "${CachedCandidate}")
			message(STATUS "Discarding stale cached glslc path: ${CachedCandidate}")
			unset(${OutputVariable} CACHE)
			unset(${OutputVariable})
		endif()
	endif()

	set(GlslcHints "")
	if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
		list(APPEND GlslcHints "$ENV{VULKAN_SDK}/Bin")
	endif()
	find_program(${OutputVariable}
		NAMES glslc
		HINTS ${GlslcHints}
		DOC "glslc executable used to compile SDL/Vulkan SPIR-V shaders"
	)
	if(NOT ${OutputVariable})
		message(FATAL_ERROR
			"glslc is required to compile shaders for Gargantuan's default SDL/Vulkan renderer. "
			"Install the Vulkan SDK or shaderc, set VULKAN_SDK, place glslc on PATH, or pass "
			"-D${OutputVariable}=<path-to-glslc.exe>. matc is not a compatible substitute."
		)
	endif()

	GargantuanValidateGlslc("${${OutputVariable}}" GlslcValid GlslcDiagnostic)
	if(NOT GlslcValid)
		message(FATAL_ERROR "${GlslcDiagnostic}")
	endif()
	string(REPLACE "\n" " | " GlslcVersion "${GlslcDiagnostic}")
	message(STATUS "Using glslc: ${${OutputVariable}} (${GlslcVersion})")
	set(${OutputVariable} "${${OutputVariable}}" PARENT_SCOPE)
endfunction()
