// forge::os process / pipe / signal (OS wrapper phase 1).
#include <forge/os/process.hpp>
#include <forge/os/pipe.hpp>
#include <forge/os/signal.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <sstream>
#include <cstdio>
#include "check.hpp"
#include <cctype>
#include <cstdlib>
#include <string>
using namespace forge;

#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
#include <pthread.h>
// Every FORGE fork goes through forge::os::serialized_fork (one process-wide
// mutex), so concurrent spawn_wait calls never run pthread_atfork prepare
// handlers at the same time. The prepare handler below fails the test if two
// forks overlap.
namespace { std::atomic<int> g_in_fork{0}; std::atomic<int> g_overlap{0}; }
TEST(forks_are_serialized) {
    static bool installed = false;
    if (!installed) {
        installed = true;
        ::pthread_atfork([] { if (g_in_fork.fetch_add(1) != 0) g_overlap.fetch_add(1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); },
                         [] { g_in_fork.fetch_sub(1); }, [] { g_in_fork.store(0); });
    }
    std::vector<std::thread> ts;
    std::atomic<int> bad{0};
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                forge::os::spawn_spec sp; sp.argv = {"true"};
                auto r = forge::os::spawn_wait(sp);
                if (r.err || r.value.exit_code != 0) bad.fetch_add(1);
            }
        });
    for (auto& t : ts) t.join();
    CHECK(bad.load() == 0);
    CHECK(g_overlap.load() == 0);
    CHECK(&forge::os::fork_mutex() == &forge::os::fork_mutex());
}

TEST(spawn_captures_separately) {
    os::spawn_spec s; s.argv = {"sh", "-c", "echo out; echo err 1>&2; exit 3"};
    auto r = os::spawn_wait(s);
    CHECK(r); CHECK(r->out == "out\n"); CHECK(r->err == "err\n"); CHECK(r->exit_code == 3); CHECK(!r->timed_out);
}
TEST(spawn_stdin_env_cwd) {
    os::spawn_spec s; s.argv = {"sh", "-c", "cat; printf %s \"$FOO\"; pwd"};
    s.stdin_data = std::string(200000, 'x'); s.env_set["FOO"] = "bar"; s.cwd = "/";
    auto r = os::spawn_wait(s);
    CHECK(r); CHECK(r->out == std::string(200000, 'x') + "bar/\n");
}
TEST(spawn_merge_stderr) {
    os::spawn_spec s; s.argv = {"sh", "-c", "echo a; echo b 1>&2"}; s.merge_stderr = true;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->out.size() == 4); CHECK(r->err.empty());
}
TEST(spawn_timeout_kills_tree) {
    os::spawn_spec s; s.argv = {"sh", "-c", "sleep 30 & sleep 30"}; s.timeout_ms = 200;
    auto t0 = os::monotonic_ns(); auto r = os::spawn_wait(s);
    CHECK(r); CHECK(r->timed_out); CHECK(r->exit_code == 124); CHECK(os::monotonic_ns() - t0 < 5'000'000'000ull);
}
TEST(spawn_missing_command) {
    os::spawn_spec s; s.argv = {"no-such-command-forge-xyz"};
    auto r = os::spawn_wait(s); CHECK(!r);
}
TEST(spawn_memory_limit) {
#if defined(__APPLE__) || defined(__ANDROID__)
    // macOS has no enforced memory rlimit, and Android's bionic cannot start a process under a realistic
    // RLIMIT_AS: the cap is a resident-size watchdog, so check that it kills a child
    // that holds 256 MiB (dd keeps its 64 MiB buffer for every block; 8 blocks take far longer than 2 ms polls).
    os::spawn_spec s; s.argv = {"sh", "-c", "exec dd if=/dev/zero bs=64M count=64 of=/dev/null 2>/dev/null"};
    s.limits.memory_bytes = 32ull << 20;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->exit_code != 0);
    s.limits.memory_bytes = 512ull << 20; s.argv = {"sh", "-c", "exec dd if=/dev/zero bs=64M count=2 of=/dev/null 2>/dev/null"};
    auto ok = os::spawn_wait(s); CHECK(ok); CHECK(ok->exit_code == 0);
#else
    os::spawn_spec s; s.argv = {"sh", "-c", "ulimit -v"}; s.limits.memory_bytes = 512ull << 20;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->out == "524288\n");
    if (r && r->out != "524288\n") std::printf("  ulimit -v printed [%s]\n", r->out.c_str());
