if(NOT TARGET retdec::retdec)
    message(STATUS "tests/retdec: skipped (retdec library not built)")
    return()
endif()

add_executable(retdec-retdec-tests
    thread_pool_test.cpp
    function_analysis_cache_test.cpp
)

target_link_libraries(retdec-retdec-tests
    PRIVATE
        retdec::retdec
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_compile_features(retdec-retdec-tests PRIVATE cxx_std_17)

add_test(NAME retdec-retdec-tests COMMAND retdec-retdec-tests)
