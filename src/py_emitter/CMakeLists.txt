add_library(retdec-py-emitter
    py_writer.cpp
    py_expr_emitter.cpp
    py_stmt_emitter.cpp
    py_file_emitter.cpp
)

target_include_directories(retdec-py-emitter
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-py-emitter
    PUBLIC
        retdec-py-reconstruct
)

set_target_properties(retdec-py-emitter PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
