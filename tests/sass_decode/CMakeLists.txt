add_executable(retdec-sass-decode-tests
    sass_decode_test.cpp
)

target_include_directories(retdec-sass-decode-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-sass-decode-tests
    PRIVATE
        retdec-sass-decode
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-sass-decode-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-sass-decode-tests
    COMMAND retdec-sass-decode-tests
)
