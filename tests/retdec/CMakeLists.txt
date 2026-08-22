if(NOT TARGET retdec::retdec)
    message(STATUS "tests/retdec: skipped (retdec library not built)")
    return()
endif()

add_executable(retdec-retdec-tests
    thread_pool_test.cpp
    function_analysis_cache_test.cpp
    semantic_recovery_export_test.cpp
    llvm_to_ssa_test.cpp
)

target_include_directories(retdec-retdec-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/retdec
)

target_link_libraries(retdec-retdec-tests
    PRIVATE
        retdec::retdec
        retdec::deps::gtest
        retdec::deps::gtest_main
        retdec::deps::rapidjson
        retdec::deps::llvm
)

target_compile_features(retdec-retdec-tests PRIVATE cxx_std_17)

add_test(NAME retdec-retdec-tests COMMAND retdec-retdec-tests)
