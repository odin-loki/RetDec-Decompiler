# retdec-llamacpp-config.cmake — imported target retdec::deps::llamacpp (llama static lib)

if(NOT TARGET retdec::deps::llamacpp)
	message(FATAL_ERROR "retdec::deps::llamacpp not built — enable RETDEC_ENABLE_NEURAL and LLAMACPP")
endif()
