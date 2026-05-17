##
## CMakeLists.txt -- retdec-wasm-parser library
##

add_library(retdec-wasm-parser
    wasm_types.cpp
    wasm_reader.cpp
    wat_emitter.cpp
)

target_include_directories(retdec-wasm-parser
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

set_target_properties(retdec-wasm-parser PROPERTIES
    OUTPUT_NAME "retdec-wasm-parser"
)
