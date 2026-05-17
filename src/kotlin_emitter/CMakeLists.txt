add_library(retdec-kotlin-emitter
    kotlin_metadata.cpp
    kotlin_type_system.cpp
    kotlin_emitter.cpp
    kotlin_file_emitter.cpp
)

target_include_directories(retdec-kotlin-emitter
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-kotlin-emitter
    PUBLIC
        retdec-bc-module
        retdec-jvm-reconstruct
        retdec-java-emitter
)

set_target_properties(retdec-kotlin-emitter PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
