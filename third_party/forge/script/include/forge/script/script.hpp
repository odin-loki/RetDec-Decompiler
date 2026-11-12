#pragma once
#include <stdexcept>
// forge::script — the FORGE managed scripting runtime (see docs/scripting.md).
// ARC + backup cycle collector, RAII handles, hybrid tree-walker / verified bytecode VM, written in safe
// ISO C++23 (no raw owning pointers, no unsafe escape hatch in the language).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge::script {

struct options {
    std::uint64_t max_steps = 0;        // 0 = unlimited
    std::uint64_t timeout_ms = 0;       // whole-run wall clock; 0 = unlimited
    std::uint64_t heap_bytes = 0;       // managed heap cap; 0 = forge::mem budget only
    int max_depth = 800;
    bool use_vm = true;                 // compile hot functions and whole scripts to bytecode
    bool debug = false;                 // race detector + lock-order / stall deadlock detector (slower)
    std::uint64_t deadlock_ms = 1000;   // debug: how long every thread may be blocked before DeadlockError
    int jit_threshold = 2;              // calls before a function is compiled
    bool allow_fs = true, allow_net = true, allow_proc = true, allow_env = true;
    // Static checks (forge/script/check.hpp) run after parsing, before execution. A script with a static
    // error is refused (exit 2, StaticCheckError). strict_check also refuses on warnings.
    bool static_check = true;
    bool strict_check = false;
    // OS-level enforcement of the capability flags (forge/os/sandbox.hpp: seccomp / Capsicum / pledge /
    // macOS sandbox_init / Windows job object). Applied once, process-wide and irreversibly, by the
    // engine constructor when any of fs/net/proc is off: !allow_net -> deny_network, !allow_proc ->
    // deny_exec, !allow_fs -> deny_fs_write (the OS layer blocks writes; reads stay blocked by the
    // interpreter only). off = interpreter checks only (embedders, rpc-serve, which must keep its own
    // sockets); best_effort (default) = apply, record any failure in sandbox_status(); require = throw if it fails.
    // ForgeScript treats this as a core guarantee: it is on by default whenever a capability is turned off.
    enum class os_sandbox_mode { off, best_effort, require };
    os_sandbox_mode os_sandbox = os_sandbox_mode::best_effort;
    // .fsc cache (docs/scripting.md ".fsc parse cache"): run_file() stores the parsed, resolved program under
    // <dir>/<BLAKE3 key>.fsc and reuses it while the source, file name and VM version are unchanged.
    // Empty dir = $FORGE_SCRIPT_CACHE, and if that is unset the cache is off. Unsigned cache files are trusted
    // only from a directory owned by the current user and not writable by group/other (POSIX); otherwise, or
    // with fsc_require_signed, only files signed by one of fsc_trusted_keys (Ed25519 public keys, hex) load.
    // fsc_sign_seed (32-byte Ed25519 seed, hex; or $FORGE_SCRIPT_CACHE_SIGN_SEED) signs the files written.
    std::string fsc_cache_dir;
    std::vector<std::string> fsc_trusted_keys;
    bool fsc_require_signed = false;
    std::string fsc_sign_seed;
    // SIGINT / SIGTERM (Windows: Ctrl-C, Ctrl-Break, console close) raise CancelledError in the running script
    // during top-level run()/run_file()/eval(); the previous handlers are restored afterwards. Embedders that
    // own those signals set this to false (engine::interrupt() still works).
    bool cancel_signals = true;
    std::vector<std::string> argv;      // visible to scripts as `args`
    std::function<void(std::string_view)> out;   // print(); default stdout
    // AI-assisted commands (ai.*). `ai` completes (system, prompt) -> text; null = use FORGE_AI_ENDPOINT
    // (OpenAI-compatible /v1/chat/completions over http) if set. `confirm` is shown AI-drafted code and
    // returns true to run it; null = AI code never runs (unless the script passes its own confirm callback).
    std::function<std::string(const std::string& system, const std::string& prompt)> ai;
    std::function<bool(const std::string& code)> confirm;
    // In-process model for the AI commands when `ai` is null: a .fpk path (empty = $FORGE_ASK_MODEL). Used only if
    // the host registered set_local_ai() (the forge CLI does); otherwise FORGE_AI_ENDPOINT is used. ask_max_tokens
    // caps each reply (0 = $FORGE_ASK_MAX_TOKENS, else 256).
    std::string ask_model;
    int ask_max_tokens = 0;
};

