
if(NOT TARGET retdec::fileformat-lattice)
    message(STATUS "retdec-packer: retdec::fileformat-lattice not available — skipping")
    return()
endif()

add_library(retdec-packer STATIC
    entropy_profiler.cpp
    packer_detector.cpp
)
add_library(retdec::packer ALIAS retdec-packer)

target_compile_features(retdec-packer PUBLIC cxx_std_17)

target_include_directories(retdec-packer
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(retdec-packer
    PUBLIC
        retdec::fileformat-lattice
)

set_target_properties(retdec-packer
    PROPERTIES
        OUTPUT_NAME "retdec-packer"
)

install(
    DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/packer
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec
)

install(TARGETS retdec-packer
    EXPORT retdec-packer-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)
