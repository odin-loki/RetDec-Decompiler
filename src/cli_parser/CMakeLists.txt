add_library(retdec-cli-parser
    pe_reader.cpp
    cli_heaps.cpp
    cli_tables.cpp
    cli_sig.cpp
    cil_lifter.cpp
    cli_reader.cpp
)

target_include_directories(retdec-cli-parser
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-cli-parser
    PUBLIC
        retdec-bc-module
)

set_target_properties(retdec-cli-parser PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
