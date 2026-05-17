add_library(retdec-vbnet-emitter
    vb_writer.cpp
    vb_type_emitter.cpp
    vb_file_emitter.cpp
)

target_include_directories(retdec-vbnet-emitter
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-vbnet-emitter
    PUBLIC
        retdec-bc-module
)

set_target_properties(retdec-vbnet-emitter PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
