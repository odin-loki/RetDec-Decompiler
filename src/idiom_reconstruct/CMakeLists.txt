add_library(retdec-idiom-reconstruct STATIC
    idiom_reconstruct.cpp
    division_recovery.cpp
    modulo_recovery.cpp
    abs_recovery.cpp
    bit_idioms.cpp
    simd_memcpy.cpp
)

target_include_directories(retdec-idiom-reconstruct
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

# __uint128_t / __int128_t are used for the magic-constant verification.
# MSVC does not support these; provide an alternative for MSVC builds.
if(MSVC)
    target_compile_definitions(retdec-idiom-reconstruct PRIVATE RETDEC_NO_INT128)
endif()

set_target_properties(retdec-idiom-reconstruct PROPERTIES
    OUTPUT_NAME "retdec-idiom-reconstruct"
)

install(TARGETS retdec-idiom-reconstruct
    EXPORT retdec-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    ${RETDEC_INCLUDE_DIR}/retdec/idiom_reconstruct/idiom_reconstruct.h
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/idiom_reconstruct
)
