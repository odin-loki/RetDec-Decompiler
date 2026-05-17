add_library(retdec-testing
    test_harness.cpp
)

target_include_directories(retdec-testing
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(retdec-testing PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-testing
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
