# Maps a FORGE_LOG_MIN_LEVEL name to the value of the FORGE_LOG_MIN_LEVEL macro in core/log.hpp:
# trace..off -> 0..6 (a number 0..6 is accepted as is), none or empty -> "" (no definition), anything else -> INVALID.
# Case-insensitive; only the full level name is accepted. Tested by tests/log_min_level.cmake.
function(forge_log_level_value name out)
  string(TOLOWER "${name}" n)
  set(levels trace debug info warn error fatal off)
  if(n STREQUAL "" OR n STREQUAL "none")
    set(v "")
  elseif(n MATCHES "^[0-6]$")
    set(v ${n})
  else()
    list(FIND levels "${n}" v)
    if(v EQUAL -1)
      set(v INVALID)
    endif()
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()
