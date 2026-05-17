@PACKAGE_INIT@

if(NOT TARGET llvm-libs)
	add_library(llvm-libs INTERFACE)
	add_library(retdec::deps::llvm-libs ALIAS llvm-libs)
	foreach(LLVM_LIB @PACKAGE_LLVM_LIBS_PATHS@)
		target_link_libraries(llvm-libs INTERFACE
			${LLVM_LIB}
		)
	endforeach(LLVM_LIB)
endif()

if(NOT TARGET retdec::deps::llvm)
	find_package(Threads REQUIRED)
	include(${CMAKE_CURRENT_LIST_DIR}/retdec-llvm-targets.cmake)
	# LLVM may have been built without zlib (e.g. Linux->MinGW cross: LLVM_ENABLE_ZLIB=OFF).
	# Link zlib only when found; if LLVM was built with zlib and this fails, the final link will error.
	if(UNIX OR MINGW)
		find_package(ZLIB QUIET)
		if(ZLIB_FOUND)
			target_link_libraries(retdec::deps::llvm INTERFACE ${ZLIB_LIBRARIES})
		endif()
	endif()
endif()
