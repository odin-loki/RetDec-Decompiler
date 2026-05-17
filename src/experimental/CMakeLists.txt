
add_library(retdec-experimental STATIC
	pipeline_stub_anchors.cpp
)
add_library(retdec::experimental ALIAS retdec-experimental)

target_compile_features(retdec-experimental PUBLIC cxx_std_17)

target_include_directories(retdec-experimental
	PUBLIC
		$<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
		$<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

set_target_properties(retdec-experimental
	PROPERTIES
		OUTPUT_NAME "retdec-experimental"
)
