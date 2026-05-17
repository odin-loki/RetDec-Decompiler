add_library(retdec-jvm-parser
    jvm_const_pool.cpp
    jvm_signature.cpp
    jvm_attr.cpp
    jvm_lifter.cpp
    jvm_class_parser.cpp
    jvm_jar_reader.cpp
)

target_include_directories(retdec-jvm-parser
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(retdec-jvm-parser
    PUBLIC
        retdec-bc-module
)

set_target_properties(retdec-jvm-parser PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

install(TARGETS retdec-jvm-parser ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
