
if(NOT TARGET retdec::packer)
    message(STATUS "retdec-packer-tests: retdec::packer not available — skipping")
    return()
endif()

add_executable(retdec-packer-tests
    packer_detector_test.cpp
)

target_compile_features(retdec-packer-tests PRIVATE cxx_std_17)

target_link_libraries(retdec-packer-tests
    PRIVATE
        retdec::packer
        retdec::deps::gtest_main
)

include(GoogleTest)
gtest_discover_tests(retdec-packer-tests)
