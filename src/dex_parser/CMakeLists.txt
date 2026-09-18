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
    PRIVATE retdec::deps::zlib
)

# The ordering, stated rather than inherited. retdec::deps::zlib is an
# INTERFACE target whose link entry is a PATH into the bundled build, so the
# only thing that can order the ExternalProject before this archive is a
# target-level dependency. It does propagate from the interface target --
# measured on Linux with a standalone reproduction, and the Windows log shows
# the zlib build finishing four minutes before the link that wanted it -- so
# this line is belt and braces rather than the fix for anything. It costs one
# line and does not depend on how propagation behaves.
if(TARGET retdec-zlib)
    add_dependencies(retdec-dex-parser retdec-zlib)
endif()

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
