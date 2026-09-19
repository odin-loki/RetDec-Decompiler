##
## CMakeLists.txt -- retdec-sass-decode
## NVIDIA SASS cubin/fatbin loader + documented SM_70/SM_80 subset.
## Not a Capstone architecture. Not linked from decompile() until wired.
##

add_library(retdec-sass-decode
    sass_types.cpp
    cubin_loader.cpp
    sass_decoder.cpp
    sass_lifter.cpp
)

target_include_directories(retdec-sass-decode
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

target_link_libraries(retdec-sass-decode
    PUBLIC
        retdec-ptx-decompile
)

set_target_properties(retdec-sass-decode PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "retdec-sass-decode"
)

install(TARGETS retdec-sass-decode
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
