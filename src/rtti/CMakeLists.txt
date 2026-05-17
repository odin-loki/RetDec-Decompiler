##
## src/rtti/CMakeLists.txt
##

add_library(retdec-rtti STATIC
    rtti.cpp
    itanium_rtti.cpp
    msvc_rtti.cpp
    borland_rtti.cpp
    dmc_rtti.cpp
    watcom_rtti.cpp
    symbian_rtti.cpp
)
add_library(retdec::rtti ALIAS retdec-rtti)

target_include_directories(retdec-rtti
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

set_target_properties(retdec-rtti PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
