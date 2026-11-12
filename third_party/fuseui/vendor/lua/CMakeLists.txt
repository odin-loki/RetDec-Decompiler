# Lua 5.2.3 (MIT, see LICENSE.txt) — vendored for FUSE fuse_script's bundled Lua backend.
# Sources are unmodified copies of the Lua 5.2.3 src/ tree (library only; no lua/luac CLIs).
cmake_minimum_required(VERSION 3.21.0)

if(TARGET fuse_lua)
    return()
endif()

# Always enable C here: language variables are directory-scoped, so a check against the global
# ENABLED_LANGUAGES would skip this when a sibling directory enabled C first.
enable_language(C)

set(_fuse_lua_sources
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c
    lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c
    lvm.c lzio.c
    lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c lmathlib.c
    loslib.c lstrlib.c ltablib.c loadlib.c linit.c)
list(TRANSFORM _fuse_lua_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/")

add_library(fuse_lua STATIC ${_fuse_lua_sources})
add_library(fuse::lua ALIAS fuse_lua)

set_target_properties(fuse_lua PROPERTIES
    LINKER_LANGUAGE C
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(fuse_lua SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

if(APPLE)
    target_compile_definitions(fuse_lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(fuse_lua PRIVATE LUA_USE_LINUX)
endif()

# Third-party C: silence warnings for this target only.
if(MSVC)
    target_compile_options(fuse_lua PRIVATE /w)
else()
    # Lua 5.2 hashes numbers with wrapping signed int arithmetic (ltable.c hashnum):
    # -fwrapv makes that well-defined (UBSan-clean) with identical codegen.
    target_compile_options(fuse_lua PRIVATE -w -fwrapv)
endif()

if(UNIX)
    target_link_libraries(fuse_lua PUBLIC m)
endif()
