add_library(retdec-var-recovery STATIC
    prologue_parser.cpp
    abi_regions.cpp
    dvsa.cpp
    var_namer.cpp
)

target_include_directories(retdec-var-recovery
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-var-recovery
    PUBLIC
        retdec-ssa
)

target_compile_features(retdec-var-recovery PUBLIC cxx_std_17)

set_target_properties(retdec-var-recovery PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
