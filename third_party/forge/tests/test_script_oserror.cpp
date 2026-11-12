// forge/os/oserror.hpp: errno / Win32 classification, typed exceptions, fault signals -> exceptions,
// and the ForgeScript exception classes they map to.
#include <forge/os/oserror.hpp>
#include <forge/script/script.hpp>
#include "check.hpp"
#include <utility>
#include <string>
#include <atomic>
#include <chrono>
#if !defined(_WIN32)
#include <unistd.h>
#include <csignal>
#endif
using namespace forge;
TEST_PROCESS_WORKER(script::run_process_worker_if_requested);

TEST(classify_errno_basic) {
#if !defined(_WIN32)
    CHECK(os::classify_errno(ENOENT) == os::os_errc::not_found);
    CHECK(os::classify_errno(EEXIST) == os::os_errc::already_exists);
    CHECK(os::classify_errno(EACCES) == os::os_errc::permission_denied);
    CHECK(os::classify_errno(ECONNREFUSED) == os::os_errc::connection_refused);
    os::note_sandboxed(true);
    CHECK(os::classify_errno(EPERM) == os::os_errc::sandbox_violation);
    os::note_sandboxed(false);
#endif
    CHECK(std::string(os::script_error_type(os::os_errc::not_found)) == "FileNotFoundError");
    CHECK(std::string(os::script_error_type(os::os_errc::sandbox_violation)) == "SandboxViolationError");
}

TEST(throw_if_typed) {
    bool caught = false;
    try { os::throw_if(os::read_file("/nonexistent/forge/oserror/probe")); }
    catch (const os::os_exception& e) { caught = true; CHECK(e.kind == os::os_errc::not_found); }
    CHECK(caught);
}

TEST(fault_guard_recovers_segv) {
    bool caught = false;
    // Volatile pointer to volatile: the optimiser can neither prove the store faults (and isolate the path)
    // nor drop it as a dead store.
    try { os::fault_guard([] { volatile int* volatile p = nullptr; *p = 1; }); }
    catch (const os::os_exception& e) { caught = true; CHECK(e.kind == os::os_errc::signal); }
    CHECK(caught);
    CHECK(os::fault_guard([] { return 7; }) == 7);   // still usable afterwards
}

// "is-a" in ForgeScript's C++ syntax: does `catch (const Base&)` catch a thrown Derived?
static std::string isa(const char* derived, const char* base) {
    return std::string("bool ok = false; try { throw ") + derived + "(\"x\"); } catch (const " + base + "&) { ok = true; } catch (...) {} ok";
}

TEST(script_catches_typed_os_errors) {
    script::engine e; std::string out;
    auto r = e.eval("std::string got = \"no\"; try { fs.read(\"/nonexistent/forge/oserror/probe\"); } catch (const FileNotFoundError& err) { got = \"yes\"; } got");
    CHECK_MSG(r.ok && r.value == "\"yes\"", "%s %s", r.value.c_str(), r.error.c_str());
    auto r2 = e.eval(isa("FileNotFoundError", "OSError"));
    CHECK_MSG(r2.ok && r2.value == "true", "%s %s", r2.value.c_str(), r2.error.c_str());
    auto r3 = e.eval(isa("SandboxViolationError", "PermissionError"));
    CHECK_MSG(r3.ok && r3.value == "true", "%s %s", r3.value.c_str(), r3.error.c_str());
}

TEST(classify_errno_extended) {
#if !defined(_WIN32)
    CHECK(os::classify_errno(EROFS) == os::os_errc::read_only);
    CHECK(os::classify_errno(ENOTEMPTY) == os::os_errc::not_empty);
    CHECK(os::classify_errno(ESRCH) == os::os_errc::no_such_process);
    CHECK(os::classify_errno(EPIPE) == os::os_errc::broken_pipe);
    CHECK(std::string(os::script_error_type(os::os_errc::broken_pipe)) == "BrokenPipeError");
#endif
}

TEST(classify_ntstatus_and_mach) {
    CHECK(os::classify_ntstatus(0xC0000034u) == os::os_errc::not_found);
    CHECK(os::classify_ntstatus(0xC0000005u) == os::os_errc::signal);
    CHECK(os::classify_ntstatus(0xC000013Au) == os::os_errc::cancelled);
    CHECK(os::classify_mach_exception(1) == os::os_errc::signal);
    CHECK(os::classify_mach_exception(12) == os::os_errc::sandbox_violation);
}

