if(NOT TARGET retdec::sem-decoder)
    message(STATUS "retdec-sem-decoder-tests: retdec::sem-decoder not available — skipping")
    return()
endif()

add_executable(retdec-sem-decoder-tests
    sem_decoder_test.cpp
)

target_link_libraries(retdec-sem-decoder-tests
    PRIVATE
        retdec::sem-decoder
        retdec::deps::gtest_main
)

set_target_properties(retdec-sem-decoder-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-sem-decoder-tests)
