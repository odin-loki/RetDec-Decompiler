add_library(retdec-py-reconstruct
    py_stack_sim.cpp
    py_cfg_builder.cpp
)

target_include_directories(retdec-py-reconstruct
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-py-reconstruct
    PUBLIC
        retdec-pyc-parser
        retdec-bc-module
)

set_target_properties(retdec-py-reconstruct PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
