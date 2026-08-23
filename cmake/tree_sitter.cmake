# Fetch tree-sitter + tree-sitter-c from cmake/deps.cmake pins.
# Do not add files under deps/.

if(TARGET retdec-tree-sitter)
	return()
endif()

include(ExternalProject)

if(NOT RETDEC_EP_CMAKE_BUILD_TYPE)
	if(CMAKE_BUILD_TYPE)
		set(RETDEC_EP_CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
	else()
		set(RETDEC_EP_CMAKE_BUILD_TYPE "Release")
	endif()
endif()

set(_TS_CMAKE_ARGS
	-DCMAKE_BUILD_TYPE=${RETDEC_EP_CMAKE_BUILD_TYPE}
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DBUILD_SHARED_LIBS=OFF
)
if(CMAKE_C_COMPILER)
	list(APPEND _TS_CMAKE_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif()

set(TREE_SITTER_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps/install/tree-sitter)
set(TREE_SITTER_C_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps/install/tree-sitter-c)
file(MAKE_DIRECTORY ${TREE_SITTER_INSTALL_DIR}/include)
file(MAKE_DIRECTORY ${TREE_SITTER_INSTALL_DIR}/lib)
file(MAKE_DIRECTORY ${TREE_SITTER_C_INSTALL_DIR}/include)
file(MAKE_DIRECTORY ${TREE_SITTER_C_INSTALL_DIR}/lib)

set(_TS_LIB
	${TREE_SITTER_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}tree-sitter${CMAKE_STATIC_LIBRARY_SUFFIX})
set(_TSC_LIB
	${TREE_SITTER_C_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}tree-sitter-c${CMAKE_STATIC_LIBRARY_SUFFIX})

# Upstream install(TARGETS ... LIBRARY) skips static archives.
ExternalProject_Add(tree-sitter-project
	URL ${TREE_SITTER_URL}
	URL_HASH SHA256=${TREE_SITTER_ARCHIVE_SHA256}
	DOWNLOAD_NAME tree-sitter.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	CMAKE_ARGS
		${_TS_CMAKE_ARGS}
		-DCMAKE_INSTALL_PREFIX=${TREE_SITTER_INSTALL_DIR}
		-DAMALGAMATED=ON
		-DTREE_SITTER_FEATURE_WASM=OFF
	BUILD_BYPRODUCTS ${_TS_LIB}
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
	COMMAND ${CMAKE_COMMAND} -E make_directory ${TREE_SITTER_INSTALL_DIR}/lib
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		<BINARY_DIR>/${CMAKE_STATIC_LIBRARY_PREFIX}tree-sitter${CMAKE_STATIC_LIBRARY_SUFFIX}
		${TREE_SITTER_INSTALL_DIR}/lib/
)

# Overlay CMakeLists: compile shipped parser.c, no tree-sitter CLI.
ExternalProject_Add(tree-sitter-c-project
	URL ${TREE_SITTER_C_URL}
	URL_HASH SHA256=${TREE_SITTER_C_ARCHIVE_SHA256}
	DOWNLOAD_NAME tree-sitter-c.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	PATCH_COMMAND ${CMAKE_COMMAND} -E copy
		${PROJECT_SOURCE_DIR}/cmake/tree_sitter_c_CMakeLists.txt
		<SOURCE_DIR>/CMakeLists.txt
	CMAKE_ARGS
		${_TS_CMAKE_ARGS}
		-DCMAKE_INSTALL_PREFIX=${TREE_SITTER_C_INSTALL_DIR}
	BUILD_BYPRODUCTS ${_TSC_LIB}
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

add_library(retdec-tree-sitter STATIC IMPORTED GLOBAL)
add_dependencies(retdec-tree-sitter tree-sitter-project)
set_target_properties(retdec-tree-sitter PROPERTIES IMPORTED_LOCATION ${_TS_LIB})
target_include_directories(retdec-tree-sitter SYSTEM INTERFACE ${TREE_SITTER_INSTALL_DIR}/include)
add_library(retdec::deps::tree-sitter ALIAS retdec-tree-sitter)

add_library(retdec-tree-sitter-c STATIC IMPORTED GLOBAL)
add_dependencies(retdec-tree-sitter-c tree-sitter-c-project)
set_target_properties(retdec-tree-sitter-c PROPERTIES IMPORTED_LOCATION ${_TSC_LIB})
target_include_directories(retdec-tree-sitter-c SYSTEM INTERFACE ${TREE_SITTER_C_INSTALL_DIR}/include)
add_library(retdec::deps::tree-sitter-c ALIAS retdec-tree-sitter-c)
