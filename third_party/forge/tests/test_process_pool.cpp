// forge::os::process_pool: worker processes, IPC, crash detection, shutdown, sandbox.
#include <forge/os/process_pool.hpp>
#include "check.hpp"
#include <fstream>
#include <iterator>
using namespace forge;

FORGE_PROCESS_TASK(square, [](std::string in) { long v = std::stol(in); return std::to_string(v * v); });
FORGE_PROCESS_TASK(echo, [](std::string in) { return in; });
FORGE_PROCESS_TASK(fail, [](std::string) -> std::string { throw os::os_exception(os::os_errc::not_found, 2, "nope"); });
FORGE_PROCESS_TASK(hang, [](std::string) { std::this_thread::sleep_for(std::chrono::seconds(120)); return std::string(); });
// A task that writes to stdout and tries to read stdin: neither may touch the IPC stream.
// (The #if lives outside the macro: MSVC rejects directives inside macro arguments.)
static std::size_t chatty_stdin_bytes() {
#if defined(_WIN32)   // the worker's stdin was the IPC pipe: it must now read as NUL (EOF at once)
    char b[16]; return std::fread(b, 1, sizeof b, stdin);
#else
    return 0;
#endif
}
FORGE_PROCESS_TASK(chatty, [](std::string in) {
    std::printf("noise from printf\n"); std::fflush(stdout);
    std::fputs("more noise\n", stdout); std::fflush(stdout);
    return in + ":" + std::to_string(chatty_stdin_bytes());
});
FORGE_PROCESS_TASK(sb_mech, [](std::string) { return os::sandbox_status().mechanism; });
// "connected" / "refused" for a TCP connect to 127.0.0.1:<port>; "wrote" / "denied" for a file write.
FORGE_PROCESS_TASK(probe_connect, [](std::string port) { return os::socket::connect("127.0.0.1", std::stoi(port), 3000) ? std::string("connected") : std::string("refused"); });
FORGE_PROCESS_TASK(probe_write, [](std::string path) { return os::write_file(path, "x") ? std::string("wrote") : std::string("denied"); });
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
FORGE_PROCESS_TASK(segv, [](std::string) -> std::string { ::raise(SIGSEGV); return {}; });
FORGE_PROCESS_TASK(pid, [](std::string) { return std::to_string(::getpid()); });
FORGE_PROCESS_TASK(try_write, [](std::string path) -> std::string {
    int f = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (f < 0) os::throw_if(os::last_error("open"));
    ::close(f); return "wrote";
});
#endif

#if defined(__linux__)
// Spawn-mode workers re-exec this test binary; serve before check.hpp's main() runs.
static const bool worker_hook = [] {
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    std::vector<std::string> parts; std::size_t b = 0;
    for (std::size_t i = 0; i < s.size(); ++i) if (!s[i]) { parts.push_back(s.substr(b, i - b)); b = i + 1; }
    std::vector<char*> av; for (auto& p : parts) av.push_back(p.data());
    return os::process_pool::run_worker_if_requested(int(av.size()), av.data());
}();
#endif

