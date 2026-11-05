# The compiled subset is copied unchanged from elalish/manifold v3.5.4.
# Tag commit: ce50d78021d64507f89e8c9fc2c2e51018117857
# Original archive SHA256: ce2f8b7798409b9f07e092c0ec0524ed0806431e01aea4020b65d0e934ea9438
# See devdocs/CurrentArchitecture/SolidGeometryFoundation1A.md for provenance.
set(GARGANTUAN_MANIFOLD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vendor/manifold")
set(GARGANTUAN_MANIFOLD_SOURCES
	boolean3.cpp boolean_result.cpp constructors.cpp csg_tree.cpp edge_op.cpp
	execution_impl.cpp face_op.cpp impl.cpp manifold.cpp minkowski.cpp polygon.cpp
	properties.cpp quickhull.cpp sdf.cpp smoothing.cpp sort.cpp subdivision.cpp tree2d.cpp
)
list(TRANSFORM GARGANTUAN_MANIFOLD_SOURCES PREPEND "${GARGANTUAN_MANIFOLD_ROOT}/src/")
add_library(GargantuanManifold STATIC ${GARGANTUAN_MANIFOLD_SOURCES})
target_include_directories(GargantuanManifold SYSTEM PUBLIC "${GARGANTUAN_MANIFOLD_ROOT}/include")
target_compile_features(GargantuanManifold PUBLIC cxx_std_17)
target_compile_definitions(GargantuanManifold PUBLIC MANIFOLD_PAR=-1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(GargantuanManifold PRIVATE -ffp-contract=off)
endif()
