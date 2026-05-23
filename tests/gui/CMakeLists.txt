set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt targets are scoped to src/gui; load them again for this test directory.
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Test
)

add_executable(retdec-gui-tests
    qt_test_main.cpp
    launch_options_test.cpp
    progress_panel_test.cpp
    tri_pane_test.cpp
    cfg_panel_test.cpp
    type_hierarchy_call_graph_test.cpp
    function_list_panel_test.cpp
    ai_assistant_panel_test.cpp
    strings_browser_panel_test.cpp
    diff_panel_test.cpp
    settings_test.cpp
    live_console_panel_test.cpp
    decompiler_launch_test.cpp
    batch_decompile_queue_test.cpp
    artifact_loader_test.cpp
    export_bundle_test.cpp
    mainwindow_layout_test.cpp
    comprehensive_smoke_test.cpp
    # mainwindow + project_file are NOT in retdec-gui-panels (the exe owns them),
    # so we compile them into the test target. This keeps a single source of truth
    # without restructuring the production binary's link order.
    ${PROJECT_SOURCE_DIR}/src/gui/mainwindow.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/batch_decompile_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/project_file.cpp
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/mainwindow.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/batch_decompile_dialog.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/project_file.h
    ${PROJECT_SOURCE_DIR}/src/gui/resources/resources.qrc
)

target_include_directories(retdec-gui-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(retdec-gui-tests
    PRIVATE
        retdec-gui-launch
        retdec-gui-panels
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Test
        retdec::deps::gtest
        retdec-qwen3
)

set_target_properties(retdec-gui-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
)

# Optional: build the test exe with AddressSanitizer. Catches use-after-free,
# heap OOB, double-free, leaks. Requires the ASAN runtime to be on PATH /
# in the same dir as the exe (windeployqt step copies our DLLs but not
# clang_rt.asan_dynamic-x86_64.dll; users on Windows need the
# "C++ AddressSanitizer" workload in VS Installer).
#
# Enable with:  cmake -DRETDEC_GUI_TESTS_ASAN=ON ...
option(RETDEC_GUI_TESTS_ASAN
       "Build retdec-gui-tests with /fsanitize=address (MSVC/Clang)." OFF)
if(RETDEC_GUI_TESTS_ASAN)
    if(MSVC)
        target_compile_options(retdec-gui-tests PRIVATE /fsanitize=address /Zi)
        target_link_options(retdec-gui-tests    PRIVATE /DEBUG)
        target_compile_definitions(retdec-gui-tests PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    else()
        target_compile_options(retdec-gui-tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(retdec-gui-tests    PRIVATE -fsanitize=address,undefined)
    endif()
    message(STATUS "retdec-gui-tests: AddressSanitizer ENABLED")
endif()

# Windows: bundle Qt next to the test exe so ctest / IDE runs without Qt on PATH.
if(WIN32)
    find_program(WINDEPLOYQT_GUI_TESTS_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT_GUI_TESTS_EXECUTABLE)
        add_custom_command(TARGET retdec-gui-tests POST_BUILD
            COMMAND "${WINDEPLOYQT_GUI_TESTS_EXECUTABLE}"
                --no-compiler-runtime
                "$<TARGET_FILE:retdec-gui-tests>"
            COMMENT "Running windeployqt for retdec-gui-tests (build tree)"
        )
    else()
        cmake_path(SET RETDEC_QT_QWINDOWS_DLL NORMALIZE "${Qt6_DIR}/../../../plugins/platforms/qwindows.dll")
        if(EXISTS "${RETDEC_QT_QWINDOWS_DLL}")
            add_custom_command(TARGET retdec-gui-tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:retdec-gui-tests>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${RETDEC_QT_QWINDOWS_DLL}"
                    "$<TARGET_FILE_DIR:retdec-gui-tests>/platforms/"
                COMMENT "Deploy Qt platforms/qwindows.dll next to retdec-gui-tests (no windeployqt)"
            )
        else()
            message(WARNING "windeployqt not found and qwindows.dll missing; retdec-gui-tests may fail to start.")
        endif()
    endif()
    # windeployqt usually omits qoffscreen; copy for RETDEC_GUI_HEADLESS / offscreen CI.
    cmake_path(SET _retdec_qt_qoffscreen NORMALIZE "${Qt6_DIR}/../../../plugins/platforms/qoffscreen.dll")
    if(EXISTS "${_retdec_qt_qoffscreen}")
        add_custom_command(TARGET retdec-gui-tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:retdec-gui-tests>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_retdec_qt_qoffscreen}"
                "$<TARGET_FILE_DIR:retdec-gui-tests>/platforms/"
            COMMENT "Deploy Qt platforms/qoffscreen.dll next to retdec-gui-tests (headless)"
        )
    endif()
endif()

add_test(NAME retdec-gui-tests COMMAND retdec-gui-tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# ── perf_bench: standalone profiler for live-stream + decompiled-load ─────────
add_executable(retdec-gui-perf-bench
    perf_bench.cpp
)
target_link_libraries(retdec-gui-perf-bench
    PRIVATE
        retdec-gui-launch
        retdec-gui-panels
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
set_target_properties(retdec-gui-perf-bench PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
)
if(WIN32 AND WINDEPLOYQT_GUI_TESTS_EXECUTABLE)
    add_custom_command(TARGET retdec-gui-perf-bench POST_BUILD
        COMMAND "${WINDEPLOYQT_GUI_TESTS_EXECUTABLE}"
            --no-compiler-runtime
            "$<TARGET_FILE:retdec-gui-perf-bench>"
        COMMENT "Running windeployqt for retdec-gui-perf-bench (build tree)"
    )
endif()

set_tests_properties(retdec-gui-tests PROPERTIES
    ENVIRONMENT "RETDEC_GUI_HEADLESS=1")
