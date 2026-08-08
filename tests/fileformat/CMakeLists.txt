
add_executable(retdec-fileformat-tests
    format_lattice_test.cpp
    lief_adapter_test.cpp
)

target_compile_features(retdec-fileformat-tests PRIVATE cxx_std_17)

target_link_libraries(retdec-fileformat-tests
    PRIVATE
        retdec::fileformat
        retdec::fileformat-lattice
        retdec::deps::gtest_main
)

if(RETDEC_HAS_LIEF)
	target_compile_definitions(retdec-fileformat-tests PRIVATE RETDEC_HAS_LIEF)
elseif(EXISTS "${CMAKE_BINARY_DIR}/lief-enabled.txt")
	file(READ "${CMAKE_BINARY_DIR}/lief-enabled.txt" _lief_flag)
	string(STRIP "${_lief_flag}" _lief_flag)
	if(_lief_flag STREQUAL "1")
		target_compile_definitions(retdec-fileformat-tests PRIVATE RETDEC_HAS_LIEF)
	endif()
endif()

include(GoogleTest)
gtest_discover_tests(retdec-fileformat-tests)
