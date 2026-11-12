// Script front end: lexer + parser + resolver, then the bytecode compiler + verifier (disassemble).
// Nothing is executed, so hostile inputs cannot loop or touch the host. Expected failures are
// syntax_error (rejected input) and the compiler's own diagnostics; a crash or hang is a finding.
#include "fuzz.hpp"
#include <forge/script/script.hpp>
#include "parser.hpp"
#include <cstdio>
#include <cstdlib>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view src(reinterpret_cast<const char*>(data), size);
    try { (void)forge::script::parse_program(src, "<fuzz>"); }
    catch (const forge::script::syntax_error&) { return 0; }
    try { static forge::script::engine e; (void)e.disassemble(src); }
    catch (const forge::script::syntax_error&) {}
    catch (const std::runtime_error& x) {   // compile-time diagnostics are fine; compiler output failing the verifier is a bug
        if (std::string_view(x.what()).starts_with("bytecode verify failed")) { std::fprintf(stderr, "%s\n", x.what()); std::abort(); }
    }
    return 0;
}
