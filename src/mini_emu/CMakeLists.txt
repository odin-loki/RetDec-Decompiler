
if(NOT TARGET retdec::fileformat-lattice)
    message(STATUS "retdec-mini-emu: retdec::fileformat-lattice not available — skipping")
    return()
endif()

add_library(retdec-mini-emu STATIC
    mini_emu.cpp
    mini_unpacker.cpp
)
add_library(retdec::mini-emu ALIAS retdec-mini-emu)

target_compile_features(retdec-mini-emu PUBLIC cxx_std_17)

target_include_directories(retdec-mini-emu
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(retdec-mini-emu
    PUBLIC
        retdec::fileformat-lattice
)

set_target_properties(retdec-mini-emu
    PROPERTIES
        OUTPUT_NAME "retdec-mini-emu"
)

install(
    DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/mini_emu
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec
)

install(TARGETS retdec-mini-emu
    EXPORT retdec-mini-emu-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)
