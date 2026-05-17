add_library(retdec-module-cluster
    module_cluster.cpp
)

target_include_directories(retdec-module-cluster
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(retdec-module-cluster PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-module-cluster
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
