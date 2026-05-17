add_library(retdec-ssa STATIC
    flag_bundle.cpp
    liveness.cpp
    domtree.cpp
    phi_placement.cpp
    ssa_rename.cpp
)

target_include_directories(retdec-ssa
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(retdec-ssa PUBLIC cxx_std_17)

set_target_properties(retdec-ssa PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_library(retdec_ssa ALIAS retdec-ssa)

install(TARGETS retdec-ssa
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
