
add_library(retdec-fileformat-lattice STATIC
    format_lattice.cpp
)
add_library(retdec::fileformat-lattice ALIAS retdec-fileformat-lattice)

target_compile_features(retdec-fileformat-lattice PUBLIC cxx_std_17)

target_include_directories(retdec-fileformat-lattice
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(retdec-fileformat-lattice
    PROPERTIES
        OUTPUT_NAME "retdec-fileformat-lattice"
)

install(
    DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/fileformat/lattice
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/fileformat
)

install(TARGETS retdec-fileformat-lattice
    EXPORT retdec-fileformat-lattice-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)
