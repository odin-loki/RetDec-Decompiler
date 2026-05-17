add_executable(retdec-crypto-detect-tests
    crypto_detect_test.cpp
)

target_link_libraries(retdec-crypto-detect-tests
    PRIVATE
        retdec-crypto-detect
        retdec-ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-crypto-detect-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

add_test(
    NAME retdec-crypto-detect-tests
    COMMAND retdec-crypto-detect-tests
)
