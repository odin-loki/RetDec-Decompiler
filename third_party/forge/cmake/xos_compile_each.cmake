# Compile each source in SRCS (;-list, passed as |-list) for TARGET with zig, objects into OUT.
# Used instead of -fsyntax-only, which zig 0.17 breaks ("failed to rename tmp/... .o").
string(REPLACE "|" ";" _srcs "${SRCS}")
string(REPLACE "|" ";" _inc "${INCS}")
file(MAKE_DIRECTORY "${OUT}")
foreach(s ${_srcs})
  get_filename_component(n "${s}" NAME_WE)
  execute_process(COMMAND "${ZIG}" c++ -target ${TARGET} -std=c++23 -c ${_inc} "${s}" -o "${OUT}/${n}.o" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${s} failed for ${TARGET}")
  endif()
endforeach()