#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
TEST(pp_basic_fork) {
    process_pool pp({.workers = 3});
    std::vector<std::future<std::string>> fs;
    for (int i = 0; i < 50; ++i) fs.push_back(pp.submit("square", std::to_string(i)));
    pp.wait_all();
    for (int i = 0; i < 50; ++i) CHECK(fs[std::size_t(i)].get() == std::to_string(i * i));
    CHECK(pp.size() == 3);
}
TEST(pp_runs_in_other_process) {
    process_pool pp({.workers = 1});
    CHECK(pp.submit("pid", "").get() != std::to_string(::getpid()));
}
TEST(pp_large_binary_payload) {
    process_pool pp({.workers = 2});
    std::string big(3 << 20, '\0'); for (std::size_t i = 0; i < big.size(); ++i) big[i] = char(i * 131);
    CHECK(pp.submit("echo", big).get() == big);
}
TEST(pp_task_exception_typed) {
    process_pool pp({.workers = 1});
    auto f = pp.submit("fail", "");
    try { f.get(); CHECK(false); } catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::not_found); CHECK(std::string(e.what()) == "nope"); }
    try { pp.submit("no_such_task", "").get(); CHECK(false); } catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::invalid_argument); }
}
TEST(pp_crash_raises_signal_and_respawns) {
    process_pool pp({.workers = 1});
    try { pp.submit("segv", "").get(); CHECK(false); }
    catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::signal); CHECK(e.signo == SIGSEGV); CHECK(std::string(e.script_type()) == "SignalError"); }
    CHECK(pp.submit("square", "9").get() == "81");
}
TEST(pp_set_workers_grow_shrink) {
    process_pool pp({.workers = 1});
    pp.set_workers(4); CHECK(pp.size() == 4);
    std::vector<std::future<std::string>> fs; for (int i = 0; i < 20; ++i) fs.push_back(pp.submit("square", "3"));
    pp.set_workers(2); CHECK(pp.size() == 2);
    for (auto& f : fs) CHECK(f.get() == "9");
}
TEST(pp_shutdown_drains_and_rejects) {
    process_pool pp({.workers = 2});
    auto f = pp.submit("square", "5");
    pp.shutdown(); pp.shutdown();
    CHECK(f.get() == "25");
    CHECK_THROWS(pp.submit("square", "1").get());
}
#if defined(__linux__)
TEST(pp_spawn_mode_exec) {
    process_pool pp({.workers = 2, .mode = process_pool::spawn_mode::spawn});
    CHECK(pp.submit("square", "12").get() == "144");
}
TEST(pp_spawn_bad_exe_fails_cleanly) {
    process_pool pp({.workers = 1, .mode = process_pool::spawn_mode::spawn, .exe = "/nonexistent/forge-worker"});
    try { pp.submit("square", "1").get(); CHECK(false); } catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::io || e.kind == os::os_errc::broken_pipe); }
}
TEST(pp_spawn_mode_task_stdio) {
    process_pool pp({.workers = 1, .mode = process_pool::spawn_mode::spawn});
    CHECK(pp.submit("chatty", "s").get().rfind("s:", 0) == 0);
}
TEST(pp_sandboxed_worker_denies_write) {
    os::sandbox_policy p; p.deny_fs_write = true;
    process_pool pp({.workers = 1, .sandbox = p});
    try { pp.submit("try_write", "/tmp/forge_pp_sandbox_probe").get(); CHECK(false); }
    catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::sandbox_violation || e.kind == os::os_errc::permission_denied); }
    // seccomp always; Landlock on top where the kernel has it
    std::string mech = pp.submit("sb_mech", "").get(); std::printf("  worker sandbox: %s\n", mech.c_str());
    CHECK(mech.rfind("seccomp-bpf", 0) == 0);
    CHECK((mech.find("landlock") != std::string::npos) == (os::sandbox_status().landlock_abi > 0));
    CHECK(pp.submit("square", "6").get() == "36");
}
#endif
#endif

