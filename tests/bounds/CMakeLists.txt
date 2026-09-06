add_executable(retdec-bounds-tests
    bounds_test.cpp
)

target_link_libraries(retdec-bounds-tests
    PRIVATE
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-bounds-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-bounds-tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(retdec-bounds-tests)
