#
# Utility functions, macros, etc.
#

function(append_if condition value)
	if (${condition})
		foreach(variable ${ARGN})
			set(${variable} "${${variable}} ${value}" PARENT_SCOPE)
		endforeach(variable)
	endif()
endfunction()

# Forwards this build's compiler launcher (ccache, sccache) into an
# ExternalProject's CMAKE_ARGS.  Sets ${out} to an empty list when no launcher
# is configured, so an ordinary build is unchanged.
#
# Without this the launcher stops at the superbuild boundary, and the LLVM
# sub-build -- which is almost all of the wall clock -- recompiles from scratch
# on every run.  ctest-linux runs hendrikmuhs/ccache-action and its own log said
# what that was worth: "Cache size (GB): 0.0 / 0.5" and "Not saving cache
# because no objects are cached", against a build step of 55 minutes.
#
# Usage: retdec_compiler_launcher_args(MY_ARGS) then ${MY_ARGS} in CMAKE_ARGS.
function(retdec_compiler_launcher_args out)
	set(args "")
	foreach(lang C CXX)
		if(CMAKE_${lang}_COMPILER_LAUNCHER)
			list(APPEND args
				"-DCMAKE_${lang}_COMPILER_LAUNCHER=${CMAKE_${lang}_COMPILER_LAUNCHER}")
		endif()
	endforeach()
	set(${out} "${args}" PARENT_SCOPE)
endfunction()

# Forces a configure step for the given external project.
# The configure step for external projects is needed to (1) detect source-file
# changes and (2) fix infinite recursion of 'make' after a terminated build.
# Usage example: force_configure_step(your-external-project)
macro(force_configure_step target)
	# This solution is based on
	# http://comments.gmane.org/gmane.comp.programming.tools.cmake.user/43024
	ExternalProject_Add_Step(${target} force-configure
		COMMAND ${CMAKE_COMMAND} -E echo "Force configure of ${target}"
		DEPENDEES update
		DEPENDERS configure
		ALWAYS 1
	)
endmacro()

# Check if 'variable' changed between two consequent CMake runs.
# Return bool in 'result'.
# Usage example:
#     check_if_variable_changed(VAR CHANGED)
#     if (CHANGED)
#         ...
#     endif()
# Sources:
# https://stackoverflow.com/questions/43542381/set-a-cmake-variable-if-it-is-not-changed-by-the-user
# https://markdewing.github.io/blog/posts/notes-on-cmake/
function(check_if_variable_changed variable result)
	if(NOT DEFINED ${variable})
		set(${variable} "")
	endif()

	if(NOT DEFINED ${variable}_old)
		set(${variable}_old ${${variable}} CACHE INTERNAL "Copy of ${variable}")
	endif()

	if(${variable} STREQUAL ${variable}_old)
		set(${result} FALSE PARENT_SCOPE)
	else()
		set(${result} TRUE PARENT_SCOPE)
	endif()
	# Store current value in the "shadow" variable unconditionally.
	set(${variable}_old ${${variable}} CACHE INTERNAL "Copy of ${variable}")
endfunction()

# Clean all CMake files in the given 'directory'.
# Sources:
# https://stackoverflow.com/questions/9680420/looking-for-a-cmake-clean-command-to-clear-up-cmake-output
function(clean_cmake_files directory)
	set(cmake_generated
		${directory}/CMakeCache.txt
		${directory}/cmake_install.cmake
		${directory}/Makefile
		${directory}/CMakeFiles
	)

	foreach(file ${cmake_generated})
		if (EXISTS ${file})
			file(REMOVE_RECURSE ${file})
		endif()
	endforeach(file)
endfunction()

# add_subdirectory(dir) if the cond is true,
#
macro(cond_add_subdirectory dir cond)
	if(${cond})
		add_subdirectory(${dir})
	endif()
endmacro()
