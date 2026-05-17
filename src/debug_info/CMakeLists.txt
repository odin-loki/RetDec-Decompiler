##
## src/debug_info/CMakeLists.txt
##
## Builds the retdec-debug-info static library.
## Optionally links libdw (elfutils) when RETDEC_USE_LIBDW is ON.
##

add_library(retdec-debug-info STATIC
    debug_info.cpp
    dwarf_extractor.cpp
    pdb_extractor.cpp
)
add_library(retdec::debug-info ALIAS retdec-debug-info)

target_include_directories(retdec-debug-info
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

## Optional libdw (elfutils) support
if(RETDEC_USE_LIBDW)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDW REQUIRED libdw)
    target_compile_definitions(retdec-debug-info PUBLIC RETDEC_USE_LIBDW)
    target_include_directories(retdec-debug-info PRIVATE ${LIBDW_INCLUDE_DIRS})
    target_link_libraries(retdec-debug-info PRIVATE ${LIBDW_LIBRARIES})
endif()

## Optional LLVM PDB support
if(RETDEC_USE_LLVM_PDB)
    target_compile_definitions(retdec-debug-info PUBLIC RETDEC_USE_LLVM_PDB)
    # Caller is responsible for providing LLVM::DebugInfoPDB target.
    target_link_libraries(retdec-debug-info PRIVATE LLVM::DebugInfoPDB)
endif()

## C++17
set_target_properties(retdec-debug-info PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
