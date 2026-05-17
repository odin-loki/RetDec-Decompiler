add_executable(retdec-serial-detect-tests
    serial_detect_test.cpp
)

target_include_directories(retdec-serial-detect-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-serial-detect-tests
    PRIVATE
        retdec-serial-detect
        retdec-ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-serial-detect-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME retdec-serial-detect-tests
    COMMAND retdec-serial-detect-tests
)