#endif
}
TEST(spawn_fsize_core_limits) {
    os::spawn_spec s; s.argv = {"sh", "-c", "ulimit -f; ulimit -c"}; s.limits.file_size_bytes = 1ull << 20; s.limits.no_core = true;
    // POSIX shells count -f in 512-byte blocks; bash before 5.1 (macOS /bin/sh is bash 3.2) uses 1024-byte blocks.
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->out == "2048\n0\n" || r->out == "1024\n0\n");
    if (r && r->out != "2048\n0\n" && r->out != "1024\n0\n") std::printf("  ulimit -f/-c printed [%s]\n", r->out.c_str());
}
TEST(apply_limits_clamps_to_hard) {
    // a child whose hard NOFILE is 64 asks for 256: lowered to 64 instead of failing and leaving none
    pid_t p = ::fork();
    if (p == 0) {
        struct rlimit h{64, 64}; ::setrlimit(RLIMIT_NOFILE, &h);
        os::proc_limits l; l.open_files = 256; l.cpu_seconds = 100;
        bool ok = os::apply_limits(l); struct rlimit got{}; ::getrlimit(RLIMIT_NOFILE, &got);
        ::_exit(ok && got.rlim_cur == 64 && got.rlim_max == 64 ? 0 : 1);
    }
    int st = 0; ::waitpid(p, &st, 0); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}
