add_library(retdec-type-seed STATIC
    type_seed.cpp
    itanium_seeder.cpp
    msvc_seeder.cpp
    rust_seeder.cpp
    swift_seeder.cpp
)

target_include_directories(retdec-type-seed
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

# Optional: link rustc-demangle C library if found
find_library(RUSTC_DEMANGLE_LIB NAMES rustc_demangle)
if(RUSTC_DEMANGLE_LIB)
    target_compile_definitions(retdec-type-seed PRIVATE RETDEC_USE_RUSTC_DEMANGLE)
    target_link_libraries(retdec-type-seed PRIVATE ${RUSTC_DEMANGLE_LIB})
    message(STATUS "retdec-type-seed: rustc-demangle found: ${RUSTC_DEMANGLE_LIB}")
else()
    message(STATUS "retdec-type-seed: rustc-demangle not found, using built-in Rust parser")
endif()

# Optional: link swift-demangle if found
find_library(SWIFT_DEMANGLE_LIB NAMES swiftDemangle)
if(SWIFT_DEMANGLE_LIB)
    target_compile_definitions(retdec-type-seed PRIVATE RETDEC_USE_SWIFT_DEMANGLE)
    target_link_libraries(retdec-type-seed PRIVATE ${SWIFT_DEMANGLE_LIB})
    message(STATUS "retdec-type-seed: swift-demangle found: ${SWIFT_DEMANGLE_LIB}")
else()
    message(STATUS "retdec-type-seed: swift-demangle not found, using built-in Swift parser")
endif()

# Optional: link libstdc++/libc++ for __cxa_demangle (Itanium)
if(NOT MSVC)
    target_compile_definitions(retdec-type-seed PRIVATE RETDEC_USE_CXXABI_DEMANGLE)
    # __cxa_demangle is part of libstdc++/libc++ already linked by default
endif()

set_target_properties(retdec-type-seed PROPERTIES
    OUTPUT_NAME "retdec-type-seed"
)

install(TARGETS retdec-type-seed
    EXPORT retdec-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    ${RETDEC_INCLUDE_DIR}/retdec/type_seed/type_seed.h
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/type_seed
)
