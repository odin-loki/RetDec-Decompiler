add_library(retdec-cfg
    cfg.cpp
)

add_library(retdec::cfg ALIAS retdec-cfg)

target_include_directories(retdec-cfg
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(retdec-cfg PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)
