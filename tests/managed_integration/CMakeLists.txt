add_subdirectory(fuzz)

find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_FOUND)
    message(STATUS "Python3 not found -- managed integration tests disabled")
    return()
endif()

if(NOT TARGET retdec-decompiler)
    message(STATUS "retdec-decompiler not built -- managed integration tests disabled")
    return()
endif()

# Path to the compiled retdec-decompiler binary in this build tree.
set(RETDEC_BIN "$<TARGET_FILE:retdec-decompiler>")

# Integration test: run all fixture languages.
add_test(
    NAME managed_integration
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/run_integration_tests.py"
        --retdec-bin "${RETDEC_BIN}"
        --fixtures   "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        --golden     "${CMAKE_CURRENT_SOURCE_DIR}/golden"
        --timeout    120
        --junit      "${CMAKE_CURRENT_BINARY_DIR}/managed_integration.xml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(managed_integration PROPERTIES
    TIMEOUT 1800
    LABELS  "managed;integration"
)

# Per-language convenience targets.
foreach(LANG java kotlin dex csharp vbnet fsharp python lua wasm malformed)
    add_test(
        NAME "managed_integration_${LANG}"
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/run_integration_tests.py"
            --retdec-bin "${RETDEC_BIN}"
            --fixtures   "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
            --golden     "${CMAKE_CURRENT_SOURCE_DIR}/golden"
            --language   "${LANG}"
            --timeout    120
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties("managed_integration_${LANG}" PROPERTIES
        TIMEOUT 600
        LABELS  "managed;${LANG}"
    )
endforeach()
