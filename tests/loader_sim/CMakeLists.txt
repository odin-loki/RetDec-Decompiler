# xbyte_width_guard_test.cpp needs no library of its own: it #includes the
# bounds-arithmetic region of src/fileformat/file_format/file_format.cpp and
# src/loader/loader/image.cpp by relative path, with each file's
# RETDEC_..._BOUNDS_KERNELS_ONLY macro defined so that everything needing an
# LLVM tree is fenced off. The functions it calls are `inline`, so linking this
# target against retdec::fileformat or retdec::loader as well would not produce
# duplicate symbols -- but it is not needed and is deliberately not done, since
# the point of that arrangement is that the suite also builds under
# scripts/standalone_check.sh, which has no LLVM at all.
add_executable(retdec-loader-sim-tests
    loader_sim_test.cpp
    xbyte_width_guard_test.cpp
)

target_link_libraries(retdec-loader-sim-tests
    PRIVATE
        retdec::loader-sim
        retdec::deps::gtest_main
)

set_target_properties(retdec-loader-sim-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
# The suite reads no files at run time, so it does not care which directory
# gtest_discover_tests launches it from. An earlier version of
# xbyte_width_guard_test.cpp checked its invariants by reading the two sources
# off disk and failed here, out of the binary directory, for want of a path.
gtest_discover_tests(retdec-loader-sim-tests)
