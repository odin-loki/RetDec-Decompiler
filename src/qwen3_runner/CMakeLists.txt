cmake_minimum_required(VERSION 3.15)

add_executable(retdec-qwen3-runner
    main.cpp
)

target_link_libraries(retdec-qwen3-runner
    PRIVATE
        retdec-qwen3
        retdec-cuda-accel
)

target_include_directories(retdec-qwen3-runner
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(retdec-qwen3-runner PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-qwen3-runner
    RUNTIME DESTINATION bin
)
