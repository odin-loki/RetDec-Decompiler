add_library(retdec_codegen
    expr_coalesce.cpp
    cond_normalise.cpp
    loop_form.cpp
    pointer_syntax.cpp
    goto_elim.cpp
    emitter.cpp
)

target_include_directories(retdec_codegen
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec_codegen
    PUBLIC
        retdec_ssa
        retdec_cfg_structure
        retdec_call_conv
        retdec_dce
)

target_compile_features(retdec_codegen PUBLIC cxx_std_17)

add_library(retdec-codegen ALIAS retdec_codegen)
