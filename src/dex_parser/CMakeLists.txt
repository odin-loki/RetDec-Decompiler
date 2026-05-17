add_library(retdec-dex-parser
    dex_header.cpp
    dex_lifter.cpp
    dex_class_parser.cpp
    dex_apk_reader.cpp
)

target_include_directories(retdec-dex-parser
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
)

target_link_libraries(retdec-dex-parser
    PUBLIC retdec-bc-module
)

set_target_properties(retdec-dex-parser PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-dex-parser
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/retdec/dex_parser
    DESTINATION include/retdec
)