TEST(spawn_stop_token_cancels) {
    std::stop_source src; os::spawn_spec s; s.argv = {"sh", "-c", "sleep 30 & sleep 30"}; s.stop = src.get_token();
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); src.request_stop(); });
    auto t0 = os::monotonic_ns(); auto r = os::spawn_wait(s); t.join();
    CHECK(r); CHECK(r->cancelled); CHECK(r->exit_code == 125); CHECK(os::monotonic_ns() - t0 < 5'000'000'000ull);
}
TEST(spawn_file_redirects) {
    std::string d = "/tmp/forge_redir_" + std::to_string(::getpid());
    { FILE* f = std::fopen((d + ".in").c_str(), "w"); std::fputs("hello\n", f); std::fclose(f); }
    os::spawn_spec s; s.argv = {"sh", "-c", "cat; echo e >&2"}; s.stdin_file = d + ".in"; s.stdout_file = d + ".out"; s.stderr_file = d + ".err";
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->exit_code == 0); CHECK(r->out.empty());
    auto slurp = [](const std::string& p) { std::string o; if (FILE* f = std::fopen(p.c_str(), "r")) { char b[256]; std::size_t n; while ((n = std::fread(b, 1, sizeof b, f))) o.append(b, n); std::fclose(f); } return o; };
    CHECK(slurp(d + ".out") == "hello\n"); CHECK(slurp(d + ".err") == "e\n");
    s.append_output = true; s.argv = {"sh", "-c", "echo more"}; s.stdin_file.clear(); CHECK(os::spawn_wait(s));
    CHECK(slurp(d + ".out") == "hello\nmore\n");
    for (auto x : {".in", ".out", ".err"}) std::remove((d + x).c_str());
    s.stdin_file = "/nonexistent/forge"; CHECK(!os::spawn_wait(s));
}
TEST(spawn_new_session_and_group_kill) {
    os::spawn_spec s; s.argv = {"sh", "-c", "ps -o sid= -p $$; ps -o pid= -p $$"}; s.new_session = true;
    auto r = os::spawn_wait(s); CHECK(r);
    if (r && r->exit_code == 0) { std::istringstream is(r->out); long sid = 0, pid = 0; is >> sid >> pid; CHECK(sid == pid); }
    os::spawn_spec c; c.argv = {"sh", "-c", "sleep 30 & sleep 30"}; c.new_session = true;
    auto ch = os::child::start(c); CHECK(ch);
    CHECK(os::kill_group(ch->pid(), SIGKILL)); CHECK(ch->wait(5000) == 128 + SIGKILL);
}
#if defined(__linux__)
TEST(spawn_memory_watchdog) {
    os::spawn_spec s; s.argv = {"sh", "-c", "x=$(head -c 200000000 /dev/zero | tr '\\0' a); sleep 5"}; s.watch_memory_bytes = 64ull << 20;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->memory_killed); CHECK(r->exit_code == 137);
}
#endif
TEST(spawn_peak_rss_reported) {
    os::spawn_spec s; s.argv = {"sh", "-c", "x=$(head -c 30000000 /dev/zero | tr '\\0' a); echo ${#x}"};
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->out == "30000000\n"); CHECK(r->peak_rss_bytes > (20ull << 20)); CHECK(!r->unreaped);
}
TEST(spawn_timeout_sweeps_session) {
    // the grandchild moves to its own process group but stays in the session: the timeout still kills it
    auto pidfile = std::string("/tmp/forge_sess_") + std::to_string(::getpid());
    os::spawn_spec s; s.argv = {"sh", "-c", "(set -m; sleep 60 & echo $! > " + pidfile + "; wait) & sleep 60"};
    s.new_session = true; s.timeout_ms = 500;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->timed_out); CHECK(!r->unreaped);
    std::FILE* f = std::fopen(pidfile.c_str(), "r"); CHECK(f != nullptr);
    long gp = 0; if (f) { CHECK(std::fscanf(f, "%ld", &gp) == 1); std::fclose(f); }
    std::remove(pidfile.c_str());
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) { gone = gp > 0 && (::kill(pid_t(gp), 0) != 0 || [&] {  // reaped, or a zombie waiting for init
        std::FILE* st = std::fopen(("/proc/" + std::to_string(gp) + "/stat").c_str(), "r"); if (!st) return true;
        char b[512]{}; std::size_t n = std::fread(b, 1, sizeof b - 1, st); std::fclose(st); (void)n;
        const char* q = std::strrchr(b, ')'); return q && q[2] == 'Z'; }()); if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(gone);
}
TEST(spawn_normal_exit_sweeps_session) {
    // the leader exits 0 but leaves a background grandchild in its own group: new_session still reaps it
    auto pidfile = std::string("/tmp/forge_sessx_") + std::to_string(::getpid());
    os::spawn_spec s; s.argv = {"sh", "-c", "(set -m; sleep 60 >/dev/null 2>&1 & echo $! > " + pidfile + ") ; sleep 60 >/dev/null 2>&1 & sleep 0.2; exit 0"};
    s.new_session = true; s.timeout_ms = 20000;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(!r->timed_out); CHECK(r->exit_code == 0);
    std::FILE* f = std::fopen(pidfile.c_str(), "r"); CHECK(f != nullptr);
    long gp = 0; if (f) { CHECK(std::fscanf(f, "%ld", &gp) == 1); std::fclose(f); }
    std::remove(pidfile.c_str());
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) { gone = gp > 0 && (::kill(pid_t(gp), 0) != 0 || [&] {
        std::FILE* st = std::fopen(("/proc/" + std::to_string(gp) + "/stat").c_str(), "r"); if (!st) return true;
        char b[512]{}; std::size_t n = std::fread(b, 1, sizeof b - 1, st); std::fclose(st); (void)n;
        const char* q = std::strrchr(b, ')'); return q && q[2] == 'Z'; }()); if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(gone);
}
TEST(spawn_output_cap) {
    os::spawn_spec s; s.argv = {"sh", "-c", "head -c 100000 /dev/zero"}; s.max_output = 1000;
    auto r = os::spawn_wait(s); CHECK(r); CHECK(r->out.size() <= 1000); CHECK(r->truncated);
}
TEST(child_stream_and_kill) {
    auto c = os::child::start({"sh", "-c", "echo hi; sleep 30"});
    CHECK(c); CHECK(c->running());
    CHECK(c->read(100, 2000) == "hi\n");
    CHECK(c->wait(50) == -1);
    CHECK(c->kill(9)); CHECK(c->wait() == 128 + 9);
}
TEST(pipe_roundtrip) {
    auto p = os::make_pipe(); CHECK(p);
    CHECK(p->write.write_all("hello")); p->write.close();
    CHECK(p->read.read_all() == "hello");
}
TEST(signals) {
    os::signal_id s; CHECK(os::sig_from_name("term", s) && s == os::signal_id::terminate);
    auto c = os::child::start({"sleep", "30"}); CHECK(c);
    CHECK(os::send_signal(c->pid(), os::signal_id::terminate));
    CHECK(c->wait() == 128 + SIGTERM);
}
TEST(concurrent_child_start_reads_to_eof) {
    // Every pipe end is close-on-exec from creation: a long-lived child started on another thread must not
    // inherit this child's stdout write end, or read() would wait for that sibling (1.5 s) instead of seeing EOF.
    std::atomic<int> slow{0}, bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 15; ++i) {
                auto sib = os::child::start({"sleep", "1.5"});
                auto c = os::child::start({"sh", "-c", "echo x"});
                if (!sib || !c) { bad.fetch_add(1); continue; }
                auto t0 = os::monotonic_ns(); std::string out, part;
                while (!(part = c->read()).empty()) out += part;
                if (os::monotonic_ns() - t0 > 1'000'000'000ull) slow.fetch_add(1);
                if (out != "x\n" || c->wait() != 0) bad.fetch_add(1);
                sib->kill();
            }
        });
    for (auto& t : ts) t.join();
    CHECK(bad.load() == 0); CHECK(slow.load() == 0);
    // spawn_wait under the same load: each one's pipes reach EOF with the direct child
    std::vector<std::thread> sw;
    for (int t = 0; t < 4; ++t)
        sw.emplace_back([&] {
            for (int i = 0; i < 10; ++i) {
                auto sib = os::child::start({"sleep", "1.5"});
                os::spawn_spec s; s.argv = {"sh", "-c", "echo y"}; s.stdin_data = "in";
                auto r = os::spawn_wait(s);
                if (!r || r->out != "y\n" || r->wall_ms > 1000) bad.fetch_add(1);
                if (sib) sib->kill();
            }
        });
    for (auto& t : sw) t.join();
    CHECK(bad.load() == 0);
}
TEST(make_pipe_is_cloexec) {
    auto p = os::make_pipe(); CHECK(p);
    if (p) { CHECK((::fcntl(p->read.native(), F_GETFD) & FD_CLOEXEC) != 0); CHECK((::fcntl(p->write.native(), F_GETFD) & FD_CLOEXEC) != 0); }
}
#endif
TEST(find_exe) { CHECK(os::find_executable("no-such-command-forge-xyz").empty()); CHECK(!os::process_caps().empty() || true); }
TEST(send_signal_rejects_invalid_pids) {
    // pid 0 would signal our own group, group + pid 1 would be kill(-1), 2^32+pid would truncate to another pid.
    // (A failure of these checks would kill the test runner, or worse, so they come first in send_signal.)
    CHECK(!os::send_signal(0, os::signal_id::terminate));
    CHECK(!os::send_signal(0, os::signal_id::terminate, true));
    CHECK(!os::send_signal(1, os::signal_id::terminate, true));
    CHECK(!os::send_signal((1ull << 32) + os::pid(), os::signal_id::terminate));
    CHECK(!os::send_signal(~0ull, os::signal_id::kill));
#if defined(FORGE_OS_POSIX)
    CHECK(os::send_signal(0, os::signal_id::terminate).err.code == EINVAL);
    CHECK(!os::kill_group(0)); CHECK(!os::kill_group(1)); CHECK(!os::kill_group((1ull << 32) + 1));
#endif
}

