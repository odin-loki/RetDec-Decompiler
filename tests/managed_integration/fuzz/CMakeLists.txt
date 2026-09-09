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

# The mangled-symbol seeders. A mangled name is attacker-controlled text out
# of the symbol table, and all four seeders are hand-written parsers over it.
add_fuzz_target(fuzz_demangle fuzz_demangle.cpp
    retdec-type-seed
)

# The loader simulator: section table, relocations, imports, delay imports and
# TLS callbacks, all driven by offsets and counts out of the file.
add_fuzz_target(fuzz_loader_sim fuzz_loader_sim.cpp
    retdec-loader-sim
)

# The lattice's classification feeding the emulating unpacker, which is how the
# decompiler chains them.
add_fuzz_target(fuzz_unpack fuzz_unpack.cpp
    retdec-mini-emu
    retdec-fileformat-lattice
)

add_fuzz_target(fuzz_pyc fuzz_pyc.cpp
    retdec-pyc-parser
    retdec-bc-module
)

add_fuzz_target(fuzz_pe fuzz_pe.cpp
    retdec::fileformat
)

add_fuzz_target(fuzz_elf fuzz_elf.cpp
    retdec::fileformat
)

add_fuzz_target(fuzz_macho fuzz_macho.cpp
    retdec::fileformat
)

add_fuzz_target(fuzz_lua fuzz_lua.cpp
    retdec-lua-parser
)

add_fuzz_target(fuzz_apk fuzz_apk.cpp
    retdec-dex-parser
    retdec-bc-module
)

add_fuzz_target(fuzz_jar fuzz_jar.cpp
    retdec-jvm-parser
    retdec-bc-module
)

if(TARGET retdec::unpacker)
    add_fuzz_target(fuzz_unpacker fuzz_unpacker.cpp
        retdec::unpacker
    )
endif()

if(TARGET retdec::unpackertool)
    add_fuzz_target(fuzz_unpacker_plugins fuzz_unpacker_plugins.cpp
        retdec::unpackertool
    )
    target_include_directories(fuzz_unpacker_plugins PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
    )
endif()

if(TARGET retdec::pdbparser)
    add_fuzz_target(fuzz_pdb fuzz_pdb.cpp
        retdec::pdbparser
    )
endif()

add_fuzz_target(fuzz_cli fuzz_cli.cpp
    retdec-cli-parser
    retdec-bc-module
)

# The signature-lattice parser. fuzz_pe.cpp above drives retdec::fileformat,
# which publicly links LLVM; the lattice is the one translation unit under
# src/fileformat/ that does not, which is why scripts/standalone_fuzz.sh can
# also build this target with nothing but a compiler.
if(TARGET retdec::fileformat-lattice)
    add_fuzz_target(fuzz_lattice fuzz_lattice.cpp
        retdec::fileformat-lattice
    )
endif()
