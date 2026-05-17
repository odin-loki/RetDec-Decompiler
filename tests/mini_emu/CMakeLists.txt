
if(NOT TARGET retdec::mini-emu)
    message(STATUS "retdec-mini-emu-tests: retdec::mini-emu not available — skipping")
    return()
endif()

add_executable(retdec-mini-emu-tests
    mini_emu_test.cpp
)

target_compile_features(retdec-mini-emu-tests PRIVATE cxx_std_17)

target_link_libraries(retdec-mini-emu-tests
    PRIVATE
        retdec::mini-emu
        retdec::deps::gtest_main
)

include(GoogleTest)
gtest_discover_tests(retdec-mini-emu-tests)
