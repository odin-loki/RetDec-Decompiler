add_library(retdec-fsharp-emitter
    fs_writer.cpp
    fs_type_emitter.cpp
    fs_file_emitter.cpp
)

target_include_directories(retdec-fsharp-emitter
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-fsharp-emitter
    PUBLIC
        retdec-bc-module
)

set_target_properties(retdec-fsharp-emitter PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