#if defined(FORGE_OS_WINDOWS)
static const bool worker_hook_win = os::process_pool::run_worker_if_requested(__argc, __argv);
TEST(pp_windows_spawn) {
    process_pool pp({.workers = 2});   // spawn is the default (and only) mode on Windows
    std::vector<std::future<std::string>> fs;
    for (int i = 0; i < 10; ++i) fs.push_back(pp.submit("square", std::to_string(i)));
    for (int i = 0; i < 10; ++i) CHECK(fs[std::size_t(i)].get() == std::to_string(i * i));
    try { (void)pp.submit("fail", "").get(); CHECK(false); } catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::not_found); }
}
TEST(pp_windows_task_stdio_does_not_corrupt_ipc) {
    process_pool pp({.workers = 1});
    for (int i = 0; i < 5; ++i) CHECK(pp.submit("chatty", "p" + std::to_string(i)).get() == "p" + std::to_string(i) + ":0");
    CHECK(pp.submit("square", "11").get() == "121");
}
TEST(pp_windows_sandbox_appcontainer) {
    // Network / file-write denial: workers run in an AppContainer. Loopback is blocked for AppContainers (no
    // privateNetworkClientServer / loopback exemption), writes outside the container folder are denied, tasks run.
    auto lis = os::socket::listen("127.0.0.1", 0); CHECK(lis); if (!lis) return;
    const std::string port = std::to_string(lis->local_port());
    const std::string dir = os::join(os::temp_dir(), "forge_pp_ac_" + std::to_string(GetCurrentProcessId()));
    os::remove_all(dir); os::mkdirs(dir);
    {
        process_pool open_pool({.workers = 1});   // control: an unconfined worker reaches the listener and writes
        CHECK(open_pool.submit("probe_connect", port).get() == "connected");
        CHECK(open_pool.submit("probe_write", os::join(dir, "open.txt")).get() == "wrote");
    }
    bool ran = false;
    {
        os::sandbox_policy p; p.deny_network = true;
        process_pool pp({.workers = 1, .sandbox = p});
        std::string sq;
        try { sq = pp.submit("square", "3").get(); }
        catch (const os::os_exception& e) { std::printf("  skip AppContainer pool: %s\n", e.what()); }
        if (!sq.empty()) {
            ran = true;
            CHECK(sq == "9");
            std::string mech = pp.submit("sb_mech", "").get(); std::printf("  worker sandbox: %s\n", mech.c_str());
            CHECK(mech.rfind("appcontainer", 0) == 0);
            CHECK(pp.submit("probe_connect", port).get() == "refused");
            CHECK(pp.submit("square", "12").get() == "144");
        }
    }
    if (ran) {
        os::sandbox_policy p; p.deny_fs_write = true; p.deny_exec = true;
        process_pool pp({.workers = 2, .sandbox = p});
        CHECK(pp.submit("probe_write", os::join(dir, "nofs.txt")).get() == "denied");
        CHECK(!os::exists(os::join(dir, "nofs.txt")));
        std::string mech = pp.submit("sb_mech", "").get();
        CHECK_MSG(mech == "appcontainer+job-object", "%s", mech.c_str());
        std::vector<std::future<std::string>> fs; for (int i = 0; i < 8; ++i) fs.push_back(pp.submit("square", std::to_string(i)));
        for (int i = 0; i < 8; ++i) CHECK(fs[std::size_t(i)].get() == std::to_string(i * i));
        try { (void)pp.submit("fail", "").get(); CHECK(false); } catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::not_found); }
    }
    // exec denial alone is applied in process (job object): tasks run
    os::sandbox_policy x; x.deny_exec = true;
    { process_pool px({.workers = 1, .sandbox = x}); CHECK(px.submit("square", "4").get() == "16"); }
    os::remove_all(dir);
    for (const wchar_t* n : {L"forge.sandbox.nonet", L"forge.sandbox.nofs", L"forge.sandbox.nonet.nofs"}) DeleteAppContainerProfile(n);
}
TEST(pp_windows_fork_rejected) {
    try { process_pool pp({.workers = 1, .mode = process_pool::spawn_mode::fork}); CHECK(false); }
    catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::not_supported); }
}
#endif

#if !defined(FORGE_OS_IOS)
TEST(pp_terminate_kills_busy_workers) {
    process_pool pp({.workers = 2});
    std::vector<std::future<std::string>> fs;
    for (int i = 0; i < 3; ++i) fs.push_back(pp.submit("hang", ""));   // two running, one queued
    CHECK(!pp.wait_all_for(300));
    auto t0 = std::chrono::steady_clock::now();
    pp.terminate(); pp.terminate();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
    for (auto& f : fs) {
        try { (void)f.get(); CHECK(false); } catch (const os::os_exception& e) { CHECK(e.kind == os::os_errc::cancelled); }
    }
    CHECK_THROWS(pp.submit("square", "1").get());
}
#endif
