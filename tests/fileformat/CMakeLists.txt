
add_executable(retdec-fileformat-tests
    ar_archive_format_probe_tests.cpp
    asn1_test.cpp
    bitmap_image_tests.cpp
    character_iterator_test.cpp
    coff_format_tests.cpp
    declared_file_length_tests.cpp
    elf_format_tests.cpp
    elfio_bounds_test.cpp
    format_detection_tests.cpp
    format_factory_tests.cpp
    format_lattice_test.cpp
    intel_hex_format_20bit_tests.cpp
    intel_hex_format_tests.cpp
    intel_hex_token_test.cpp
    lief_adapter_test.cpp
    macho_format_tests.cpp
    other_tests.cpp
    pe_format_tests.cpp
    raw_data_format_tests.cpp
)

target_compile_features(retdec-fileformat-tests PRIVATE cxx_std_17)

# The format tests include "fileformat/fileformat_tests.h" relative to tests/.
target_include_directories(retdec-fileformat-tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

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
