add_executable(retdec-type-seed-tests
    type_seed_test.cpp
)

target_link_libraries(retdec-type-seed-tests
    PRIVATE
        retdec-type-seed
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-type-seed-tests
    PRIVATE
        ${RETDEC_INCLUDE_DIR}
)

gtest_discover_tests(retdec-type-seed-tests)
