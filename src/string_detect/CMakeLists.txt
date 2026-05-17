add_library(retdec-string-detect STATIC
    string_typer.cpp
    ref_anchored.cpp
    sso_detect.cpp
    string_table.cpp
    arm_literal_pool.cpp
)

target_include_directories(retdec-string-detect
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

set_target_properties(retdec-string-detect PROPERTIES
    OUTPUT_NAME "retdec-string-detect"
)

install(TARGETS retdec-string-detect
    EXPORT retdec-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    ${RETDEC_INCLUDE_DIR}/retdec/string_detect/string_detect.h
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/string_detect
)
