# retdec-gui tests: model tests need no window; view tests drive a headless
# FuseUI UiContext (and render with CpuRasterizer where pixels matter).
file(GLOB RETDEC_GUI_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(retdec-gui-tests ${RETDEC_GUI_TEST_SOURCES})
target_link_libraries(retdec-gui-tests PRIVATE retdec-gui-ui retdec::deps::gtest retdec::deps::gtest_main)
target_include_directories(retdec-gui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(retdec-gui-tests PRIVATE
	RETDEC_GUI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
	RETDEC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_test(NAME retdec-gui-tests COMMAND retdec-gui-tests
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(retdec-gui-tests PROPERTIES LABELS "unit;gui")
