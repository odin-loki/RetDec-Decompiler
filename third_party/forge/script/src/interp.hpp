#pragma once
// forge::script interpreter state, shared runtime operations, and the AST tree-walker.
#include "ast.hpp"
#include "objects.hpp"
#include <cstdio>
#include <exception>
#include <string_view>

namespace forge::script {

struct Limits {
    std::uint64_t max_steps = 0;      // statements + instructions; 0 = unlimited
    std::uint64_t timeout_ms = 0;     // wall clock for the whole run
    std::uint64_t heap_bytes = 0;     // managed heap cap (on top of the forge::mem budget)
    int max_depth = 800;              // script call depth (keeps the C++ stack safe)
    std::uint64_t max_output = 64ull << 20;   // per child-process stream
};
struct Caps { bool fs = true, net = true, proc = true, env = true, reflect = true; };   // reflect: reflect::set/invoke/call/construct
// FSS CAP-1 scope (docs/security-api.md): built-ins pass the path / host:port / program they act on to
// enforce_scope(), which checks it with forge::security::try_enforce against the process's active policy
// (security::lockdown / set_active_policy) and raises PermissionError when the policy denies it (audited). The Caps
// flags are still checked first. No active policy (profile none): enforce_scope does nothing.
enum class SecScope { fs_read, fs_write, net_connect, net_listen, exec, env, plugin_load };
struct Interp;
void enforce_scope(Interp& in, SecScope s, std::string_view target);

// A script-level exception travelling through C++ frames. `v` is the thrown value (usually an Error object).
struct ScriptError : std::exception {
    Value v; std::string msg;
    ScriptError(Value x, std::string m) : v(std::move(x)), msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

enum class Flow { Normal, Break, Continue, Return };

// Read-mostly state shared by every interpreter (task) of one engine.
struct Shared {
    std::unordered_map<std::string, Value> builtins;           // frozen natives and modules
    std::unordered_map<std::string, Ref<Class>> errors;        // Error hierarchy by name
    Limits limits; Caps caps;
    std::function<void(std::string_view)> out = [](std::string_view s) { std::fwrite(s.data(), 1, s.size(), stdout); };
    std::function<bool(const std::string&)> confirm;           // y/N prompt for ai.run; null = never confirm
    std::function<std::string(const std::string&, const std::string&)> ai;   // (system, prompt) -> completion
    std::string ask_model; int ask_max_tokens = 0;             // in-process model for the AI commands (ai.cpp)
    std::atomic<bool> interrupt{false};
    std::atomic<std::uint64_t> steps{0};
    std::uint64_t start_ns = 0;
    std::vector<std::string> argv;
    std::string base_dir;                                      // for import resolution
    bool use_vm = true; int jit_threshold = 2;
    std::mutex mod_m; std::unordered_map<std::string, Ref<Globals>> modules;
    std::function<Value(Interp&, const std::string&)> load_plugin;
    std::unordered_map<std::string, Value> plugins;                  // path -> frozen map of natives   // native plugin loader (builtins.cpp)
    std::mutex trace_m; bool trace_calls = false;
    std::vector<std::shared_ptr<const FnDecl>> keep;           // programs whose AST must outlive globals
    std::unordered_map<std::string, std::unordered_map<std::string, Value>> methods;   // built-in methods by receiver type
    std::unordered_map<std::string, Ref<Class>> classes;       // built-in classes (ProcResult, ...)
    std::mutex cb_m; std::uint64_t cb_next = 1;                // engine::define / call: script callbacks held by the host
    std::unordered_map<std::uint64_t, Value> callbacks;
};

struct Chunk;

struct Interp {
    std::shared_ptr<Shared> sh;
    Ref<Globals> globals;
    int depth = 0, line = 0;
    std::string file;
    Value ret;                                                 // return value in flight
    std::vector<std::pair<std::string, int>> frames;           // name, call line (stack traces)
    std::shared_ptr<TaskState> task;                           // cancellation when running as a task
    Caps caps;                                                 // narrowed inside sandbox(...) blocks
    std::uint64_t child_mem = 0, child_cpu_s = 0, child_timeout_ms = 0;   // applied to spawned processes
    std::uint64_t deadline_ns = 0;                             // limit({timeout}) / Limits::timeout_ms
    std::uint64_t cpu_deadline_ns = 0;                         // limit({cpu_s}): this thread's CPU time (os::thread_cpu_time_ns)
    std::shared_ptr<Quota> handle_quota, proc_quota;           // limit({handles}) / limit({procs}); shared with tasks started inside
    void charge_process(const std::string& what);              // count one child process against limit({procs})

    explicit Interp(std::shared_ptr<Shared> s, Ref<Globals> g = {}) : sh(std::move(s)), globals(g ? std::move(g) : make<Globals>()), caps(sh->caps) {}

    // ---- entry points
    Value run_top(const std::shared_ptr<const FnDecl>& top, bool vm);
    Value call(const Value& f, std::vector<Value>& args, int call_line = 0);
    Value call_function(const Ref<Function>& f, std::vector<Value>& args, const Value* self);

    // ---- tree-walker
    Flow exec(const Node& n, const Ref<Env>& env);
    Flow exec_block(const Node& n, const Ref<Env>& env);
    Value eval(const Node& n, const Ref<Env>& env);

