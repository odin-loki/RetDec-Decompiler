add_library(retdec-func-boundary
    func_boundary.cpp
)

add_library(retdec::func-boundary ALIAS retdec-func-boundary)

target_include_directories(retdec-func-boundary
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(retdec-func-boundary PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)
