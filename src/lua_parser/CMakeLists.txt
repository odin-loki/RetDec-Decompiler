##
## CMakeLists.txt -- retdec-lua-parser library
##

add_library(retdec-lua-parser
    lua_types.cpp
    lua_reader.cpp
    lua_emitter.cpp
)

target_include_directories(retdec-lua-parser
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

set_target_properties(retdec-lua-parser PROPERTIES
    OUTPUT_NAME "retdec-lua-parser"
)
