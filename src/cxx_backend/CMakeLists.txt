##
## CMakeLists.txt -- retdec-cxx-backend library
##

add_library(retdec-cxx-backend
    cxx_ast.cpp
    cxx_emitter.cpp
    cxx_lifter.cpp
)

target_include_directories(retdec-cxx-backend
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

target_link_libraries(retdec-cxx-backend
    PUBLIC
        retdec-codegen
)

set_target_properties(retdec-cxx-backend PROPERTIES
    OUTPUT_NAME "retdec-cxx-backend"
)
