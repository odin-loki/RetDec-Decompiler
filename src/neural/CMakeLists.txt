add_library(retdec-neural STATIC
    mock_inference.cpp
    gates.cpp
    refiner.cpp
)

add_library(retdec::neural ALIAS retdec-neural)

target_include_directories(retdec-neural
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-neural PUBLIC cxx_std_17)

set_target_properties(retdec-neural PROPERTIES OUTPUT_NAME "retdec-neural")

install(TARGETS retdec-neural
    EXPORT retdec-neural-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/retdec/neural/
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/neural
)