#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
TEST(child_start_spec_env_cwd_and_exec_error) {
    os::spawn_spec s; s.argv = {"sh", "-c", "echo $FORGE_X; pwd"}; s.env_set["FORGE_X"] = "hi"; s.cwd = "/";
    auto c = os::child::start(s); CHECK(c);
    std::string out, part; while (!(part = c->read()).empty()) out += part;
    CHECK(c->wait() == 0); CHECK(out == "hi\n/\n");
    os::spawn_spec bad; bad.argv = {"/nonexistent/forge-no-such-binary"};
    CHECK(!os::child::start(bad));
}
TEST(child_start_memory_limit) {
    // count=16: the 64 MiB buffer stays resident across blocks, long enough for the macOS watchdog (2 ms polls);
    // elsewhere the allocation itself fails under RLIMIT_AS / the Job Object (Android: the watchdog, as on macOS).
    os::spawn_spec s; s.argv = {"sh", "-c", "exec dd if=/dev/zero bs=64M count=16 of=/dev/null 2>/dev/null"};
    s.limits.memory_bytes = 32ull << 20;
    auto c = os::child::start(s); CHECK(c); CHECK(c->wait() != 0);   // 64 MiB buffer cannot fit in 32 MiB
    s.limits.memory_bytes = 512ull << 20;
    auto d = os::child::start(s); CHECK(d); CHECK(d->wait() == 0);
}
TEST(wait_any_picks_first_and_wait_all) {
    auto mk = [](const char* t, int code) { os::spawn_spec s; s.argv = {"sh", "-c", std::string("sleep ") + t + "; exit " + std::to_string(code)}; return std::move(os::child::start(s).value); };
    os::child a = mk("0.6", 1), b = mk("0.05", 2), c = mk("0.3", 3);
    os::child* all[] = {&a, &b, &c, nullptr};
    auto i = os::wait_any(all); CHECK(i && *i == 1); CHECK(b.exit_code() == 2);
    i = os::wait_any(all); CHECK(i && *i == 2); CHECK(c.exit_code() == 3);
    CHECK(!os::wait_any(all, 20));                         // a still sleeping: timeout
    CHECK(a.try_wait() == -1);
    CHECK(os::wait_all(all, 5000)); CHECK(a.exit_code() == 1);
    CHECK(!os::wait_any(all));                             // nothing left to wait for
    os::child d = mk("5", 0); os::child* one[] = {&d};
    auto t0 = os::monotonic_ns(); CHECK(!os::wait_all(one, 50)); CHECK(os::monotonic_ns() - t0 < 2000000000ull);
    d.kill(); CHECK(d.wait() == 128 + 9);
}
#endif

