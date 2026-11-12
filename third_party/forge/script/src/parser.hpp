#pragma once
#include "ast.hpp"
#include <stdexcept>
#include <string_view>

namespace forge::script {

struct syntax_error : std::runtime_error {
    int line; bool incomplete;   // incomplete = input ended early (the REPL asks for more lines)
    syntax_error(int l, const std::string& m, bool inc = false) : std::runtime_error(m), line(l), incomplete(inc) {}
};

// Parse and resolve a whole program. Top-level `let`/`fn`/`class` become globals.
std::shared_ptr<const FnDecl> parse_program(std::string_view src, const std::string& file);

} // namespace forge::script
