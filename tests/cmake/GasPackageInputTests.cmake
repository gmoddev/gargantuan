cmake_minimum_required(VERSION 3.25)

foreach(RequiredVariable
	GARGANTUAN_PACKAGER
	GARGANTUAN_RUNTIME_DISTRIBUTION
	GARGANTUAN_SERVER_RUNTIME_DISTRIBUTION
	GARGANTUAN_PROJECT_ROOT
	GARGANTUAN_PACKAGE_TEST_ROOT
)
	if(NOT DEFINED ${RequiredVariable} OR "${${RequiredVariable}}" STREQUAL "")
		message(FATAL_ERROR "Missing ${RequiredVariable}")
	endif()
endforeach()

if(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
	file(TO_CMAKE_PATH "$ENV{TEMP}" Base)
elseif(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
	file(TO_CMAKE_PATH "$ENV{TMPDIR}" Base)
else()
	set(Base "${GARGANTUAN_PACKAGE_TEST_ROOT}")
endif()
file(REAL_PATH "${Base}" Base)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef Suffix)
cmake_path(APPEND Base "gas-${Suffix}" OUTPUT_VARIABLE Workspace)
if(EXISTS "${Workspace}")
	message(FATAL_ERROR "GAS package test workspace already exists")
endif()
file(MAKE_DIRECTORY "${Workspace}")

function(RemoveWorkspace)
	file(REAL_PATH "${Workspace}" Resolved)
	if(NOT "${Resolved}" STREQUAL "${Workspace}")
		message(FATAL_ERROR "GAS package test workspace moved")
	endif()
	file(REMOVE_RECURSE "${Resolved}")
endfunction()

function(Fail MessageText)
	RemoveWorkspace()
	message(FATAL_ERROR "${MessageText}")
endfunction()

function(RunPackager ExpectedExit)
	execute_process(
		COMMAND "${GARGANTUAN_PACKAGER}" ${ARGN}
		WORKING_DIRECTORY "${Workspace}"
		RESULT_VARIABLE Result
		OUTPUT_VARIABLE Output
		ERROR_VARIABLE Error
		TIMEOUT 90
	)
	if(NOT "${Result}" STREQUAL "${ExpectedExit}")
		Fail("Unexpected packager exit ${Result} (expected ${ExpectedExit}): ${Output}${Error}")
	endif()
	set(PackagerError "${Error}" PARENT_SCOPE)
endfunction()

set(Origin "${Workspace}/Origin")
set(Export "${Workspace}/Export.gasp")
set(Input "${Workspace}/Place.input")
set(Client "${Workspace}/Client")
set(Server "${Workspace}/Server")
set(Rejected "${Workspace}/Rejected")
file(COPY "${GARGANTUAN_PROJECT_ROOT}/" DESTINATION "${Origin}")
file(READ "${Origin}/.gargantuan/project.json" Metadata)
string(JSON ProjectId GET "${Metadata}" ProjectId)

file(WRITE "${Workspace}/Unrelated.gasp" "preserve this file")
RunPackager(3 export-gasp --project "${Origin}" --output "${Workspace}/Unrelated.gasp")
file(READ "${Workspace}/Unrelated.gasp" Unrelated)
if(NOT "${Unrelated}" STREQUAL "preserve this file")
	Fail("GASP export overwrote an unrelated file")
endif()
RunPackager(0 export-gasp --project "${Origin}" --output "${Export}")
file(RENAME "${Export}" "${Input}")
file(REMOVE "${Origin}/.gargantuan/project.instance.json")

RunPackager(0 build --project "${Origin}" --gasp "${Input}" --output "${Client}"
	--runtime "${GARGANTUAN_RUNTIME_DISTRIBUTION}")
RunPackager(0 validate "${Client}")
file(SHA256 "${Client}/game.package.json" InitialManifest)
RunPackager(0 build --project "${Origin}" --gasp "${Input}" --output "${Client}"
	--runtime "${GARGANTUAN_RUNTIME_DISTRIBUTION}")
file(SHA256 "${Client}/game.package.json" RepeatedManifest)
if(NOT "${InitialManifest}" STREQUAL "${RepeatedManifest}")
	Fail("The same GASP input generated different client package manifests")
endif()

RunPackager(0 build --project "${Origin}" --gasp "${Input}" --output "${Server}"
	--runtime "${GARGANTUAN_SERVER_RUNTIME_DISTRIBUTION}")
RunPackager(0 validate "${Server}")
foreach(RuntimePackage "${Client}" "${Server}")
	file(READ "${RuntimePackage}/game.package.json" Manifest)
	string(JSON PackagedProjectId GET "${Manifest}" ProjectId)
	if(NOT "${PackagedProjectId}" STREQUAL "${ProjectId}")
		Fail("The GASP package changed project identity")
	endif()
endforeach()

if(WIN32)
	set(ExecutableSuffix ".exe")
endif()
execute_process(
	COMMAND "${Client}/GargantuanPlayer${ExecutableSuffix}" --headless --startup-smoke
	WORKING_DIRECTORY "${Workspace}"
	RESULT_VARIABLE Result OUTPUT_VARIABLE Output ERROR_VARIABLE Error TIMEOUT 60
)
if(NOT "${Result}" STREQUAL "0")
	Fail("GASP client startup failed (${Result}): ${Output}${Error}")
endif()
execute_process(
	COMMAND "${Server}/GargantuanServer${ExecutableSuffix}" --startup-smoke --max-ticks 12
	WORKING_DIRECTORY "${Workspace}"
	RESULT_VARIABLE Result OUTPUT_VARIABLE Output ERROR_VARIABLE Error TIMEOUT 60
)
if(NOT "${Result}" STREQUAL "0")
	Fail("GASP server startup failed (${Result}): ${Output}${Error}")
endif()

set(JsonPath "${Origin}/.gargantuan/project.instance.json")
set(BinaryPath "${Origin}/.gargantuan/project.instance.bin")
file(WRITE "${BinaryPath}" "preserve the existing binary project")
RunPackager(3 restore-gasp --project "${Origin}" --gasp "${Input}")
if(EXISTS "${JsonPath}")
	Fail("GASP restore published JSON beside an existing binary project")
endif()
file(READ "${BinaryPath}" Binary)
if(NOT "${Binary}" STREQUAL "preserve the existing binary project")
	Fail("GASP restore changed the existing binary project")
endif()
file(REMOVE "${BinaryPath}")
RunPackager(0 restore-gasp --project "${Origin}" --gasp "${Input}")
file(READ "${JsonPath}" Restored)
string(JSON RestoredVersion GET "${Restored}" Version)
if(NOT RestoredVersion EQUAL 5)
	Fail("GASP restore did not write current Instance JSON")
endif()
file(SHA256 "${JsonPath}" RestoredDigest)
RunPackager(3 restore-gasp --project "${Origin}" --gasp "${Input}")
file(SHA256 "${JsonPath}" ExistingDigest)
if(NOT "${RestoredDigest}" STREQUAL "${ExistingDigest}")
	Fail("GASP restore changed the existing project")
endif()
RunPackager(0 export-gasp --project "${Origin}" --output "${Export}")
file(SHA256 "${Input}" InitialGasDigest)
file(SHA256 "${Export}" ReexportedDigest)
if(NOT "${InitialGasDigest}" STREQUAL "${ReexportedDigest}")
	Fail("GASP changed after restoring its project representation and exporting it again")
endif()

file(WRITE "${Workspace}/Malformed.gasp" "not a GAS place")
RunPackager(3 build --project "${Origin}" --gasp "${Workspace}/Malformed.gasp" --output "${Rejected}")
if(NOT PackagerError MATCHES "Package:GAS")
	Fail("Malformed GASP rejection did not come from GAS admission")
endif()
RunPackager(2 build --project "${Origin}" --output "${Rejected}" --gasp)
RunPackager(2 build --project "${Origin}" --output "${Rejected}" "--gasp=${Input}")

file(WRITE "${Origin}/.gargantuan/prerun.luau" "error('Unexpected schema bootstrap')")
RunPackager(3 build --project "${Origin}" --gasp "${Input}" --output "${Rejected}")
if(NOT PackagerError MATCHES "Place PreRun source does not match")
	Fail("Mismatched PreRun was not rejected before bootstrap")
endif()
string(JSON OtherMetadata SET "${Metadata}" ProjectId "\"11111111111111111111111111111111\"")
file(WRITE "${Origin}/.gargantuan/project.json" "${OtherMetadata}")
RunPackager(3 build --project "${Origin}" --gasp "${Input}" --output "${Rejected}")
if(NOT PackagerError MATCHES "Place identity does not match")
	Fail("Mismatched project identity was not rejected before bootstrap")
endif()
if(EXISTS "${Rejected}")
	Fail("Rejected GASP input published package output")
endif()

RemoveWorkspace()
message(STATUS "GASP client/server packaging, startup, reproducibility and admission passed")
