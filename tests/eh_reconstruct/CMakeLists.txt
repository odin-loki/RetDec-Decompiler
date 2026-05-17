add_executable(retdec-eh-reconstruct-tests
    eh_test.cpp
)

target_link_libraries(retdec-eh-reconstruct-tests
    PRIVATE
        retdec-eh-reconstruct
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-eh-reconstruct-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-eh-reconstruct-tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(retdec-eh-reconstruct-tests)
