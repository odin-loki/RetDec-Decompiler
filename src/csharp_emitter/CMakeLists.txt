add_library(retdec-csharp-emitter
    cs_writer.cpp
    cs_expr_emitter.cpp
    cs_stmt_emitter.cpp
    cs_type_emitter.cpp
    cs_file_emitter.cpp
)

target_include_directories(retdec-csharp-emitter
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-csharp-emitter
    PUBLIC
        retdec-bc-module
        retdec-cil-reconstruct
)

set_target_properties(retdec-csharp-emitter PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
