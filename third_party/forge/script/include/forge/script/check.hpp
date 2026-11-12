#pragma once
// forge::script static checker (docs/scripting.md "Static checks"). Runs over the parsed and resolved AST,
// before execution. Errors are sound: an error means the flagged operation raises (or is refused) whenever it is
// reached. Anything the checker cannot prove is a warning. The runtime checks stay in place as the backstop.
#include <string>
#include <string_view>
#include <vector>

namespace forge::script {

struct diagnostic {
    enum class level { warning, error };
    level severity = level::warning;
    std::string id;        // stable check id, e.g. "use-after-move"; used by suppression comments
    std::string message;
    std::string file;
    int line = 0, col = 0; // 1-based; col 0 when unknown
};

struct check_options {
    bool allow_fs = true, allow_net = true, allow_proc = true, allow_env = true;   // the run's declared capabilities
    bool warnings = true;  // false: return errors only
    std::vector<std::string> module_dirs;   // extra import search dirs, searched after the script dir (the engine passes its base_dir)
};

// Parse, resolve and check. A syntax error is returned as a single "syntax" error diagnostic.
std::vector<diagnostic> check(std::string_view src, const std::string& file, const check_options& o = {});
// Checks over an already parsed program (used by engine::exec).
struct FnDecl;
std::vector<diagnostic> check_parsed(const FnDecl& top, std::string_view src, const std::string& file, const check_options& o = {});
std::string to_text(const std::vector<diagnostic>& d);   // file:line:col: error[id]: message
std::string to_json(const std::vector<diagnostic>& d);   // [{"file","line","col","severity","id","message"}]
bool has_errors(const std::vector<diagnostic>& d);

} // namespace forge::script
