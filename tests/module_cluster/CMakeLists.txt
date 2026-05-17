add_executable(retdec-module-cluster-tests
    module_cluster_test.cpp
)

target_link_libraries(retdec-module-cluster-tests
    PRIVATE
        retdec-module-cluster
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-module-cluster-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-module-cluster-tests
    COMMAND retdec-module-cluster-tests
)
