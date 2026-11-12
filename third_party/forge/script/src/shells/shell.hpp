#pragma once
// Pluggable shell backends for forge::sh-style calls (docs/shells.md).
// Adding a shell = one file in script/src/shells/ that defines
//     void forge_shell_register_<file-stem>(forge::script::shell_registry& r)
// CMake globs the directory and generates the call list, so nothing else needs editing.
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace forge::script {

struct emit_dialect;
struct shell_backend {
    std::string name;                         // forge::<name>(...) and sh { } style kind
    std::vector<std::string> aliases;         // other kinds that map here (e.g. "ps" -> pwsh)
    std::vector<std::string> exes;            // executables to look for on PATH, in order
    // argv to run `script` with the found executable
    std::function<std::vector<std::string>(const std::string& exe, const std::string& script)> argv;
    std::function<std::string(const std::string&)> quote;   // one literal argument, for {} interpolation
    std::string family;                       // posix, csh, fish, nu, python, elvish, powershell, cmd, rc
    bool golden_tested = false;               // true = covered by tests/test_shells.cpp on CI boxes that have it
    std::string notes;
    // Emitter for `forge script --emit <name>` (null = this shell has no emitter yet).
    std::function<std::unique_ptr<emit_dialect>()> emitter;
    std::string emit_ext = ".sh";             // file extension for emitted scripts
    bool emit_tested = false;                 // emitted output golden-tested against the interpreter
};

class shell_registry {
public:
    void add(shell_backend b) { v_.push_back(std::move(b)); }
    const shell_backend* find(const std::string& kind) const {
        for (auto& b : v_) { if (b.name == kind) return &b; for (auto& a : b.aliases) if (a == kind) return &b; }
        return nullptr;
    }
    const std::vector<shell_backend>& all() const { return v_; }
private:
    std::vector<shell_backend> v_;
};
const shell_registry& shells();     // all registered backends (built once)
// The executable to run for a backend ("" if none is installed). On Windows this is a full path and skips
// the WSL bash relays (System32 and WindowsApps bash.exe), which run Linux bash and cannot take Windows paths or quoting.
std::string shell_exe(const shell_backend& b);

namespace shellq {
inline std::string posix(const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; }
inline std::string doubled(const std::string& s, char q) { std::string o(1, q); for (char c : s) { o += c; if (c == q) o += q; } return o + q; }
inline std::vector<std::string> dash_c(const std::string& exe, const std::string& s) { return {exe, "-c", s}; }
} // namespace shellq
} // namespace forge::script