// In-process inference for forge::ask / forge::ask_json / ... (docs/scripting.md "AI-assisted commands").
// forge_script does not link the inference module; a host that does registers a generator once, e.g. with
// forge/script/local_ai.hpp's install_local_ai(). json = constrain the reply to JSON (ask_json).
struct local_ai_request { std::string model, system, prompt; bool json = false; int max_tokens = 256; };
using local_ai_fn = std::function<std::string(const local_ai_request&)>;
void set_local_ai(local_ai_fn fn);   // process-wide; null clears
bool has_local_ai();

struct result {
    bool ok = true;
    std::string value;                  // repr of the result (eval / REPL); empty for nil
    std::string error_type, error;      // e.g. "TypeError", "TypeError: ... \n  at f (x.fs:3)"
    int line = 0;
    int exit_code = 0;                  // int main()'s return value / std::exit(n); 1 on an uncaught error
};

class engine {
public:
    explicit engine(options o = {});
    ~engine();
    engine(const engine&) = delete; engine& operator=(const engine&) = delete;

    result run_file(const std::string& path);
    result run(std::string_view src, const std::string& name = "<string>");   // VM path if enabled
    result eval(std::string_view src);                                       // tree-walker, keeps globals (REPL)
    std::string disassemble(std::string_view src);                           // bytecode listing of a program
    std::string type_of(std::string_view expr);
    void interrupt();                                                          // async-safe: cancels the running script
    void reset();                                                              // drop all globals
    int repl();                                                                // interactive loop on stdin/stdout
    std::string stats();
    // What os_sandbox did: e.g. "seccomp", "pledge", "off", or "failed: <reason>".
    const std::string& sandbox_status() const;
    // What the .fsc cache did on the last run_file(): "off", "hit", "miss (stored): <reason>", "miss: <reason>".
    const std::string& cache_status() const;

    // ---- host bindings (embedding): native C++ functions callable from scripts, and script callbacks the host
    // can invoke later. Values cross as JSON text, like the plugin ABI (plugin.h). A script function passed as an
    // argument arrives as {"$callback": id}; keep the id and call it with call(id, args), release() it when done.
    // `name` may be qualified ("ui::find"); scripts call it as ui::find(...) (or ui.find after `auto ui = ...`).
    // A host function reports an error by throwing; the script sees HostError with what().
    using host_fn = std::function<std::string(const std::string& args_json)>;
    void define(const std::string& name, int min_args, int max_args, host_fn fn);
    result call(std::uint64_t callback, const std::string& args_json = "[]");   // re-entrant (safe inside a host fn)
    void release(std::uint64_t callback);
    std::size_t callbacks() const;                                             // live callback ids

    struct impl;
private:
    std::unique_ptr<impl> p_;
};

// Write once, emit anywhere (docs/scripting.md): translate a script in the portable subset into a
// standalone native script for `target` (a shell backend name/alias, "auto" = host default, or "ast").
// Throws emit_error with the offending line when the script uses something the target can't express.
struct emit_error : std::runtime_error { int line; emit_error(int l, const std::string& m) : std::runtime_error(m), line(l) {} };
std::string emit(std::string_view src, const std::string& file, const std::string& target);
std::vector<std::string> emit_targets();      // backends that have an emitter
std::string host_shell();                     // default shell for this OS ("pwsh"/"cmd" on Windows, "sh" elsewhere)

// Programs that use ForgeScript's ProcessPool in spawn mode (Windows, or spawn on POSIX) call this first in
// main(): returns true if this process was started as a pool worker and has finished serving.
bool run_process_worker_if_requested(int argc, char** argv);
} // namespace forge::script
