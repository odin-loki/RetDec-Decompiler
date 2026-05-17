add_library(retdec-call-conv
    caller_cleanup.cpp
    reg_arg_analysis.cpp
    return_value.cpp
    variadic_detect.cpp
    call_conv_pass.cpp
)

target_include_directories(retdec-call-conv PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(retdec-call-conv PUBLIC
    retdec-ssa
)

set_target_properties(retdec-call-conv PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_library(retdec_call_conv ALIAS retdec-call-conv)

install(TARGETS retdec-call-conv
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
