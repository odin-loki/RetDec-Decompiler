add_library(retdec-compiler-detect
    compiler_fingerprinter.cpp
)

add_library(retdec::compiler-detect ALIAS retdec-compiler-detect)

target_include_directories(retdec-compiler-detect
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(retdec-compiler-detect PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)
