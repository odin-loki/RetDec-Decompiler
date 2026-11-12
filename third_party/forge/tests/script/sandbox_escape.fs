// Escape tests for the capability checks: inside forge::sandbox({fs/net/proc/env: false}) every indirect route to a
// denied operation must still raise PermissionError (SandboxViolationError is a subclass, so it is caught too).
#include <functional>

std::string path = forge::fs::temp_file("forge-escape-");
forge::fs::write(path, "secret");

void attempt(const std::string& route, std::function<void()> f) {
    try { f(); std::println("ESCAPED: {}", route); }
    catch (const PermissionError& e) { std::println("denied: {}", route); }
}
const std::map<std::string, bool> no_fs{{"fs", false}}, no_net{{"net", false}}, no_proc{{"proc", false}}, no_env{{"env", false}};

// values that are prepared outside the sandbox and used inside it
auto read_alias = forge::fs::read;
std::function<std::string(std::string)> read_fn = forge::fs::read;
auto read_lambda = [] { return forge::fs::read(path); };
std::map<std::string, std::function<std::string(std::string)>> table{{"read", forge::fs::read}};
struct Reader { std::string p; std::string get() const { return forge::fs::read(p); } };
Reader reader{path};
auto open_before = forge::fs::open(path);
auto sh_alias = forge::sh;
auto run_alias = forge::run;

// ---- fs
forge::sandbox(no_fs, [] {
    attempt("fs direct call", [] { forge::fs::read(path); });
    attempt("fs alias taken outside the sandbox", [] { read_alias(path); });
    attempt("fs alias taken inside the sandbox", [] { auto w = forge::fs::write; w(path, "x"); });
    attempt("fs through std::function", [] { read_fn(path); });
    attempt("fs lambda defined outside", [] { read_lambda(); });
    attempt("fs function stored in a map", [] { table["read"](path); });
    attempt("fs method of an object made outside", [] { reader.get(); });
    attempt("fs higher-order std::transform", [] {
        std::vector<std::string> in{path}, out(1);
        std::transform(in.begin(), in.end(), out.begin(), forge::fs::read);
    });
    attempt("fs higher-order std::for_each", [] { std::vector<std::string> ps{path}; std::for_each(ps.begin(), ps.end(), forge::fs::remove); });
    attempt("fs through eval", [] { eval("forge::fs::read(path);"); });
    attempt("fs eval defining a function", [] { eval("auto f() { return forge::fs::exists(path) ? forge::fs::read(path) : std::string(); } f();"); });
    attempt("fs through reflect::call", [] { forge::reflect::call("forge::fs::read", path); });
    attempt("fs handle opened outside: read", [] { open_before.read(); });
    attempt("fs handle opened outside: write", [] { open_before.write("x"); });
    attempt("fs import", [] { eval("import \"no_such_module.fs\";"); });
    attempt("fs in a thread", [] { std::async([] { return forge::fs::read(path); }).get(); });
    attempt("fs in std::thread", [] { std::thread t([] { forge::fs::write(path, "x"); }); t.join(); });
    attempt("fs in forge::parallel", [] { forge::parallel(std::vector<std::string>{path}, [](std::string p) { return forge::fs::read(p); }); });
    attempt("fs nested sandbox re-enabling fs", [] { forge::sandbox(std::map<std::string, bool>{{"fs", true}}, [] { forge::fs::read(path); }); });
    attempt("fs limit block inside the sandbox", [] { limit {.timeout = "5s"} { forge::fs::read(path); } });
    attempt("fs glob", [] { forge::fs::glob("*"); });
});

// ---- proc
forge::sandbox(no_proc, [] {
    attempt("proc shell call", [] { forge::sh("echo hi"); });
    attempt("proc shell alias", [] { sh_alias("echo hi"); });
    attempt("proc bash", [] { forge::bash("echo hi"); });
    attempt("proc host shell", [] { forge::shell("echo hi"); });
    attempt("proc run alias", [] { run_alias(std::vector<std::string>{"echo", "hi"}); });
    attempt("proc proc::start", [] { forge::proc::start(std::vector<std::string>{"echo", "hi"}); });
    attempt("proc through eval", [] { eval("forge::sh(\"echo hi\");"); });
    attempt("proc through reflect::call", [] { forge::reflect::call("forge::sh", "echo hi"); });
    attempt("proc in a thread", [] { std::async([] { return forge::sh("echo hi").out; }).get(); });
    attempt("proc ProcessPool asking for proc", [] {
        auto pp = forge::ProcessPool(1, std::map<std::string, bool>{{"proc", true}});
        pp.submit("auto task(auto x) { return forge::sh(\"echo hi\").out; }", 1).get();
    });
});

// ---- net
forge::sandbox(no_net, [] {
    attempt("net resolve", [] { forge::net::resolve("localhost"); });
    attempt("net alias", [] { auto r = forge::net::resolve; r("localhost"); });
    attempt("net port_open", [] { forge::net::port_open("127.0.0.1", 9); });
    attempt("net through eval", [] { eval("forge::net::resolve(\"localhost\");"); });
    attempt("net remote script", [] { forge::remote("127.0.0.1:9", "1;"); });
});

// ---- env
forge::sandbox(no_env, [] {
    attempt("env os::env", [] { forge::os::env("HOME"); });
    attempt("env std::getenv", [] { std::getenv("HOME"); });
    attempt("env setenv", [] { forge::os::setenv("FORGE_ESCAPE", "1"); });
});

// capabilities come back after the sandbox
open_before.close();
std::println("after: {}", forge::fs::read(path));
forge::fs::remove(path);
