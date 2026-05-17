
add_executable(retdec-fileformat-tests
    format_lattice_test.cpp
)

target_compile_features(retdec-fileformat-tests PRIVATE cxx_std_17)

target_link_libraries(retdec-fileformat-tests
    PRIVATE
        retdec::fileformat-lattice
        retdec::deps::gtest_main
)

include(GoogleTest)
gtest_discover_tests(retdec-fileformat-tests)
