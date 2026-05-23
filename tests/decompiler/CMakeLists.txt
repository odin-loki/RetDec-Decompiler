find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_FOUND)
    message(STATUS "tests/decompiler: skipped (Python3 not found)")
    return()
endif()

if(NOT TARGET retdec-decompiler)
    message(STATUS "tests/decompiler: skipped (retdec-decompiler not built)")
    return()
endif()

if(NOT CMAKE_C_COMPILER)
    message(STATUS "tests/decompiler: skipped (no C compiler for fib fixture)")
    return()
endif()

set(RETDEC_DECOMPILER "$<TARGET_FILE:retdec-decompiler>")

# ── Build fib.c test binary at configure/build time ───────────────────────────
set(_FIB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../test_binaries/fib.c")
set(_FIB_BIN "${CMAKE_CURRENT_BINARY_DIR}/fib_smoke${CMAKE_EXECUTABLE_SUFFIX}")

if(MSVC)
    add_custom_command(
        OUTPUT "${_FIB_BIN}"
        COMMAND ${CMAKE_C_COMPILER} /nologo /O1 /Fe:"${_FIB_BIN}" "${_FIB_SRC}"
        DEPENDS "${_FIB_SRC}"
        COMMENT "Building fib_smoke test binary (MSVC)"
        VERBATIM
    )
else()
    add_custom_command(
        OUTPUT "${_FIB_BIN}"
        COMMAND ${CMAKE_C_COMPILER} -O1 -o "${_FIB_BIN}" "${_FIB_SRC}"
        DEPENDS "${_FIB_SRC}"
        COMMENT "Building fib_smoke test binary"
        VERBATIM
    )
endif()

add_custom_target(retdec-decompiler-fixture-fib ALL DEPENDS "${_FIB_BIN}")

# ── CLI smoke ─────────────────────────────────────────────────────────────────
add_test(
    NAME decompiler_smoke_cli_fib
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/decompilation_smoke_test.py"
        "${RETDEC_DECOMPILER}"
        "${_FIB_BIN}"
        "${CMAKE_CURRENT_BINARY_DIR}/fib_smoke_out.c"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(decompiler_smoke_cli_fib PROPERTIES
    TIMEOUT 180
    LABELS "decompiler;smoke;integration"
)

# ── Install smoke (post cmake --install; Windows only) ────────────────────────
if(WIN32)
    set(_INSTALL_DIR "${CMAKE_SOURCE_DIR}/install/windows")
    add_test(
        NAME install_smoke
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/install_smoke.ps1"
            -InstallDir "${_INSTALL_DIR}"
            -Binary "${_FIB_BIN}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(install_smoke PROPERTIES
        TIMEOUT 600
        LABELS "decompiler;gui;smoke;integration;install"
        SKIP_REGULAR_EXPRESSION "SKIP:"
        ENVIRONMENT "RETDEC_GUI_HEADLESS=1;QT_QPA_PLATFORM=offscreen"
    )
endif()

# ── GUI staging + headless / parity (requires retdec-gui) ─────────────────────
if(NOT TARGET retdec-gui)
    message(STATUS "tests/decompiler: GUI integration tests skipped (retdec-gui not built)")
    return()
endif()

set(_GUI_STAGING "${CMAKE_CURRENT_BINARY_DIR}/gui_staging")
set(_GUI_STAGING_STAMP "${_GUI_STAGING}/.staged")
set(_GUI_STAGED "${_GUI_STAGING}/retdec-gui${CMAKE_EXECUTABLE_SUFFIX}")

add_custom_command(
    OUTPUT "${_GUI_STAGING_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_GUI_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GUI_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "$<TARGET_FILE_DIR:retdec-gui>"
        "${_GUI_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:retdec-decompiler>"
        "${_GUI_STAGING}/$<TARGET_FILE_NAME:retdec-decompiler>"
    COMMAND ${CMAKE_COMMAND} -E touch "${_GUI_STAGING_STAMP}"
    DEPENDS retdec-gui retdec-decompiler
    COMMENT "Stage retdec-gui (with Qt deps) + retdec-decompiler for integration tests"
    VERBATIM
)

add_custom_target(retdec-gui-decompiler-staging ALL DEPENDS "${_GUI_STAGING_STAMP}")
add_dependencies(retdec-gui-decompiler-staging retdec-decompiler-fixture-fib)

add_test(
    NAME gui_headless_decompile_fib
    COMMAND "${_GUI_STAGED}"
        --headless
        --headless-decompile
        "${_FIB_BIN}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(gui_headless_decompile_fib PROPERTIES
    TIMEOUT 300
    LABELS "gui;decompiler;smoke;integration"
    ENVIRONMENT "RETDEC_GUI_HEADLESS=1;QT_QPA_PLATFORM=offscreen"
)

if(WIN32)
    set(_PARITY_WORK "${CMAKE_CURRENT_BINARY_DIR}/parity_work")
    add_test(
        NAME gui_cli_decompile_parity_fib
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/parity_ctest.ps1"
            -Decompiler "${RETDEC_DECOMPILER}"
            -Gui "${_GUI_STAGED}"
            -Binary "${_FIB_BIN}"
            -WorkDir "${_PARITY_WORK}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(gui_cli_decompile_parity_fib PROPERTIES
        TIMEOUT 600
        LABELS "gui;decompiler;parity;integration"
        ENVIRONMENT "RETDEC_GUI_HEADLESS=1;QT_QPA_PLATFORM=offscreen"
    )
endif()
