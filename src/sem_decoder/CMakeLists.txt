if(NOT TARGET retdec::deps::capstone)
    message(STATUS "retdec-sem-decoder: retdec::deps::capstone not available — skipping")
    return()
endif()

add_library(retdec-sem-decoder
    sem_decoder.cpp
)

add_library(retdec::sem-decoder ALIAS retdec-sem-decoder)

target_include_directories(retdec-sem-decoder
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(retdec-sem-decoder
    PUBLIC
        retdec::deps::capstone
)

set_target_properties(retdec-sem-decoder PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)
