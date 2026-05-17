add_library(retdec-cil-reconstruct
    cil_stack_sim.cpp
    cil_var_recovery.cpp
    cil_patterns.cpp
    cil_reconstructor.cpp
)

target_include_directories(retdec-cil-reconstruct
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-cil-reconstruct
    PUBLIC
        retdec-bc-module
)

set_target_properties(retdec-cil-reconstruct PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
