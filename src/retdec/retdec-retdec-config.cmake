
if(NOT TARGET retdec::retdec)
    find_package(retdec @PROJECT_VERSION@
        REQUIRED
        COMPONENTS
            bin2llvmir
            llvmir2hll
            config
            common
            capstone
            llvm
    )

    # retdec-retdec-targets.cmake carries retdec-neural's PUBLIC link to
    # retdec-tree-sitter and retdec-tree-sitter-c. Those are IMPORTED targets
    # from cmake/tree_sitter.cmake, so install(EXPORT) could not export them --
    # it wrote the bare names into INTERFACE_LINK_LIBRARIES instead, and a
    # consumer with no targets by those names got them as library flags:
    # `cannot find -lretdec-tree-sitter`. Declaring them here, against the
    # archives cmake/tree_sitter.cmake now installs, is what makes the names
    # resolve. The prefix is computed by walking up from this file so the
    # install tree can still be moved.
    get_filename_component(_retdec_prefix
        "${CMAKE_CURRENT_LIST_DIR}/@_RETDEC_CMAKE_DIR_TO_PREFIX@" ABSOLUTE)
    foreach(_retdec_ts tree-sitter tree-sitter-c)
        if(NOT TARGET retdec-${_retdec_ts})
            add_library(retdec-${_retdec_ts} STATIC IMPORTED)
            set_target_properties(retdec-${_retdec_ts} PROPERTIES
                IMPORTED_LOCATION
                    "${_retdec_prefix}/@RETDEC_INSTALL_LIB_DIR@/@CMAKE_STATIC_LIBRARY_PREFIX@${_retdec_ts}@CMAKE_STATIC_LIBRARY_SUFFIX@")
        endif()
    endforeach()
    unset(_retdec_ts)

    # retdec-neural's Forge inference backend (src/neural/forge_inference.cpp)
    # is header-only Forge code plus two static archives, installed next to
    # the RetDec libraries: forge_simd_mv (CPU kernels) and forge_trust
    # (Monocypher). The export names forge_simd_mv; declare both here.
    if(NOT TARGET forge_trust)
        add_library(forge_trust STATIC IMPORTED)
        set_target_properties(forge_trust PROPERTIES
            IMPORTED_LOCATION
                "${_retdec_prefix}/@RETDEC_INSTALL_LIB_DIR@/@CMAKE_STATIC_LIBRARY_PREFIX@forge_trust@CMAKE_STATIC_LIBRARY_SUFFIX@")
    endif()
    if(NOT TARGET forge_simd_mv)
        find_package(Threads REQUIRED)
        add_library(forge_simd_mv STATIC IMPORTED)
        set(_retdec_forge_libs forge_trust Threads::Threads ${CMAKE_DL_LIBS})
        if(WIN32)
            list(APPEND _retdec_forge_libs ws2_32 bcrypt advapi32)
        endif()
        set_target_properties(forge_simd_mv PROPERTIES
            IMPORTED_LOCATION
                "${_retdec_prefix}/@RETDEC_INSTALL_LIB_DIR@/@CMAKE_STATIC_LIBRARY_PREFIX@forge_simd_mv@CMAKE_STATIC_LIBRARY_SUFFIX@"
            INTERFACE_LINK_LIBRARIES "${_retdec_forge_libs}")
        unset(_retdec_forge_libs)
    endif()
    unset(_retdec_prefix)

    include(${CMAKE_CURRENT_LIST_DIR}/retdec-retdec-targets.cmake)
endif()