    // ---- shared runtime (used by both the tree-walker and the VM)
    void step();
    void run_finalizers();
    // Non-const reference parameters: call_function leaves (argument index, final value) here for the caller.
    const Function* ref_back_fn = nullptr;
    std::vector<std::pair<std::size_t, Value>> ref_back;
    Value call_writeback(const Node& n, const Ref<Env>& env);
    // Call f(args...) and copy the final values of its non-const T& parameters back into args.
    Value call_ref(const Value& f, std::vector<Value>& args);
    void iter_writeback(Obj& it, const Value& a, const Value* b);
    bool in_finalizers_ = false;
    Value load(const VarRef& v, const Ref<Env>& env);
    void store(const VarRef& v, const Ref<Env>& env, Value x, bool define = false, bool is_const = false);
    static bool truthy(const Value& v) {
        switch (v.index()) { case 0: return false; case 1: return std::get<bool>(v); case 2: return std::get<std::int64_t>(v) != 0; case 3: return std::get<double>(v) != 0.0; }
        return !is_nil(v);
    }
    Value binary(Op op, const Value& a, const Value& b);
    Value unary(const std::string& op, const Value& a);
    bool equal(const Value& a, const Value& b, int depth = 0);
    Value get_field(const Value& o, const std::string& name, bool optional = false);
    void set_field(const Value& o, const std::string& name, Value v);
    Value get_index(const Value& o, const Value& i);
    void set_index(const Value& o, const Value& i, Value v);
    Value make_closure(const std::shared_ptr<const FnDecl>& fd, const Ref<Env>& env);
    Value define_class(const ClassDecl& cd, const Ref<Env>& env);
    Value instantiate(const Ref<Class>& c, std::vector<Value>& args);
    Value interpolate(const std::vector<Value>& parts);
    Value shell(const Node& n, const std::vector<Value>& parts);
    Value proc_result(const std::string& cmd, int code, std::string out, std::string err, std::uint64_t ms, bool timed_out);
    Value spawn(const Value& f, std::vector<Value> args);
    Value await(const Value& t);
    Value import_module(const std::string& path);
    bool conforms(const Value& v, const TypeAnn& t);
    void check_type(const Value& v, const TypeAnn& t, const std::string& what);
    Value share(const Value& v);                 // struct values are copied on assignment
    Value copy_value(const Value& v, int depth = 0);   // C++ copy semantics (vector/map/string/struct deep copy)
    Value move_from(const Node& lvalue, const Ref<Env>& env);
    Value load_qualified(const std::string& name);
    Value run_main();
    void stream_write(Ostream& os, const Value& v);
    void sort_keys(std::vector<Value>& keys);
    int compare(const Value& a, const Value& b);   // <0, 0, >0 (std::less-like total order)
    Value call_operator(const Value& self, const std::string& op, std::vector<Value> args, bool& found);
    Flow run_try(const Node& n, const Ref<Env>& env);
    Flow run_using(const Node& n, const Ref<Env>& env);
    void dispose(const Value& v);
    Ref<Obj> iterate(const Value& v);            // iterator object
    bool iter_next(Obj& it, Value& out, Value* out2);

    // ---- strings and errors
    std::string str(const Value& v, int depth = 0);
    std::string repr(const Value& v, int depth = 0);
    static std::string type_of(const Value& v);
    Value new_string(std::string s) { return obj(make<String>(std::move(s))); }
    Value make_error(const std::string& type, const std::string& msg);
    [[noreturn]] void raise(const std::string& type, const std::string& msg);
    ScriptError from_cpp(std::exception_ptr e);  // map any C++ exception to a script exception
    std::string trace_text() const;
    std::string error_message(const Value& err);
};

// Deep copy for crossing task boundaries; handles are refused, channels and frozen strings are shared.
Value deep_copy(Interp& in, const Value& v, std::unordered_map<const Obj*, Value>& memo);

struct NodeIter;   // runtime iterator

void install_builtins(Shared& sh);
// engine::define: a native that converts its arguments to JSON (script callables become {"$callback": id}).
Value host_native(Shared& sh, const std::string& name, int lo, int hi, std::function<std::string(const std::string&)> fn);
Value host_args(const std::string& json_array);              // JSON array -> argument list (List)
void install_cpp_std(Shared& sh);
void install_automation(Shared& sh);
void install_mobile(Shared& sh);
void install_text(Shared& sh);
void install_reflect(Shared& sh);
void install_ai(Shared& sh);
void install_remote(Shared& sh);
std::vector<std::string> module_search_path(const Interp& in);
bool is_native_module(const std::string& p);
std::string cpp_format(Interp& in, const std::string& fmt, const std::vector<Value>& args, std::size_t first);
std::vector<std::string>& script_argv();   // forge::args()

namespace debug {
bool on();
void check_write(Interp& in, const Obj* o);          // RaceError if o belongs to another thread
void adopt(const Value& v);                            // receiver takes ownership of a transferred graph
void lock_acquire(Interp& in, std::uint64_t lock);       // lock-order graph; DeadlockError on inversion
void lock_release(std::uint64_t lock) noexcept;
void thread_started() noexcept;
void thread_finished() noexcept;
inline std::uint64_t stall_ms = 1000;
struct Blocked {                                        // RAII: this thread is waiting on another one
    Interp& in; const char* what;
    Blocked(Interp& i, const char* w);
    ~Blocked();
    Blocked(const Blocked&) = delete; Blocked& operator=(const Blocked&) = delete;
    void poll();                                        // DeadlockError when every thread is blocked
};
}

} // namespace forge::script
