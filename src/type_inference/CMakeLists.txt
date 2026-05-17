add_library(retdec-type-inference STATIC
    width_seeder.cpp
    type_propagation.cpp
    abi_seeder.cpp
)

target_include_directories(retdec-type-inference
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(retdec-type-inference
    PUBLIC
        retdec-ssa
)

target_compile_features(retdec-type-inference PUBLIC cxx_std_17)

set_target_properties(retdec-type-inference PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

install(TARGETS retdec-type-inference
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
