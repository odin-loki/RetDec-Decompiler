add_library(retdec-eh-reconstruct STATIC
    eh_reconstruct.cpp
    lsda_parser.cpp
    msvc_eh.cpp
    itanium_eh.cpp
    arm_ehabi.cpp
    borland_eh.cpp
    dmc_eh.cpp
    watcom_eh.cpp
    symbian_eh.cpp
)

target_include_directories(retdec-eh-reconstruct
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-eh-reconstruct PUBLIC cxx_std_17)

set_target_properties(retdec-eh-reconstruct PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