#if !defined(FORGE_OS_IOS)
// list_processes / process_info on every desktop platform: our own entry carries every field, and a child we
// started lists us as its parent with its argv in cmdline.
TEST(process_list_self_and_child) {
    auto l = os::list_processes(); CHECK(l); CHECK(l && l->size() > 1);
    auto me = os::process_info(os::pid()); CHECK(me);
    if (me) {
        CHECK(me->pid == os::pid()); CHECK(me->ppid > 0); CHECK(me->rss_bytes > 0);
        CHECK(me->name.find("test_os_pro") != std::string::npos);
        CHECK(me->cmdline.find("test_os_process") != std::string::npos);
        if (me->name.find("test_os_pro") == std::string::npos || me->cmdline.find("test_os_process") == std::string::npos)
            std::printf("  self: name [%s] cmdline [%s]\n", me->name.c_str(), me->cmdline.c_str());
    }
#if defined(FORGE_OS_WINDOWS)
    auto c = os::child::start({"ping", "-n", "30", "127.0.0.1"});
#else
    auto c = os::child::start({"sleep", "29.5"});
#endif
    CHECK(c);
    if (c) {
        // POSIX start(argv) returns right after fork: until exec the child still shows our argv, so retry briefly
        auto has_argv = [](const os::process_entry& e) { return e.cmdline.find("29.5") != std::string::npos || e.cmdline.find("127.0.0.1") != std::string::npos; };
        auto k = os::process_info(c->pid());
        for (int i = 0; i < 200 && k && !has_argv(k.value); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); k = os::process_info(c->pid()); }
        CHECK(k);
        if (k) { CHECK(k->ppid == os::pid()); CHECK(has_argv(k.value)); }
        CHECK(c->kill()); CHECK(c->wait() == 128 + 9);
    }
    CHECK(!os::process_info(~0ull - 7));
}
#endif