#if !defined(_WIN32)
TEST(fault_guard_sigfpe_and_sigpipe_ignored) {
    bool caught = false;
    try { os::fault_guard([] { ::raise(SIGFPE); }); }
    catch (const os::os_exception& e) { caught = true; CHECK(e.signo == SIGFPE); }
    CHECK(caught);
    int fd[2]; CHECK(::pipe(fd) == 0); ::close(fd[0]);
    errno = 0; auto n = ::write(fd[1], "x", 1); CHECK(n < 0 && errno == EPIPE); ::close(fd[1]);   // SIGPIPE ignored
}

TEST(cancel_signal_sets_flag) {
    std::atomic<bool> f{false};
    os::install_cancel_signals(&f); ::raise(SIGTERM); CHECK(f.load());
    CHECK(os::take_cancel_signal() == SIGTERM);
    os::install_cancel_signals(nullptr);
}
#endif

TEST(script_os_hierarchy_extended) {
    script::engine e;
    for (auto [d, b] : {std::pair{"BrokenPipeError", "ConnectionError"}, {"ConnectionError", "OSError"},
                        {"ReadOnlyFileSystemError", "OSError"}, {"ProcessLookupError", "OSError"}}) {
        auto r = e.eval(isa(d, b)); CHECK_MSG(r.ok && r.value == "true", "%s is %s -> %s %s", d, b, r.value.c_str(), r.error.c_str());
    }
}

TEST(sandbox_label_matches_policy) {
#if !defined(_WIN32)
    os::note_sandbox_policy(/*net*/true, /*exec*/false, /*fs_write*/false);
    { os::os_op_scope s(os::os_op::net); CHECK(os::classify_errno(EPERM) == os::os_errc::sandbox_violation); }
    { os::os_op_scope s(os::os_op::exec); CHECK(os::classify_errno(EPERM) == os::os_errc::permission_denied); }
    { os::os_op_scope s(os::os_op::net); CHECK(os::classify_errno(EACCES) == os::os_errc::permission_denied); }
    CHECK(os::classify_errno(EPERM) == os::os_errc::permission_denied);   // os_op::any never matches a known policy
    os::note_sandboxed(false);
#endif
}

TEST(script_python_style_hierarchy) {
    script::engine e;
    for (const char* d : {"PermissionError", "TimeoutError", "OutOfMemoryError", "CancelledError", "DeadlockError", "SandboxViolationError"}) {
        auto r = e.eval(isa(d, "OSError")); CHECK_MSG(r.ok && r.value == "true", "%s is OSError -> %s %s", d, r.value.c_str(), r.error.c_str());
    }
}

TEST(engine_cancel_signals_option) {
    script::options o; o.cancel_signals = false; script::engine e(o);
    auto r = e.run("1 + 1;"); CHECK(r.ok);
}

#if !defined(FORGE_OS_IOS)
TEST(script_process_pool) {
    script::engine e;
    auto r = e.eval("auto pp = ProcessPool(2); auto f = pp.submit(\"auto task(auto x) { return x * x; }\", 7); auto v = f.get(); pp.shutdown(); v");
    CHECK_MSG(r.ok && r.value == "49", "%s %s", r.value.c_str(), r.error.c_str());
    auto r2 = e.eval("auto p2 = ProcessPool(1); std::vector<int> xs = {1, 2, 3}; auto m = p2.map(\"auto task(auto x) { return x + 1; }\", xs); p2.shutdown(); m");
    CHECK_MSG(r2.ok, "%s", r2.error.c_str());
    // error type crosses the process boundary; worker has no fs by default
    auto r3 = e.eval("auto p3 = ProcessPool(1); auto t = \"\"; try { p3.submit(\"auto task(auto x) { return fs.read(\\\"/etc/hostname\\\"); }\", 0).get(); } catch (const PermissionError& err) { t = \"perm\"; } p3.shutdown(); t");
    CHECK_MSG(r3.ok && r3.value.find("perm") != std::string::npos, "%s %s", r3.value.c_str(), r3.error.c_str());
}
// the script's time limit also bounds closing the pool: a worker still busy then is killed, not awaited
TEST(script_process_pool_time_limit_kills_workers) {
    script::options o; o.timeout_ms = 1500; o.out = [](std::string_view) {}; script::engine e(o);
    auto t0 = std::chrono::steady_clock::now();
    auto r = e.run("auto pp = ProcessPool(1); pp.submit(\"auto task(auto x) { while (true) {} return x; }\", 0).get();", "<pool-timeout>");
    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_MSG(!r.ok, "spinning pool task returned");
    CHECK_MSG(secs < 20, "run took %.1f s", secs);
}
#endif
