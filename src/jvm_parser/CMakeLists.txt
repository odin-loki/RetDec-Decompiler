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
    PRIVATE
        retdec::deps::zlib
)

# The ordering, stated rather than inherited. retdec::deps::zlib is an
# INTERFACE target whose link entry is a PATH into the bundled build, so the
# only thing that can make the ExternalProject run before this archive is a
# target-level dependency. It propagates from the interface target on Linux --
# measured, with a standalone reproduction -- and the first Windows run with
# the bundled zlib still linked before it existed:
#
#     LINK : fatal error LNK1104: cannot open file
#         'external\zlib-install\lib\zlibstatic.lib'
#
# with no sign of the zlib build having been scheduled at all. Saying it here
# costs one line and does not depend on how propagation behaves.
if(TARGET retdec-zlib)
    add_dependencies(retdec-jvm-parser retdec-zlib)
endif()

set_target_properties(retdec-jvm-parser PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

install(TARGETS retdec-jvm-parser ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
