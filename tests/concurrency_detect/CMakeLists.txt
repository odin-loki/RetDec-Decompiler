add_executable(retdec-concurrency-detect-tests
    concurrency_detect_test.cpp
)

target_include_directories(retdec-concurrency-detect-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-concurrency-detect-tests
    PRIVATE
        retdec-concurrency-detect
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-concurrency-detect-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-concurrency-detect-tests
    COMMAND retdec-concurrency-detect-tests
)
