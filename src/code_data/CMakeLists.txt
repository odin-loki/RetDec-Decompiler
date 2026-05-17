add_library(retdec-code-data
    code_data_classifier.cpp
)

add_library(retdec::code-data ALIAS retdec-code-data)

target_include_directories(retdec-code-data
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(retdec-code-data PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)
