cmake_minimum_required(VERSION 3.21)

# ── Qt6 ────────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
)
# Qt6::Svg is optional — provides QSvgGenerator for CFG SVG export.
find_package(Qt6 OPTIONAL_COMPONENTS Svg)

# Automatically run moc, uic, and rcc on files that need it.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ── Source files ───────────────────────────────────────────────────────────────

set(GUI_PANEL_SOURCES
    cli_tool_paths.cpp
    panels/inspect_panel.cpp
    panels/command_log_panel.cpp
    panels/live_console_panel.cpp
    panels/triage_banner.cpp
    panels/target_panel.cpp
    panels/signature_studio_panel.cpp
    panels/panel_base.cpp
    panels/binary_browser_panel.cpp
    panels/function_list_panel.cpp
    panels/assembly_panel.cpp
    panels/ir_panel.cpp
    panels/decompiled_c_panel.cpp
    panels/cfg_panel.cpp
    panels/type_hierarchy_panel.cpp
    panels/call_graph_panel.cpp
    panels/ai_assistant_panel.cpp
    panels/strings_browser_panel.cpp
    panels/diff_panel.cpp
    panels/settings_dialog.cpp
    settings/settings.cpp
    settings/plugin_manager.cpp
    panels/remaining_panels.cpp
    panels/progress_panel.cpp
    panels/tri_pane_code_view.cpp
)

# Panel library MOC headers only — mainwindow / project_file are linked with retdec-gui.
set(GUI_HEADERS
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/cli_tool_paths.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/inspect_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/command_log_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/live_console_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/triage_banner.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/target_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/signature_studio_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/panel_base.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/binary_browser_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/function_list_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/assembly_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/ir_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/decompiled_c_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/cfg_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/type_hierarchy_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/call_graph_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/strings_browser_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/ai_assistant_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/diagnostics_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/progress_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/tri_pane_code_view.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/diff_panel.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/panels/settings_dialog.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/settings/settings.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/settings/plugin_manager.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/settings/plugin_interface.h
)

# ── Launch helpers (headless / argv filtering for CI and automated debug) ─────

add_library(retdec-gui-launch STATIC
    launch_options.cpp
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/launch_options.h
)

target_include_directories(retdec-gui-launch
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-gui-launch
    PUBLIC
        Qt6::Core
)

# ── Panel library (shared between executable and tests) ────────────────────────

add_library(retdec-gui-panels STATIC
    ${GUI_PANEL_SOURCES}
    ${GUI_HEADERS}
)

target_include_directories(retdec-gui-panels
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-gui-panels
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        retdec-qwen3
)

# ── Executable ─────────────────────────────────────────────────────────────────

add_executable(retdec-gui WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    project_file.cpp
    resources/resources.qrc
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/mainwindow.h
    ${PROJECT_SOURCE_DIR}/include/retdec/gui/project_file.h
    ${GUI_HEADERS}
)

target_include_directories(retdec-gui
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-gui
    PRIVATE
        retdec-gui-launch
        retdec-gui-panels
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        $<TARGET_NAME_IF_EXISTS:Qt6::Svg>
        retdec-qwen3
)

if(TARGET Qt6::Svg)
    target_compile_definitions(retdec-gui PRIVATE QT_SVG_LIB)
endif()

# Optional: build retdec-gui with AddressSanitizer.
# Enable with:  cmake -DRETDEC_GUI_ASAN=ON ...
option(RETDEC_GUI_ASAN "Build retdec-gui with /fsanitize=address (MSVC/Clang)." OFF)
if(RETDEC_GUI_ASAN)
    if(MSVC)
        target_compile_options(retdec-gui PRIVATE /fsanitize=address /Zi)
        target_link_options(retdec-gui    PRIVATE /DEBUG)
        target_compile_definitions(retdec-gui PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
        target_compile_options(retdec-gui-panels PRIVATE /fsanitize=address /Zi)
        target_compile_definitions(retdec-gui-panels PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    else()
        target_compile_options(retdec-gui PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(retdec-gui    PRIVATE -fsanitize=address,undefined)
        target_compile_options(retdec-gui-panels PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    endif()
    message(STATUS "retdec-gui: AddressSanitizer ENABLED")
endif()

set_target_properties(retdec-gui PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
    OUTPUT_NAME           "retdec-gui"
    # On macOS bundle the app properly.
    MACOSX_BUNDLE_BUNDLE_NAME          "RetDec"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
)

# ── Qt6 deployment helper (Windows) ────────────────────────────────────────────
set(RETDEC_QT_OFFSCREEN_DLL "")
if(WIN32)
    cmake_path(SET RETDEC_QT_OFFSCREEN_DLL NORMALIZE "${Qt6_DIR}/../../../plugins/platforms/qoffscreen.dll")
    if(NOT EXISTS "${RETDEC_QT_OFFSCREEN_DLL}")
        set(RETDEC_QT_OFFSCREEN_DLL "")
    endif()
endif()

if(WIN32 AND RETDEC_QT_OFFSCREEN_DLL)
    function(retdec_gui_deploy_offscreen_plugin target_name)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RETDEC_QT_OFFSCREEN_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>/platforms/"
            COMMENT "Deploy Qt platforms/qoffscreen.dll next to ${target_name} (headless)"
        )
    endfunction()
    retdec_gui_deploy_offscreen_plugin(retdec-gui)
endif()

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET retdec-gui POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-compiler-runtime
                "$<TARGET_FILE:retdec-gui>"
            COMMENT "Running windeployqt for retdec-gui (build tree)"
        )
    endif()
endif()

# ── Installation ───────────────────────────────────────────────────────────────
install(TARGETS retdec-gui
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)

# Deploy Qt next to installed retdec-gui.exe (POST_BUILD does not affect install prefix).
if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/install-windeployqt-gui.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/install-windeployqt-gui.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install-windeployqt-gui.cmake")
endif()

# ── Optional: macOS deploy ─────────────────────────────────────────────────────
if(APPLE)
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(MACDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET retdec-gui POST_BUILD
            COMMAND "${MACDEPLOYQT_EXECUTABLE}"
                "$<TARGET_BUNDLE_DIR:retdec-gui>" -dmg
            COMMENT "Running macdeployqt for retdec-gui"
        )
    endif()
endif()
