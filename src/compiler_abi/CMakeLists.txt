##
## src/compiler_abi/CMakeLists.txt
##

add_library(retdec-compiler-abi STATIC
    abi_descriptor.cpp
)
add_library(retdec::compiler-abi ALIAS retdec-compiler-abi)

target_include_directories(retdec-compiler-abi
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

set_target_properties(retdec-compiler-abi PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
