add_library(retdec-cfg-structure
    irreducibility.cpp
    post_domtree.cpp
    sese_decomp.cpp
    loop_recovery.cpp
    compiler_structurer.cpp
)

target_include_directories(retdec-cfg-structure PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-cfg-structure PUBLIC
    retdec-ssa
)

set_target_properties(retdec-cfg-structure PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_library(retdec_cfg_structure ALIAS retdec-cfg-structure)
