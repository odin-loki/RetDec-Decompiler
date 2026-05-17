add_library(retdec_pattern_detect
    singleton_detect.cpp
    factory_detect.cpp
    observer_detect.cpp
    command_detect.cpp
    strategy_detect.cpp
    state_detect.cpp
    raii_detect.cpp
    pattern_detector.cpp
)

target_include_directories(retdec_pattern_detect
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec_pattern_detect
    PUBLIC
        retdec_ssa
)

set_target_properties(retdec_pattern_detect PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
