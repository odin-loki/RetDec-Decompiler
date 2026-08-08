# cmake/lief_optional.cmake — Optional LIEF linkage for LiefAdapter (step 29).
set(RETDEC_HAS_LIEF FALSE)

if(NOT RETDEC_ENABLE_LIEF)
	return()
endif()

find_package(LIEF CONFIG QUIET)
if(NOT LIEF_FOUND)
	find_package(LIEF QUIET)
endif()

if(LIEF_FOUND)
	message(STATUS "LIEF found — building LiefAdapter with RETDEC_HAS_LIEF")
	set(RETDEC_HAS_LIEF TRUE)
else()
	message(WARNING "RETDEC_ENABLE_LIEF=ON but LIEF not found (install liblief-dev or LIEF CMake package)")
endif()
