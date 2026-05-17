# WindowsOpenCLImportLib.cmake
#
# For MinGW-w64 cross-compilation: generate a minimal OpenCL import library
# (libOpenCL.a / OpenCL.lib) that lets binaries link against OpenCL without
# a real SDK installed on the build host.
#
# At runtime the application will load the OpenCL ICD loader (OpenCL.dll)
# from the driver or the Khronos ICD loader, whichever is installed.
#
# Usage in a CMakeLists.txt:
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")
#   include(WindowsOpenCLImportLib)
#   retdec_get_opencl_import_lib(OpenCL_IMPORT_LIB)
#   target_link_libraries(my_target PRIVATE "${OpenCL_IMPORT_LIB}")
#
# The generated .a lives in ${CMAKE_BINARY_DIR}/opencl-import/.

cmake_minimum_required(VERSION 3.20)

function(retdec_get_opencl_import_lib OUT_VAR)
    # Only needed for MinGW / Windows cross-compilation.
    if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # On native Linux/macOS find_package(OpenCL) handles this.
        find_package(OpenCL QUIET)
        if(OpenCL_FOUND)
            set(${OUT_VAR} "OpenCL::OpenCL" PARENT_SCOPE)
        else()
            set(${OUT_VAR} "" PARENT_SCOPE)
        endif()
        return()
    endif()

    set(_OCL_LIB_DIR "${CMAKE_BINARY_DIR}/opencl-import")
    set(_OCL_DEF     "${_OCL_LIB_DIR}/OpenCL.def")
    set(_OCL_LIB     "${_OCL_LIB_DIR}/libOpenCL.a")

    if(EXISTS "${_OCL_LIB}")
        set(${OUT_VAR} "${_OCL_LIB}" PARENT_SCOPE)
        return()
    endif()

    file(MAKE_DIRECTORY "${_OCL_LIB_DIR}")

    # ── Minimal OpenCL 3.0 export definition ──────────────────────────────────
    # Covers the symbols used by RetDec's OpenCL layer.  A real OpenCL.dll
    # from any ICD loader exports a superset of these.
    file(WRITE "${_OCL_DEF}" "\
LIBRARY OpenCL.dll\n\
EXPORTS\n\
  clGetPlatformIDs\n\
  clGetPlatformInfo\n\
  clGetDeviceIDs\n\
  clGetDeviceInfo\n\
  clCreateContext\n\
  clCreateContextFromType\n\
  clRetainContext\n\
  clReleaseContext\n\
  clGetContextInfo\n\
  clCreateCommandQueueWithProperties\n\
  clCreateCommandQueue\n\
  clRetainCommandQueue\n\
  clReleaseCommandQueue\n\
  clGetCommandQueueInfo\n\
  clCreateBuffer\n\
  clCreateSubBuffer\n\
  clCreateImage\n\
  clRetainMemObject\n\
  clReleaseMemObject\n\
  clGetSupportedImageFormats\n\
  clGetMemObjectInfo\n\
  clGetImageInfo\n\
  clEnqueueReadBuffer\n\
  clEnqueueWriteBuffer\n\
  clEnqueueCopyBuffer\n\
  clEnqueueReadImage\n\
  clEnqueueWriteImage\n\
  clEnqueueCopyImage\n\
  clEnqueueFillBuffer\n\
  clEnqueueMapBuffer\n\
  clEnqueueMapImage\n\
  clEnqueueUnmapMemObject\n\
  clEnqueueNDRangeKernel\n\
  clEnqueueTask\n\
  clEnqueueNativeKernel\n\
  clCreateProgramWithSource\n\
  clCreateProgramWithBinary\n\
  clCreateProgramWithBuiltInKernels\n\
  clCreateProgramWithIL\n\
  clRetainProgram\n\
  clReleaseProgram\n\
  clBuildProgram\n\
  clCompileProgram\n\
  clLinkProgram\n\
  clGetProgramInfo\n\
  clGetProgramBuildInfo\n\
  clCreateKernel\n\
  clCreateKernelsInProgram\n\
  clRetainKernel\n\
  clReleaseKernel\n\
  clSetKernelArg\n\
  clGetKernelInfo\n\
  clGetKernelWorkGroupInfo\n\
  clWaitForEvents\n\
  clGetEventInfo\n\
  clCreateUserEvent\n\
  clRetainEvent\n\
  clReleaseEvent\n\
  clSetUserEventStatus\n\
  clSetEventCallback\n\
  clGetEventProfilingInfo\n\
  clFlush\n\
  clFinish\n\
  clGetExtensionFunctionAddressForPlatform\n\
")

    # Find the MinGW dlltool
    find_program(_DLLTOOL
        NAMES x86_64-w64-mingw32-dlltool dlltool
        DOC "dlltool for generating import libraries")

    if(NOT _DLLTOOL)
        message(WARNING
            "WindowsOpenCLImportLib: dlltool not found. "
            "Cannot generate libOpenCL.a. Link without OpenCL or install "
            "binutils-mingw-w64-x86-64.")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    # Generate the import library
    execute_process(
        COMMAND "${_DLLTOOL}"
                --kill-at
                --input-def "${_OCL_DEF}"
                --dllname   OpenCL.dll
                --output-lib "${_OCL_LIB}"
        RESULT_VARIABLE _dlltool_rc
        OUTPUT_VARIABLE _dlltool_out
        ERROR_VARIABLE  _dlltool_err
    )

    if(_dlltool_rc)
        message(WARNING
            "WindowsOpenCLImportLib: dlltool failed (rc=${_dlltool_rc}):\n"
            "${_dlltool_err}")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    message(STATUS "Generated OpenCL import lib: ${_OCL_LIB}")
    set(${OUT_VAR} "${_OCL_LIB}" PARENT_SCOPE)
endfunction()
