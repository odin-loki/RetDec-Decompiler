
if(NOT TARGET retdec::opencl)
	find_package(OpenCL REQUIRED)
	include(${CMAKE_CURRENT_LIST_DIR}/retdec-opencl-targets.cmake)
endif()
