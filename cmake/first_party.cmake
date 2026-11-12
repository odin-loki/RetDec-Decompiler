# First-party libraries vendored under third_party/ (see third_party/README.md).
#
#   Forge   -- OS layer (processes, files, settings, clipboard), core, support
#              (JSON, thread pool, BLAKE3) and the inference stack.
#   FuseUI  -- the GUI toolkit retdec-gui is built on (replaced Qt).
#
# Both are added EXCLUDE_FROM_ALL: only the targets RetDec links are built.

set(RETDEC_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party")
get_filename_component(RETDEC_THIRD_PARTY_DIR "${RETDEC_THIRD_PARTY_DIR}" ABSOLUTE)

# ---- Forge -------------------------------------------------------------------
set(FORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FORGE_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(FORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FORGE_REPRODUCIBLE ON CACHE BOOL "" FORCE)
# RetDec has its own hardening checks (QUAL-08); Forge's flags stay inside its
# own targets either way.
set(FORGE_HARDENING ON CACHE BOOL "" FORCE)
# rpc-serve TLS is not used by RetDec and would pull OpenSSL into Forge's graph.
set(FORGE_TLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RETDEC_THIRD_PARTY_DIR}/forge" "${CMAKE_BINARY_DIR}/third_party/forge" EXCLUDE_FROM_ALL)

# One target RetDec code links for Forge's header-only layers.
if(NOT TARGET retdec_forge)
	add_library(retdec_forge INTERFACE)
	target_link_libraries(retdec_forge INTERFACE forge_core forge_os)
	target_include_directories(retdec_forge SYSTEM INTERFACE
		"${RETDEC_THIRD_PARTY_DIR}/forge/support/include")
	target_compile_features(retdec_forge INTERFACE cxx_std_23)
	add_library(retdec::forge ALIAS retdec_forge)
endif()

# Forge's inference stack (model, session, sampler, GBNF grammar, importers),
# for src/neural. Beyond the header-only layers it builds two static
# libraries: forge_simd_mv (the CPU kernels, one TU per ISA) and forge_trust
# (vendored Monocypher). GPU runtimes are loaded at run time, so nothing GPU
# is linked here.
if(NOT TARGET retdec_forge_inference)
	add_library(retdec_forge_inference INTERFACE)
	target_link_libraries(retdec_forge_inference INTERFACE
		forge_inference forge_verify forge_simd_mv retdec_forge)
	target_compile_features(retdec_forge_inference INTERFACE cxx_std_23)
	add_library(retdec::forge_inference ALIAS retdec_forge_inference)
endif()

# RetDec's warning flags (-Wold-style-cast, -Wcast-qual, ...) and src/neural's
# -Werror are for RetDec's code: consumers see Forge's headers as system
# headers. CMake 3.25 is Forge's own minimum, so the property is always there.
foreach(_forge_tgt
		forge_core forge_os forge_ops forge_simd forge_gpu forge_graph
		forge_modelfile forge_security forge_inference forge_verify forge_trust)
	if(TARGET ${_forge_tgt})
		set_property(TARGET ${_forge_tgt} PROPERTY SYSTEM TRUE)
	endif()
endforeach()

# ---- FuseUI ------------------------------------------------------------------
if(RETDEC_GUI)
	set(FUSE_BUILD_UI_WIDGETS ON CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_DOC ON CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_APP ON CACHE BOOL "" FORCE)
	set(FUSE_BUILD_APP_SAMPLES OFF CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_TESTS OFF CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_GAME OFF CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_THEORA OFF CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_SQL OFF CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_DESIGNER OFF CACHE BOOL "" FORCE)
	set(FUSE_BUILD_UI_SCRIPT OFF CACHE BOOL "" FORCE)
	# CPU rasteriser + native blit everywhere; a decompiler UI has no need for
	# a Vulkan loader, and it keeps the GUI running on machines without one.
	set(FUSE_BUILD_UI_VK OFF CACHE BOOL "" FORCE)
	add_subdirectory("${RETDEC_THIRD_PARTY_DIR}/fuseui/Source/FUSE/UI"
		"${CMAKE_BINARY_DIR}/third_party/fuseui" EXCLUDE_FROM_ALL)
endif()
