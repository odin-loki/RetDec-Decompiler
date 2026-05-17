# Before project(): Visual Studio + CUDA .vcxproj targets need the generator toolset field
#   cuda=<toolkit root>
# so MSBuild CUDA *.targets get CudaToolkitDir (see CUDA X.Y.Version.props / CUDA_PATH_V13_2).
# try_compile-only env (cuda_msvc_trycompile.cmake) is not enough for the main build graph.
#
# Note: MSVC is not set until after project(); use WIN32 + Visual Studio generator.
if(NOT WIN32)
	return()
endif()
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
	return()
endif()
if(CMAKE_GENERATOR_TOOLSET MATCHES "cuda=")
	return()
endif()

set(_retdec_cuda_tk "")
if(CUDAToolkit_ROOT)
	set(_retdec_cuda_tk "${CUDAToolkit_ROOT}")
else()
	set(_retdec_cuda_pf "$ENV{ProgramFiles}")
	if(NOT _retdec_cuda_pf)
		set(_retdec_cuda_pf "C:/Program Files")
	endif()
	file(GLOB _retdec_cuda_bases "${_retdec_cuda_pf}/NVIDIA GPU Computing Toolkit/CUDA/*")
	if(_retdec_cuda_bases)
		list(SORT _retdec_cuda_bases COMPARE NATURAL ORDER DESCENDING)
		foreach(_cand IN LISTS _retdec_cuda_bases)
			if(EXISTS "${_cand}/bin/nvcc.exe")
				set(_retdec_cuda_tk "${_cand}")
				break()
			endif()
		endforeach()
	endif()
endif()

if(_retdec_cuda_tk AND EXISTS "${_retdec_cuda_tk}/bin/nvcc.exe")
	file(TO_CMAKE_PATH "${_retdec_cuda_tk}" _retdec_cuda_tk)
	# host=x64 avoids x86 host cl for x64 targets (MSVC warning during CUDA compile).
	set(CMAKE_GENERATOR_TOOLSET "host=x64,cuda=${_retdec_cuda_tk}" CACHE STRING
		"Generator toolset (CUDA path for MSBuild)" FORCE)
	message(STATUS "MSVC+CUDA: CMAKE_GENERATOR_TOOLSET -> ${CMAKE_GENERATOR_TOOLSET}")
endif()
unset(_retdec_cuda_tk)
