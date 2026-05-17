add_library(retdec-ipa
    call_graph.cpp
    function_summary.cpp
    ipa_propagation.cpp
    global_typing.cpp
    inline_candidate.cpp
    ipa_pass.cpp
)

target_include_directories(retdec-ipa PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(retdec-ipa PUBLIC
    retdec-ssa
    retdec-call-conv
)

set_target_properties(retdec-ipa PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-ipa
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
