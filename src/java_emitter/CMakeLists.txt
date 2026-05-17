add_library(retdec-java-emitter
    java_type_printer.cpp
    java_expr_emitter.cpp
    java_stmt_emitter.cpp
    java_class_emitter.cpp
    java_file_emitter.cpp
)

target_include_directories(retdec-java-emitter
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
)

target_link_libraries(retdec-java-emitter
    PUBLIC retdec-bc-module
           retdec-jvm-reconstruct
)

set_target_properties(retdec-java-emitter PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-java-emitter
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/retdec/java_emitter
    DESTINATION include/retdec
)
