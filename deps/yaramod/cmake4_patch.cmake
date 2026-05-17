# Nested ExternalProjects (fmt, re2, googletest) use old cmake_minimum_required; CMake 4 needs a policy floor.
if(NOT DEFINED YARAMOD_SOURCE_DIR)
	message(FATAL_ERROR "cmake4_patch.cmake: YARAMOD_SOURCE_DIR not set")
endif()
function(_yaramod_patch_file relpath old_snippet new_snippet)
	set(_path "${YARAMOD_SOURCE_DIR}/${relpath}")
	if(NOT EXISTS "${_path}")
		return()
	endif()
	file(READ "${_path}" _c)
	string(FIND "${_c}" "${old_snippet}" _pos)
	if(_pos LESS 0)
		return()
	endif()
	string(REPLACE "${old_snippet}" "${new_snippet}" _c "${_c}")
	file(WRITE "${_path}" "${_c}")
	message(STATUS "YaraMod: CMake 4 policy line added (${relpath})")
endfunction()

# deps/pog/deps/fmt/CMakeLists.txt
_yaramod_patch_file(
	"deps/pog/deps/fmt/CMakeLists.txt"
[=[	CMAKE_ARGS
		-DCMAKE_INSTALL_PREFIX=${FMT_INSTALL_DIR}]=]
[=[	CMAKE_ARGS
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5
		-DCMAKE_INSTALL_PREFIX=${FMT_INSTALL_DIR}]=]
)

# deps/pog/deps/re2/CMakeLists.txt
_yaramod_patch_file(
	"deps/pog/deps/re2/CMakeLists.txt"
[=[	CMAKE_ARGS
		-DCMAKE_INSTALL_PREFIX=${RE2_INSTALL_DIR}]=]
[=[	CMAKE_ARGS
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5
		-DCMAKE_INSTALL_PREFIX=${RE2_INSTALL_DIR}]=]
)

# deps/googletest/CMakeLists.txt
_yaramod_patch_file(
	"deps/googletest/CMakeLists.txt"
[=[	CMAKE_ARGS
		# This does not work on MSVC, but is useful on Linux.]=]
[=[	CMAKE_ARGS
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5
		# This does not work on MSVC, but is useful on Linux.]=]
)

# MSVC + Ninja: `-- -m` is for MSBuild only; Ninja rejects `-m`. Use `-j` like Unix.
_yaramod_patch_file(
	"deps/pog/deps/fmt/CMakeLists.txt"
[=[if(MSVC)
	set(FMT_BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG> -- -m)
	set(FMT_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config $<CONFIG>)
else()
	set(FMT_BUILD_COMMAND ${CMAKE_COMMAND} --build . -- -j${CPUS})
	set(FMT_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install)
endif()]=]
[=[if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
	set(FMT_BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG> -- -m)
	set(FMT_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config $<CONFIG>)
else()
	set(FMT_BUILD_COMMAND ${CMAKE_COMMAND} --build . -- -j${CPUS})
	set(FMT_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install)
endif()]=]
)
_yaramod_patch_file(
	"deps/pog/deps/re2/CMakeLists.txt"
[=[if(MSVC)
	set(RE2_BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG> -- -m)
	set(RE2_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config $<CONFIG>)
else()
	set(RE2_BUILD_COMMAND ${CMAKE_COMMAND} --build . -- -j${CPUS})
	set(RE2_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install)
endif()]=]
[=[if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
	set(RE2_BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG> -- -m)
	set(RE2_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config $<CONFIG>)
else()
	set(RE2_BUILD_COMMAND ${CMAKE_COMMAND} --build . -- -j${CPUS})
	set(RE2_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install)
endif()]=]
)

# MSVC: fmt-dep / re2-dep are separate ExternalProject configures; they need explicit
# rc/mt paths. Values must be quoted in CMakeLists so paths with spaces survive cmd.exe.
if(DEFINED YARAMOD_WINDOWS_RC_COMPILER AND YARAMOD_WINDOWS_RC_COMPILER
	AND DEFINED YARAMOD_WINDOWS_MT AND YARAMOD_WINDOWS_MT)
	string(CONCAT _yaramod_sdk_cmake_args
		"\t\t\"-DCMAKE_RC_COMPILER=${YARAMOD_WINDOWS_RC_COMPILER}\"\n"
		"\t\t\"-DCMAKE_MT=${YARAMOD_WINDOWS_MT}\"")
	foreach(_yaramod_nested IN ITEMS "deps/pog/deps/fmt/CMakeLists.txt" "deps/pog/deps/re2/CMakeLists.txt")
		set(_ypath "${YARAMOD_SOURCE_DIR}/${_yaramod_nested}")
		if(NOT EXISTS "${_ypath}")
			continue()
		endif()
		file(READ "${_ypath}" _yc)
		if(_yc MATCHES "\"-DCMAKE_RC_COMPILER=")
			continue()
		endif()
		# Repair older patch that injected unquoted paths (broken on "Program Files (x86)/...")
		if(_yc MATCHES "\t\t-DCMAKE_RC_COMPILER=")
			string(REGEX REPLACE
				"\t\t-DCMAKE_RC_COMPILER=[^\r\n]+\r?\n\t\t-DCMAKE_MT=[^\r\n]+"
				"${_yaramod_sdk_cmake_args}"
				_yc "${_yc}")
		else()
			string(REPLACE
				"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
				"-DCMAKE_POLICY_VERSION_MINIMUM=3.5\n${_yaramod_sdk_cmake_args}"
				_yc "${_yc}")
		endif()
		file(WRITE "${_ypath}" "${_yc}")
		message(STATUS "YaraMod: Windows SDK tools forwarded to nested dep (${_yaramod_nested})")
	endforeach()
endif()

# Nested re2/googletest hard-code Release; MSVC+Ninja Debug needs /MDd + matching _ITERATOR_DEBUG_LEVEL (LNK2038).
function(_yaramod_nested_ep_match_parent_build_type relpath)
	set(_path "${YARAMOD_SOURCE_DIR}/${relpath}")
	if(NOT EXISTS "${_path}")
		return()
	endif()
	file(READ "${_path}" _yc)
	if(_yc MATCHES "\\$\\{CMAKE_BUILD_TYPE\\}")
		return()
	endif()
	set(_new_line "\t\t-DCMAKE_BUILD_TYPE=")
	string(APPEND _new_line "$" "{" "CMAKE_BUILD_TYPE" "}")
	string(REPLACE "\t\t-DCMAKE_BUILD_TYPE=Release" "${_new_line}" _yc "${_yc}")
	string(REPLACE "\t\t-DCMAKE_BUILD_TYPE=Debug" "${_new_line}" _yc "${_yc}")
	file(WRITE "${_path}" "${_yc}")
	message(STATUS "YaraMod: nested ExternalProject uses parent CMAKE_BUILD_TYPE (${relpath})")
endfunction()

_yaramod_nested_ep_match_parent_build_type("deps/pog/deps/re2/CMakeLists.txt")
_yaramod_nested_ep_match_parent_build_type("deps/googletest/CMakeLists.txt")
