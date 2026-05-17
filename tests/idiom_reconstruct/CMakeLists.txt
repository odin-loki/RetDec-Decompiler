add_executable(retdec-idiom-reconstruct-tests
    idiom_test.cpp
)

target_link_libraries(retdec-idiom-reconstruct-tests
    PRIVATE
        retdec-idiom-reconstruct
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-idiom-reconstruct-tests
    PRIVATE
        ${RETDEC_INCLUDE_DIR}
)

gtest_discover_tests(retdec-idiom-reconstruct-tests)
