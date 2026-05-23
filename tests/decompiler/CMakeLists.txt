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

# ── Format router probe + unit tests ─────────────────────────────────────────
set(_MANAGED_DECOMPILER_SRC "${CMAKE_SOURCE_DIR}/src/retdec-decompiler/managed_decompiler.cpp")

add_executable(format_router_probe
    format_router_probe.cpp
    "${_MANAGED_DECOMPILER_SRC}"
)
target_include_directories(format_router_probe
    PRIVATE "${CMAKE_SOURCE_DIR}/src/retdec-decompiler"
)
target_compile_features(format_router_probe PUBLIC cxx_std_17)
target_link_libraries(format_router_probe
    retdec-jvm-parser
    retdec-java-emitter
    retdec-cli-parser
    retdec-csharp-emitter
    retdec-dex-parser
    retdec-pyc-parser
    retdec-py-reconstruct
    retdec-py-emitter
    retdec-lua-parser
    retdec-wasm-parser
)

add_test(
    NAME format_router_unit
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/format_router_test.py"
        "$<TARGET_FILE:format_router_probe>"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(format_router_unit PROPERTIES
    TIMEOUT 60
    LABELS "decompiler;unit;managed"
)

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

# ── Golden corpus native fixtures (hello, vector_sort) ───────────────────────
set(_CORPUS_FIXTURES_DIR "${CMAKE_CURRENT_BINARY_DIR}/corpus_fixtures")
file(MAKE_DIRECTORY "${_CORPUS_FIXTURES_DIR}")

set(_CORPUS_NATIVE_SOURCES hello vector_sort)
set(_CORPUS_FIXTURE_OUTPUTS "${_FIB_BIN}")
foreach(_name IN LISTS _CORPUS_NATIVE_SOURCES)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/corpus/sources/${_name}.c")
    set(_bin "${_CORPUS_FIXTURES_DIR}/${_name}${CMAKE_EXECUTABLE_SUFFIX}")
    list(APPEND _CORPUS_FIXTURE_OUTPUTS "${_bin}")
    if(MSVC)
        add_custom_command(
            OUTPUT "${_bin}"
            COMMAND ${CMAKE_C_COMPILER} /nologo /O1 /Fe:"${_bin}" "${_src}"
            DEPENDS "${_src}"
            COMMENT "Building corpus fixture ${_name} (MSVC)"
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT "${_bin}"
            COMMAND ${CMAKE_C_COMPILER} -O1 -o "${_bin}" "${_src}"
            DEPENDS "${_src}"
            COMMENT "Building corpus fixture ${_name}"
            VERBATIM
        )
    endif()
endforeach()

# fib_smoke lives in binary dir; link/copy into corpus_fixtures for manifest lookup
set(_CORPUS_FIB_LINK "${_CORPUS_FIXTURES_DIR}/fib_smoke${CMAKE_EXECUTABLE_SUFFIX}")
if(WIN32)
    add_custom_command(
        OUTPUT "${_CORPUS_FIB_LINK}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_FIB_BIN}" "${_CORPUS_FIB_LINK}"
        DEPENDS "${_FIB_BIN}"
        COMMENT "Staging fib_smoke into corpus_fixtures"
        VERBATIM
    )
else()
    add_custom_command(
        OUTPUT "${_CORPUS_FIB_LINK}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${_FIB_BIN}" "${_CORPUS_FIB_LINK}"
        DEPENDS "${_FIB_BIN}"
        COMMENT "Linking fib_smoke into corpus_fixtures"
        VERBATIM
    )
endif()
list(APPEND _CORPUS_FIXTURE_OUTPUTS "${_CORPUS_FIB_LINK}")

# Optional hello.pyc for managed corpus entry
set(_CORPUS_HELLO_PYC "${CMAKE_CURRENT_SOURCE_DIR}/corpus/fixtures/hello.pyc")
set(_CORPUS_HELLO_PY "${CMAKE_CURRENT_SOURCE_DIR}/corpus/sources/hello.py")
add_custom_command(
    OUTPUT "${_CORPUS_HELLO_PYC}"
    COMMAND ${Python3_EXECUTABLE} -c
        "import py_compile; py_compile.compile('${_CORPUS_HELLO_PY}', cfile='${_CORPUS_HELLO_PYC}', doraise=True)"
    DEPENDS "${_CORPUS_HELLO_PY}"
    COMMENT "Generating corpus hello.pyc"
    VERBATIM
)
list(APPEND _CORPUS_FIXTURE_OUTPUTS "${_CORPUS_HELLO_PYC}")

add_custom_target(retdec-decompiler-corpus-fixtures ALL DEPENDS ${_CORPUS_FIXTURE_OUTPUTS})
add_dependencies(retdec-decompiler-corpus-fixtures retdec-decompiler-fixture-fib)

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

add_test(
    NAME decompiler_smoke_managed_formats
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/managed_format_smoke_test.py"
        "${RETDEC_DECOMPILER}"
        "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(decompiler_smoke_managed_formats PROPERTIES
    TIMEOUT 300
    LABELS "decompiler;smoke;managed;integration"
)

# ── Golden corpus regression ──────────────────────────────────────────────────
add_test(
    NAME decompiler_corpus_regression
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/corpus/corpus_regression_test.py"
        "${RETDEC_DECOMPILER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/corpus/manifest.yaml"
        "${_CORPUS_FIXTURES_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/corpus_out"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(decompiler_corpus_regression PROPERTIES
    TIMEOUT 600
    LABELS "decompiler;corpus;integration"
)

add_test(
    NAME semantic_detection_json_schema
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/semantic_detection_json_test.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(semantic_detection_json_schema PROPERTIES
    TIMEOUT 30
    LABELS "decompiler;unit"
)

add_test(
    NAME semantic_c_hint_json_schema
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/semantic_c_hint_test.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(semantic_c_hint_json_schema PROPERTIES
    TIMEOUT 30
    LABELS "decompiler;unit"
)

add_test(
    NAME decompiler_intel_export_mock
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/intel_export_test.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(decompiler_intel_export_mock PROPERTIES
    TIMEOUT 60
    LABELS "decompiler;unit;intel"
)

# ── Pipeline profile JSON schema ──────────────────────────────────────────────
add_test(
    NAME decompiler_validate_pipeline_profiles
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/validate_pipeline_test.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(decompiler_validate_pipeline_profiles PROPERTIES
    TIMEOUT 60
    LABELS "decompiler;unit;schema"
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
