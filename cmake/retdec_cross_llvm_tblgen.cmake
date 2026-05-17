# Host llvm-tblgen for LLVM ExternalProject when CMAKE_CROSSCOMPILING (e.g. Linux -> MinGW).
# Included from deps/llvm/CMakeLists.txt after RETDEC_LLVM_TABLEGEN cache is declared.
#
# Priority:
#   1) CMake cache RETDEC_LLVM_TABLEGEN (user -D or cmake-gui)
#   2) Environment variable RETDEC_LLVM_TABLEGEN
#   3) CMake cache RETDEC_LLVM_TABLEGEN_EXTRA_CANDIDATES (semicolon-separated paths; first existing wins)
#   4) Well-known paths from a normal in-repo Linux ELF build (build-wsl / build / build-linux, …)
#
# The bundled LLVM revision must match the host tblgen; use a build directory produced from this
# tree (e.g. cmake .. without a Windows toolchain, then build llvm-project) or pass -D explicitly.

if(NOT CMAKE_CROSSCOMPILING)
	return()
endif()

set(RETDEC_LLVM_TABLEGEN_EXTRA_CANDIDATES "" CACHE STRING
	"Semicolon-separated absolute paths to host llvm-tblgen to try when cross-compiling (after env, before built-in build-wsl/build/... search). Must match RetDec's pinned LLVM revision.")

if(RETDEC_LLVM_TABLEGEN)
	return()
endif()

if(NOT "$ENV{RETDEC_LLVM_TABLEGEN}" STREQUAL "")
	set(RETDEC_LLVM_TABLEGEN "$ENV{RETDEC_LLVM_TABLEGEN}" CACHE FILEPATH
		"Path to host llvm-tblgen for cross builds (from env RETDEC_LLVM_TABLEGEN)" FORCE)
	message(STATUS "LLVM cross: RETDEC_LLVM_TABLEGEN taken from environment: ${RETDEC_LLVM_TABLEGEN}")
	return()
endif()

foreach(_tbl IN LISTS RETDEC_LLVM_TABLEGEN_EXTRA_CANDIDATES)
	if(_tbl AND EXISTS "${_tbl}")
		set(RETDEC_LLVM_TABLEGEN "${_tbl}" CACHE FILEPATH
			"Path to host llvm-tblgen for cross builds (from RETDEC_LLVM_TABLEGEN_EXTRA_CANDIDATES)" FORCE)
		message(STATUS "LLVM cross: RETDEC_LLVM_TABLEGEN from EXTRA_CANDIDATES: ${RETDEC_LLVM_TABLEGEN}")
		return()
	endif()
endforeach()

# Typical layout: ${REPO}/<host-build>/external/src/llvm-project-build/bin/llvm-tblgen
# Also: sibling of repo parent, e.g. .../workspace/build-wsl when repo is .../workspace/retdec-master
get_filename_component(_retdec_source_parent "${CMAKE_SOURCE_DIR}" DIRECTORY)
set(_retdec_tblgen_candidates
	"${CMAKE_SOURCE_DIR}/build-wsl/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-wsl/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-wsl/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-linux/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-linux/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-linux/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-ninja/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-ninja/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-ninja/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-debug/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-debug/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-debug/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-rel/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-rel/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-rel/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-host/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-host/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-host/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-elf/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-elf/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-elf/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/build-ci-mingw/external/src/llvm-project-build/bin/llvm-tblgen"
	"${CMAKE_SOURCE_DIR}/../build-ci-mingw/external/src/llvm-project-build/bin/llvm-tblgen"
	"${_retdec_source_parent}/build-ci-mingw/external/src/llvm-project-build/bin/llvm-tblgen"
)

foreach(_tbl IN LISTS _retdec_tblgen_candidates)
	if(EXISTS "${_tbl}")
		set(RETDEC_LLVM_TABLEGEN "${_tbl}" CACHE FILEPATH
			"Path to host llvm-tblgen for cross builds (auto-detected next to this source tree)" FORCE)
		message(STATUS "LLVM cross: auto-detected RETDEC_LLVM_TABLEGEN: ${RETDEC_LLVM_TABLEGEN}")
		return()
	endif()
endforeach()

message(WARNING
	"LLVM cross-build: RETDEC_LLVM_TABLEGEN is not set and no host llvm-tblgen was found under "
	"build-wsl / build / build-host / build-elf / build-ci-mingw / build-linux / build-ninja / build-debug / build-rel / …/llvm-project-build/bin/. Build LLVM for the *host* first (e.g. a Linux ELF "
	"build dir from this same RetDec commit), then re-run cmake with:\n"
	"  -DRETDEC_LLVM_TABLEGEN=/path/to/llvm-project-build/bin/llvm-tblgen\n"
	"Or export RETDEC_LLVM_TABLEGEN, or set RETDEC_LLVM_TABLEGEN_EXTRA_CANDIDATES "
	"(semicolon-separated absolute paths to llvm-tblgen). "
	"See docs/README.md (Linux / WSL → Windows PE).")