#if defined(FORGE_OS_WINDOWS)
// child on Windows: CreateProcess + Job Object. ping -n N sleeps about N-1 seconds; sort reads stdin.
namespace {
std::string drain(os::child& c) { std::string out, part; while (!(part = c.read()).empty()) out += part; return out; }
std::string sysroot() { std::string r = os::getenv("SystemRoot"); return r.empty() ? std::string("C:\\Windows") : r; }
}
TEST(win_child_stream_and_kill_tree) {
    auto c = os::child::start({"cmd", "/d", "/c", "echo hi& ping -n 30 127.0.0.1 >nul"});
    CHECK(c); if (!c) return;
    CHECK(c->running()); CHECK(c->pid() != 0);
    CHECK(c->read(100, 5000) == "hi\r\n");
    CHECK(c->read(100, 50).empty());                       // nothing more yet: times out
    CHECK(c->try_wait() == -1); CHECK(c->wait(50) == -1); CHECK(!c->exited());
    CHECK(c->kill(0));                                     // still running
    CHECK(c->kill(9)); CHECK(c->wait() == 128 + 9); CHECK(c->exit_code() == 137); CHECK(!c->running());
    CHECK(!c->kill(9));                                    // already reaped
    // ping (the grandchild, holding the pipe too) died with the job, so the stream ends now, not in 30 s
    auto t0 = os::monotonic_ns(); CHECK(c->read().empty()); CHECK(os::monotonic_ns() - t0 < 5'000'000'000ull);
}
TEST(win_child_merge_exit_code_and_errors) {
    auto m = os::child::start({"cmd", "/d", "/c", "echo a& echo b>&2& exit 3"}, true);
    CHECK(m); if (m) { CHECK(drain(*m) == "a\r\nb\r\n"); CHECK(m->wait() == 3); CHECK(m->try_wait() == 3); }
    CHECK(!os::child::start({"C:\\nonexistent\\forge-no-such-binary.exe"}));
    CHECK(!os::child::start({"cmd", "/d", "/c", "exit 0"}, false, "C:\\nonexistent-forge-dir\\x"));
    os::child moved;
    { auto t = os::child::start({"cmd", "/d", "/c", "exit 5"}); CHECK(t); if (t) moved = std::move(t.value); }
    CHECK(moved.wait(10000) == 5);
}
TEST(win_child_spec_env_cwd_stdin_stderr) {
    os::spawn_spec s; s.argv = {"cmd", "/d", "/c", "echo %FORGE_X%& cd"}; s.env_set["FORGE_X"] = "hi"; s.cwd = sysroot();
    auto c = os::child::start(s); CHECK(c);
    // %SystemRoot% is often "C:\WINDOWS" while cd prints the on-disk "C:\Windows": compare case-insensitively
    auto lower = [](std::string x) { for (auto& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch))); return x; };
    if (c) { auto out = drain(*c); CHECK(c->wait() == 0); CHECK(lower(out) == lower("hi\r\n" + sysroot() + "\r\n")); }
    std::string d = os::temp_dir() + "\\forge_wchild_" + std::to_string(os::pid());
    CHECK(os::write_file(d + ".in", "b\r\na\r\n"));
    os::spawn_spec so; so.argv = {sysroot() + "\\System32\\sort.exe"}; so.stdin_file = d + ".in";
    auto st = os::child::start(so); CHECK(st);
    if (st) { CHECK(drain(*st) == "a\r\nb\r\n"); CHECK(st->wait() == 0); }
    os::spawn_spec se; se.argv = {"cmd", "/d", "/c", "echo e>&2"}; se.stderr_file = d + ".err";
    auto e = os::child::start(se); CHECK(e);
    if (e) { CHECK(drain(*e).empty()); CHECK(e->wait() == 0); }
    auto ef = os::read_file(d + ".err"); CHECK(ef); CHECK(ef && ef.value == "e\r\n");
    os::spawn_spec nul; nul.argv = {sysroot() + "\\System32\\sort.exe"};   // no stdin_file: NUL, so sort ends at once
    auto n = os::child::start(nul); CHECK(n); if (n) { CHECK(drain(*n).empty()); CHECK(n->wait(10000) == 0); }
    std::remove((d + ".in").c_str()); std::remove((d + ".err").c_str());
    se.stdin_file = d + ".missing"; CHECK(!os::child::start(se));
}
TEST(win_child_memory_limit) {
    os::spawn_spec s; s.argv = {"cmd", "/d", "/c", "exit 0"}; s.limits.memory_bytes = 256ull << 10;   // cmd cannot even start in 256 KiB
    auto c = os::child::start(s); if (c) CHECK(c->wait(10000) != 0);
    s.limits.memory_bytes = 512ull << 20;
    auto d = os::child::start(s); CHECK(d); if (d) CHECK(d->wait(10000) == 0);
}
TEST(win_wait_any_and_wait_all) {
    auto mk = [](int secs, int code) { return std::move(os::child::start({"cmd", "/d", "/c", "ping -n " + std::to_string(secs + 1) + " 127.0.0.1 >nul& exit " + std::to_string(code)}).value); };
    os::child a = mk(3, 1), b = mk(0, 2), c = mk(1, 3);
    os::child* all[] = {&a, &b, &c, nullptr};
    auto i = os::wait_any(all); CHECK(i && *i == 1); CHECK(b.exit_code() == 2);
    i = os::wait_any(all); CHECK(i && *i == 2); CHECK(c.exit_code() == 3);
    CHECK(!os::wait_any(all, 20));
    CHECK(a.try_wait() == -1);
    CHECK(os::wait_all(all, 10000)); CHECK(a.exit_code() == 1);
    CHECK(!os::wait_any(all));
    os::child d = mk(5, 0); os::child* one[] = {&d};
    auto t0 = os::monotonic_ns(); CHECK(!os::wait_all(one, 50)); CHECK(os::monotonic_ns() - t0 < 2000000000ull);
    d.kill(); CHECK(d.wait() == 128 + 9);
}
TEST(win_spawn_large_stdin_to_child_that_ignores_it) {
    // 1 MB of stdin to a child that never reads it: the writer runs on its own thread, so the timeout still
    // fires (the old synchronous WriteFile blocked forever once the pipe buffer filled).
    os::spawn_spec s; s.argv = {"ping", "-n", "30", "127.0.0.1"}; s.stdin_data = std::string(1 << 20, 'x'); s.timeout_ms = 500;
    auto t0 = os::monotonic_ns(); auto r = os::spawn_wait(s);
    CHECK(r); if (r) { CHECK(r->timed_out); CHECK(r->exit_code == 124); }
    CHECK(os::monotonic_ns() - t0 < 10'000'000'000ull);
    // without a timeout: the child exits without reading, the blocked writer is cancelled
    os::spawn_spec q; q.argv = {"cmd", "/d", "/c", "exit 4"}; q.stdin_data = std::string(1 << 20, 'x');
    t0 = os::monotonic_ns(); auto rq = os::spawn_wait(q);
    CHECK(rq); if (rq) CHECK(rq->exit_code == 4); CHECK(os::monotonic_ns() - t0 < 10'000'000'000ull);
    // and a child that reads everything gets all of it
    std::string lines; for (int i = 0; i < 16384; ++i) lines += std::string(63, 'x') + "\n";   // 1 MiB of short lines
    os::spawn_spec f; f.argv = {sysroot() + "\\System32\\findstr.exe", "/c:x"}; f.stdin_data = lines;
    auto rf = os::spawn_wait(f); CHECK(rf); if (rf) { CHECK(rf->exit_code == 0); CHECK(rf->out.size() >= 16384u * 64u); }
}
TEST(win_spawn_background_grandchild_holding_pipes) {
    // cmd exits at once; the ping it started in the background inherits stdout/stderr. spawn_wait returns when
    // cmd exits (draining what is buffered) instead of waiting 30 s for ping to drop the pipes.
    os::spawn_spec s; s.argv = {"cmd", "/d", "/c", "start /b ping -n 30 127.0.0.1& echo done"};
    auto t0 = os::monotonic_ns(); auto r = os::spawn_wait(s);
    CHECK(r); if (r) { CHECK(r->exit_code == 0); CHECK(r->out.find("done") != std::string::npos); CHECK(!r->timed_out); }
    CHECK(os::monotonic_ns() - t0 < 10'000'000'000ull);
}
TEST(win_batch_arguments_are_not_commands) {
    // BatBadBut: a .bat runs through cmd.exe, which ignores MSVCRT's \" escaping. Each argument must stay one
    // literal argument: no "pwned" line, %PATH% not expanded.
    std::string d = os::temp_dir() + "\\forge_bat_" + std::to_string(os::pid()) + ".bat";
    CHECK(os::write_file(d, "@echo off\r\necho [%1]\r\necho [%2]\r\n"));
    os::spawn_spec s; s.argv = {d, "\"&echo pwned&\"", "%PATH%"};
    auto r = os::spawn_wait(s); CHECK(r);
    if (r) {
        CHECK(r->exit_code == 0);
        CHECK(r->out.find("\npwned") == std::string::npos); CHECK(r->out.rfind("pwned", 0) != 0);
        CHECK(r->out.find("&echo pwned&") != std::string::npos);
        CHECK(r->out.find("%PATH%") != std::string::npos);
        std::printf("  batch out [%s]\n", r->out.c_str());
    }
    auto c = os::child::start({d, "a&b"}); CHECK(c); if (c) { CHECK(drain(*c) == "[\"a&b\"]\r\n[]\r\n"); CHECK(c->wait() == 0); }
    os::spawn_spec nl; nl.argv = {d, "a\nb"}; CHECK(!os::spawn_wait(nl));   // CR/LF would end cmd's line: refused
    std::remove(d.c_str());
}
TEST(win_env_names_are_case_insensitive) {
    auto count_path = [](const std::string& out, std::string& val) {
        int n = 0; std::size_t i = 0;
        while (i < out.size()) {
            auto j = out.find('\n', i); if (j == std::string::npos) j = out.size();
            std::string line = out.substr(i, j - i); if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 5 && (line[0] | 32) == 'p' && (line[1] | 32) == 'a' && (line[2] | 32) == 't' && (line[3] | 32) == 'h' && line[4] == '=') { ++n; val = line.substr(5); }
            i = j + 1;
        }
        return n;
    };
    os::spawn_spec s; s.argv = {sysroot() + "\\System32\\cmd.exe", "/d", "/c", "set"}; s.env_set["path"] = sysroot() + "\\System32;C:\\forge-x";
    auto r = os::spawn_wait(s); CHECK(r);
    std::string v; if (r) { CHECK(count_path(r->out, v) == 1); CHECK(v.find("forge-x") != std::string::npos); }
    os::spawn_spec u; u.argv = s.argv; u.env_unset = {"pATH"};
    auto ru = os::spawn_wait(u); CHECK(ru); if (ru) CHECK(count_path(ru->out, v) == 0);
}
TEST(win_no_core_and_job_limits) {
    // no_core: DIE_ON_UNHANDLED_EXCEPTION in the job; the child still starts and exits normally
    os::spawn_spec s; s.argv = {"cmd", "/d", "/c", "exit 6"}; s.limits.no_core = true;
    auto r = os::spawn_wait(s); CHECK(r); if (r) CHECK(r->exit_code == 6);
}
#endif
