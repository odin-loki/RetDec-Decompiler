add_library(retdec-bc-module
    bc_type.cpp
    bc_instr.cpp
    bc_cfg.cpp
    bc_module.cpp
    bc_json.cpp
)

target_include_directories(retdec-bc-module
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(retdec-bc-module PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

install(TARGETS retdec-bc-module ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
