# Fuzzer harnesses — only built when using a Clang + libFuzzer toolchain.
# Enable with: cmake -DRETDEC_FUZZ=ON ...

option(RETDEC_FUZZ "Build libFuzzer harnesses for managed-language parsers" OFF)

if(NOT RETDEC_FUZZ)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Fuzzer harnesses require Clang. RETDEC_FUZZ ignored.")
    return()
endif()

set(FUZZ_FLAGS -fsanitize=fuzzer,address -O1 -g)

function(add_fuzz_target name src)
    add_executable(${name} ${src})
    target_compile_options(${name} PRIVATE ${FUZZ_FLAGS})
    target_link_options(${name}    PRIVATE ${FUZZ_FLAGS})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_link_libraries(${name} PRIVATE ${ARGN})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endfunction()

add_fuzz_target(fuzz_jvm_class fuzz_jvm_class.cpp
    retdec-jvm-parser
    retdec-bc-module
)

add_fuzz_target(fuzz_dex fuzz_dex.cpp
    retdec-dex-parser
    retdec-bc-module
)

add_fuzz_target(fuzz_wasm fuzz_wasm.cpp
    retdec-wasm-parser
)

add_fuzz_target(fuzz_pyc fuzz_pyc.cpp
    retdec-pyc-parser
    retdec-bc-module
)
