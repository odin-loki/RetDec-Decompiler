add_library(retdec-profiling
    profiling.cpp
)

target_include_directories(retdec-profiling
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(retdec-profiling PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(WIN32)
    target_link_libraries(retdec-profiling PRIVATE psapi)
endif()

install(TARGETS retdec-profiling
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
