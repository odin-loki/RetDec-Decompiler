add_library(retdec-loader-sim
    loader_sim.cpp
)

add_library(retdec::loader-sim ALIAS retdec-loader-sim)

target_include_directories(retdec-loader-sim
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(retdec-loader-sim PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)
