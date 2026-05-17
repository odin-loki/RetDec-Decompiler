add_library(retdec-alias-analysis STATIC
    stack_alias.cpp
    steensgaard.cpp
    escape_analysis.cpp
)

target_include_directories(retdec-alias-analysis
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-alias-analysis
    PUBLIC
        retdec-ssa
)

target_compile_features(retdec-alias-analysis PUBLIC cxx_std_17)

set_target_properties(retdec-alias-analysis PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
