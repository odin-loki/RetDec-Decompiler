add_library(retdec-dce
    abi_artifact_marker.cpp
    live_root_collector.cpp
    dead_propagation.cpp
    unreachable_elim.cpp
    dce_pass.cpp
)

target_include_directories(retdec-dce PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-dce PUBLIC
    retdec-ssa
    retdec-call-conv
)

set_target_properties(retdec-dce PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_library(retdec_dce ALIAS retdec-dce)
