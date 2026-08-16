include("${GARGANTUAN_SOURCE_DIR}/cmake/ShaderTools.cmake")

GargantuanValidateGlslc("C:/invalid/matc.exe" WrongToolValid WrongToolDiagnostic)
if(WrongToolValid OR NOT WrongToolDiagnostic MATCHES "must be glslc")
	message(FATAL_ERROR "matc was not rejected as an SDL/Vulkan shader compiler: ${WrongToolDiagnostic}")
endif()

GargantuanValidateGlslc("${CMAKE_CURRENT_BINARY_DIR}/missing/glslc.exe" MissingToolValid MissingToolDiagnostic)
if(MissingToolValid OR NOT MissingToolDiagnostic MATCHES "does not exist")
	message(FATAL_ERROR "A missing cached glslc path was not rejected: ${MissingToolDiagnostic}")
endif()

GargantuanValidateGlslc("${GARGANTUAN_GLSLC_EXECUTABLE}" RealToolValid RealToolDiagnostic)
if(NOT RealToolValid)
	message(FATAL_ERROR "The configured glslc executable failed validation: ${RealToolDiagnostic}")
endif()
