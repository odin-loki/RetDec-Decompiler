if(NOT TARGET retdec::retdec)
    message(STATUS "tests/retdec: skipped (retdec library not built)")
    return()
endif()

add_executable(retdec-retdec-tests
    thread_pool_test.cpp
    function_analysis_cache_test.cpp
    semantic_recovery_export_test.cpp
    llvm_to_ssa_test.cpp
    managed_decompiler_test.cpp
    # The unit under test. managed_decompiler.cpp is compiled into the
    # retdec-decompiler executable rather than into a library, so there is no
    # target to link against and the translation unit is compiled here too. It
    # reaches std::span through cli_heaps.h, which is why the C++20 property
    # below applies to this target as a whole.
    ${PROJECT_SOURCE_DIR}/src/retdec-decompiler/managed_decompiler.cpp
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
        # What managed_decompiler.cpp dispatches to; the same list
        # src/retdec-decompiler/CMakeLists.txt links for the same file.
        retdec-jvm-parser
        retdec-jvm-reconstruct
        retdec-java-emitter
        retdec-cli-parser
        retdec-csharp-emitter
        retdec-dex-parser
        retdec-pyc-parser
        retdec-py-reconstruct
        retdec-py-emitter
        retdec-lua-parser
        retdec-wasm-parser
)

target_compile_features(retdec-retdec-tests PRIVATE cxx_std_20)

add_test(NAME retdec-retdec-tests COMMAND retdec-retdec-tests)
