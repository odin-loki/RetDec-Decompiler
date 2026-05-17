##
## CMakeLists.txt -- retdec-cxx-backend-tests
##

add_executable(retdec-cxx-backend-tests
    cxx_backend_test.cpp
)

target_link_libraries(retdec-cxx-backend-tests
    PRIVATE
        retdec-cxx-backend
        gtest_main
)

add_test(NAME retdec-cxx-backend-tests
    COMMAND retdec-cxx-backend-tests
)
