// forge::os phase 2: fs, users, time, sysinfo, console.
#include <forge/os/fs.hpp>
#include <forge/os/users.hpp>
#include <forge/os/time.hpp>
#include <forge/os/sysinfo.hpp>
#include <forge/os/console.hpp>
#include <forge/os/oserror.hpp>
#include <forge/os/pipe.hpp>
#include <forge/os/service_control.hpp>
#include <atomic>
#include <random>
#include <thread>
#include <vector>
#include "check.hpp"
using namespace forge;
namespace fs = forge::os::fs;

TEST(glob_match_rules) {
    CHECK(fs::glob_match("*.cpp", "a.cpp")); CHECK(!fs::glob_match("*.cpp", "d/a.cpp"));
    CHECK(fs::glob_match("**/*.cpp", "a.cpp")); CHECK(fs::glob_match("**/*.cpp", "x/y/a.cpp"));
    CHECK(fs::glob_match("src/**", "src/a/b")); CHECK(fs::glob_match("a?c", "abc")); CHECK(!fs::glob_match("a?c", "a/c"));
    CHECK(fs::glob_match("[a-c]x", "bx")); CHECK(!fs::glob_match("[!a-c]x", "bx")); CHECK(fs::glob_match("\\*", "*"));
    CHECK(fs::glob_match("a*b*c", "aXXbYYc")); CHECK(!fs::glob_match("a*b*c", "aXXbYY"));
}
TEST(fs_tree_ops) {
    auto d = fs::make_temp_dir(); CHECK(d); std::string r = *d;
    CHECK(fs::make_dirs(r + "/src/sub"));
    CHECK(fs::write_file_atomic(r + "/src/a.txt", "alpha")); CHECK(fs::write_file_atomic(r + "/src/sub/b.cpp", "beta"));
    CHECK(fs::write_file_atomic(r + "/src/sub/b.cpp", "beta2"));
    CHECK(os::read_file(r + "/src/sub/b.cpp").value == "beta2");
    CHECK(fs::copy_tree(r + "/src", r + "/dst")); CHECK(os::read_file(r + "/dst/sub/b.cpp").value == "beta2");
    auto g = fs::glob("**/*.cpp", r); CHECK(g); CHECK(g->size() == 2);
    auto g2 = fs::glob("src/*.txt", r); CHECK(g2); CHECK(g2->size() == 1 && (*g2)[0] == "src/a.txt");
    auto w = fs::walk(r + "/src"); CHECK(w); CHECK(w->size() == 3);
    { fs::walk_opts o; o.include_dirs = false; auto w2 = fs::walk(r, o); CHECK(w2 && w2->size() == 4); }
    CHECK(fs::move(r + "/dst", r + "/moved")); CHECK(!fs::exists(r + "/dst")); CHECK(fs::is_file(r + "/moved/a.txt"));
    CHECK(*fs::file_size(r + "/src/a.txt") == 5);
    std::int64_t t = 1700000000123000000LL; CHECK(fs::set_mtime_ns(r + "/src/a.txt", t));
    auto m = fs::mtime_ns(r + "/src/a.txt"); CHECK(m); CHECK(*m / 1000000 == t / 1000000);
#if defined(FORGE_OS_POSIX)
    CHECK(fs::chmod(r + "/src/a.txt", 0600)); CHECK(*fs::mode(r + "/src/a.txt") == 0600);
    CHECK(fs::symlink(r + "/src/a.txt", r + "/ln")); CHECK(fs::is_symlink(r + "/ln")); CHECK(fs::read_link(r + "/ln").value == r + "/src/a.txt");
#endif
    auto du = fs::disk_usage(r); CHECK(du); CHECK(du->total > 0);
    auto n = fs::remove_all(r); CHECK(n); CHECK(*n >= 8); CHECK(!fs::exists(r));
}
TEST(file_lock_exclusive) {
    auto p = fs::make_temp_file(); CHECK(p);
    auto a = fs::file_lock::acquire(*p); CHECK(a); CHECK(a->held());
#if defined(FORGE_OS_POSIX)
    auto b = fs::file_lock::acquire(*p, true, false); CHECK(!b);   // flock is per open file description
#endif
    a->unlock(); auto c = fs::file_lock::acquire(*p, true, false); CHECK(c);
    c->unlock(); fs::remove_all(*p);
}
#if !defined(FORGE_OS_ANDROID) && !defined(FORGE_OS_IOS)
TEST(mounts_list) {
    auto m = fs::mounts(); CHECK(m); CHECK(!m->empty());
    bool root = false; for (auto& x : *m) { CHECK(!x.path.empty()); root |= x.path == "/" || (x.path.size() == 3 && x.path[1] == ':'); }
    CHECK(root);
}
#endif
TEST(users_dirs) {
    auto u = os::current_user(); CHECK(!u.name.empty() || !u.home.empty());
    CHECK(u.uid >= 0); auto r = os::lookup_uid(u.uid); CHECK(r); CHECK(r->name == u.name); CHECK(r->uid == u.uid);
    auto n = os::lookup_user(u.name); CHECK(n); CHECK(n->uid == u.uid); CHECK(!n->home.empty());
    CHECK(!os::lookup_user("no-such-user-forge-xyz"));
#if defined(FORGE_OS_WINDOWS)
    CHECK(u.sid.rfind("S-1-", 0) == 0); CHECK(n->sid == u.sid); CHECK(u.gid >= 0);
    CHECK(!os::lookup_uid(0x7FFFFFF0));
#else
    CHECK(u.sid.empty());
#endif
    CHECK(!os::config_dir().empty()); CHECK(!os::cache_dir().empty());
#if defined(FORGE_OS_WINDOWS)
    std::printf("  full name '%s', %zu groups\n", u.full_name.c_str(), u.groups.size()); CHECK(!u.groups.empty());
    {   // Known Folders, as the shell resolves them (OneDrive / GPO redirection included)
        auto r = os::run({"powershell", "-NoProfile", "-Command", "[Console]::OutputEncoding=[Text.Encoding]::UTF8; foreach ($f in 'Desktop','MyDocuments','ApplicationData','LocalApplicationData') { [Environment]::GetFolderPath($f) }"});
        CHECK(r); std::vector<std::string> want; std::string cur;
        if (r) for (char c : r->out) { if (c == '\n') { if (!cur.empty()) want.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
        CHECK(want.size() == 4);
        if (want.size() == 4) {
            CHECK_MSG(os::user_directory(os::user_dir::desktop) == want[0], "%s vs %s", os::user_directory(os::user_dir::desktop).c_str(), want[0].c_str());
            CHECK(os::user_directory(os::user_dir::documents) == want[1]); CHECK(os::config_dir() == want[2]); CHECK(os::user_directory(os::user_dir::state) == want[3]);
        }
    }
#elif !defined(__APPLE__)
    {   // user-dirs.dirs wins over the environment and the defaults
        auto cfg = os::join(os::temp_dir(), "forge_userdirs_" + std::to_string(os::pid())); os::mkdirs(cfg);
        CHECK(os::write_file(os::join(cfg, "user-dirs.dirs"), "# written by xdg-user-dirs-update\nXDG_DESKTOP_DIR=\"$HOME/Schreibtisch\"\n  XDG_DOWNLOAD_DIR=\"/srv/dl\"\nXDG_DOCUMENTS_DIR=\"$HOME/My \\\"Docs\\\"\"\nXDG_MUSIC_DIR=relative\n"));
        auto old = os::getenv("XDG_CONFIG_HOME"); bool had = os::has_env("XDG_CONFIG_HOME"); auto oldd = os::getenv("XDG_DESKTOP_DIR"); bool hadd = os::has_env("XDG_DESKTOP_DIR");
        os::setenv("XDG_CONFIG_HOME", cfg); os::setenv("XDG_DESKTOP_DIR", "/env/desktop");
        auto h = os::home_dir();
        CHECK(os::user_directory(os::user_dir::desktop) == h + "/Schreibtisch"); CHECK(os::user_directory(os::user_dir::downloads) == "/srv/dl");
        CHECK(os::user_directory(os::user_dir::documents) == h + "/My \"Docs\"");
        CHECK(os::write_file(os::join(cfg, "user-dirs.dirs"), "XDG_DOWNLOAD_DIR=\"/srv/dl\"\n")); CHECK(os::user_directory(os::user_dir::desktop) == "/env/desktop");
        if (had) os::setenv("XDG_CONFIG_HOME", old); else os::unsetenv("XDG_CONFIG_HOME");
        if (hadd) os::setenv("XDG_DESKTOP_DIR", oldd); else os::unsetenv("XDG_DESKTOP_DIR");
        os::remove_all(cfg);
    }
#endif
}
TEST(time_civil_and_iso) {
    CHECK(os::format_iso8601(0) == "1970-01-01T00:00:00.000Z");
    CHECK(os::format_iso8601(951782400LL * 1000000000LL, 0, 0) == "2000-02-29T00:00:00Z");
    CHECK(os::format_iso8601(1793318400123000000LL, 11 * 3600) == "2026-10-30T11:00:00.123+11:00");
    auto p = os::parse_iso8601("2026-10-30T11:00:00.123+11:00"); CHECK(p); CHECK(*p == 1793318400123000000LL);
    CHECK(*os::parse_iso8601("1969-12-31T23:59:59Z") == -1000000000LL);
    CHECK(!os::parse_iso8601("2026-13-01")); CHECK(!os::parse_iso8601("nope"));
    auto c = os::civil_from_unix_ns(0); CHECK(c.weekday == 4 && c.year == 1970);
    auto now = os::unix_time_ns(); CHECK(*os::parse_iso8601(os::format_iso8601(now, 0, 9)) == now);
    CHECK(*os::parse_iso8601(os::format_local_iso8601(now, 9)) == now);
}
TEST(timer_queue_runs) {
    std::atomic<int> once{0}, periodic{0};
    {
        os::timer_queue q;
        q.after(10, [&] { ++once; });
        auto id = q.every(5, [&] { ++periodic; });
        auto cancelled = q.after(10000, [&] { once += 100; });
        CHECK(q.cancel(cancelled));
        os::sleep_ns(120'000'000);
        q.cancel(id);
    }
    CHECK(once == 1); CHECK(periodic >= 3);
}
TEST(sysinfo_ext) {
    auto r = os::release_info(); CHECK(!r.name.empty());
    auto m = os::memory(); CHECK(m.total > 0);
    std::printf("  %s (%s %s) mem total %llu MiB avail %llu free %llu swap %llu/%llu cgroup %llu/%llu\n", r.pretty.c_str(), r.kernel.c_str(), r.kernel_version.c_str(),
                (unsigned long long)(m.total >> 20), (unsigned long long)(m.available >> 20), (unsigned long long)(m.free >> 20), (unsigned long long)(m.swap_free >> 20),
                (unsigned long long)(m.swap_total >> 20), (unsigned long long)(m.cgroup_usage >> 20), (unsigned long long)(m.cgroup_limit >> 20));
    CHECK(m.free > 0); CHECK(m.free <= m.total); CHECK(m.swap_free <= m.swap_total);
#if defined(FORGE_OS_WINDOWS)
    CHECK(r.version == "10" || r.version == "11"); CHECK(m.free <= m.available);
    CHECK(m.swap_total > 0);   // the page file
#elif defined(__linux__)
    if (os::stat("/sys/fs/cgroup/cgroup.controllers").exists) { auto d = os::detail::cgroup2_dir(); CHECK(!d.empty()); CHECK(os::stat(d).is_dir); }
#endif
    auto u = os::self_usage(); CHECK(u);
    CHECK(u->max_rss > 0); CHECK(u->minor_faults + u->major_faults > 0);
    {   // current_rss grows with touched memory, on every OS
        std::uint64_t r0 = os::current_rss(); CHECK(r0 > 0);
        std::vector<char> big(64u << 20); volatile char* vp = big.data(); for (std::size_t i = 0; i < big.size(); i += 4096) vp[i] = char(i | 1);
        std::uint64_t r1 = os::current_rss(); CHECK(r1 >= r0 + (32ull << 20)); CHECK(r1 < m.total); CHECK(vp[4096] != 0);
    }
    CHECK(*os::uptime_seconds() > 0); CHECK(m.available > 0);
    CHECK(os::load_average());
    auto a = os::per_cpu_times(); CHECK(a); CHECK(!a->empty()); CHECK(a->size() >= 1u);
    double x = os::cpu_usage((*a)[0], (*a)[0]); CHECK(x == 0);
    {   // the counters advance, and a busy loop shows up as busy time somewhere
        double busy0 = 0, all0 = 0; for (auto& t : *a) { CHECK(t.idle >= 0 && t.user >= 0 && t.system >= 0); busy0 += t.total() - t.idle; all0 += t.total(); }
        CHECK(all0 > 0);
        volatile std::uint64_t spin = 0; auto t0 = os::unix_time_ns(); while (os::unix_time_ns() - t0 < 300'000'000) spin = spin + 1;
        auto b = os::per_cpu_times(); CHECK(b); CHECK(b->size() == a->size());
        double busy1 = 0, all1 = 0; for (auto& t : *b) { busy1 += t.total() - t.idle; all1 += t.total(); }
        CHECK(all1 > all0); CHECK(busy1 > busy0);
    }
}
// ---------------------------------------------------------------- audit W1-B regressions
TEST(fs_non_ascii_paths) {
    auto d = fs::make_temp_dir("forge-\xC3\xA9-\xE6\x97\xA5-"); CHECK(d); if (!d) return;   // "é", "日"
    CHECK(os::stat(*d).is_dir);
    auto p = *d + "/\xE6\x97\xA5\xE6\x9C\xAC-caf\xC3\xA9.txt";
    CHECK(fs::write_file_atomic(p, "one")); CHECK(fs::write_file_atomic(p, "two")); CHECK(os::read_file(p).value == "two");
    auto l = os::list_dir(*d); CHECK(l && l->size() == 1 && (*l)[0].name == "\xE6\x97\xA5\xE6\x9C\xAC-caf\xC3\xA9.txt");   // no stray temp file
    { auto k = fs::file_lock::acquire(*d + "/l\xC3\xA9.lock"); CHECK(k); }
    CHECK(fs::exists(*d + "/l\xC3\xA9.lock"));   // created under its real (UTF-8) name, not a byte-widened one
    auto t = fs::make_temp_file("t\xC3\xA9-", ".tmp"); CHECK(t); if (t) { CHECK(os::stat(*t).is_file); CHECK(t->find("t\xC3\xA9-") != std::string::npos); fs::remove_all(*t); }
#if defined(FORGE_OS_WINDOWS)
    CHECK(fs::write_file_atomic(*d + "/ro.txt", "r", 0444)); CHECK(!os::file::open(*d + "/ro.txt", os::wr));
    SetFileAttributesW(os::widen(*d + "/ro.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
#else
    CHECK(fs::write_file_atomic(*d + "/ro.txt", "r", 0600)); CHECK((*fs::mode(*d + "/ro.txt") & 0077) == 0);
#endif
    CHECK(fs::remove_all(*d)); CHECK(!fs::exists(*d));
}
TEST(mount_unescape_is_safe) {
    CHECK(fs::mount_unescape("/mnt/with\\040space") == "/mnt/with space");
    CHECK(fs::mount_unescape("a\\134b") == "a\\b");
    CHECK(fs::mount_unescape("bad\\9zz") == "bad\\9zz"); CHECK(fs::mount_unescape("end\\04") == "end\\04"); CHECK(fs::mount_unescape("\\") == "\\");
}
TEST(classify_by_domain) {
    CHECK(os::classify(os::errno_error(ENOENT, "x")) == os::os_errc::not_found);
    CHECK(os::classify(os::errno_error(EEXIST, "x")) == os::os_errc::already_exists);   // 17 = ERROR_NOT_SAME_DEVICE as Win32
    CHECK(os::classify(os::errno_error(EINVAL, "x")) == os::os_errc::invalid_argument);
#if defined(EAI_NONAME)
    CHECK(os::classify(os::error{EAI_NONAME, "x", os::err_domain::gai}) == os::os_errc::not_found);
    CHECK(os::classify_gai(EAI_AGAIN) == os::os_errc::would_block);
#endif
    auto missing = fs::file_size("definitely/not/here-forge"); CHECK(!missing); CHECK(os::classify(missing.err) == os::os_errc::not_found);
    auto parse = os::parse_iso8601("nope"); CHECK(os::classify(parse.err) == os::os_errc::invalid_argument);
#if defined(FORGE_OS_WINDOWS)
    CHECK(os::classify(os::error{int(ERROR_FILE_NOT_FOUND), "x"}) == os::os_errc::not_found);
    CHECK(os::classify(os::error{int(ERROR_FILE_NOT_FOUND), "x", os::err_domain::win32}) == os::os_errc::not_found);
    struct { unsigned long code; os::os_errc k; } t[] = {
        {ERROR_HANDLE_EOF, os::os_errc::io}, {11001, os::os_errc::not_found}, {11002, os::os_errc::would_block}, {ERROR_SEM_TIMEOUT, os::os_errc::timed_out},
        {ERROR_PIPE_BUSY, os::os_errc::busy}, {ERROR_NOT_FOUND, os::os_errc::not_found}, {10052, os::os_errc::connection_reset},
        {ERROR_NETWORK_UNREACHABLE, os::os_errc::network_unreachable}, {ERROR_CANT_RESOLVE_FILENAME, os::os_errc::too_many_links},
        {10047, os::os_errc::not_supported}, {ERROR_COMMITMENT_LIMIT, os::os_errc::out_of_memory}, {ERROR_ALREADY_EXISTS, os::os_errc::already_exists}};
    for (auto& e : t) CHECK_MSG(os::classify_win32(e.code) == e.k, "%lu -> %s", e.code, os::name(os::classify_win32(e.code)));
#else
    CHECK(os::classify(os::error{ENOENT, "x"}) == os::os_errc::not_found);
    CHECK(os::classify(os::error{2, "x", os::err_domain::win32}) == os::os_errc::other);
#endif
}
TEST(timezone_and_offsets) {
    auto tz = os::timezone_name(); std::printf("  timezone_name: '%s'\n", tz.c_str());
#if defined(FORGE_OS_WINDOWS) || defined(__APPLE__) || defined(FORGE_OS_ANDROID) || defined(FORGE_OS_FREEBSD)
    CHECK(!tz.empty());
#endif
#if defined(FORGE_OS_WINDOWS)
    CHECK(std::string(os::windows_zone_to_iana("W. Europe Standard Time")) == "Europe/Berlin"); CHECK(!os::windows_zone_to_iana("No Such Zone"));
#endif
    int o = os::utc_offset_seconds(-86400LL * 365 * 300); CHECK(o >= -16 * 3600 && o <= 16 * 3600);   // year 1670: 0 or a sane offset, no garbage
}
TEST(pipe_read_timeout) {
    auto p = os::make_pipe(); CHECK(p); if (!p) return;
    auto t0 = os::monotonic_ns(); CHECK(p->read.read_some(100, 50).empty()); CHECK(os::monotonic_ns() - t0 >= 40'000'000ull);
    CHECK(p->write.write_all("hello")); CHECK(p->read.read_some(100, 2000) == "hello");
    std::thread w([&] { os::sleep_ns(30'000'000); p->write.write_all(" world"); p->write.close(); });
    std::string all; for (std::string c; !(c = p->read.read_some(100, 5000)).empty();) all += c;
    w.join(); CHECK(all == " world");
}
TEST(console_bits) {
    CHECK(os::console::paint("x", os::console::color::red, false) == "x");
    CHECK(os::console::paint("x", os::console::color::red, true) == "\x1b[31mx\x1b[0m");
    auto s = os::console::size(); CHECK(s.value.cols > 0);
    (void)os::console::is_tty(os::console::stream::out);
}

// ---- service control wave 2: SCM parameters, Task Scheduler XML, rc.d scripts (rendering everywhere) ----
namespace svc = forge::os::service;
static std::size_t count_of(const std::string& h, const std::string& n) { std::size_t c = 0; for (auto p = h.find(n); p != std::string::npos; p = h.find(n, p + 1)) ++c; return c; }
TEST(service_render_windows) {
    svc::service_spec s; s.name = "forge-test-svc"; s.description = "Forge test"; s.argv = {"C:\\Program Files\\Forge\\forge.exe", "a b", "q\"x", "tail\\"};
    s.working_dir = "C:\\srv"; s.env = {{"A", "1"}};
    auto c = svc::render_scm(s);
    CHECK(c.command_line == "\"C:\\Program Files\\Forge\\forge.exe\" \"a b\" \"q\\\"x\" tail\\");   // argv[0] quoted: no unquoted-path hijack
    CHECK(c.display_name == "Forge test" && c.description == "Forge test" && c.auto_start && c.restart);
    CHECK(c.environment.size() == 2 && c.environment[0] == "A=1" && c.environment[1] == "FORGE_SERVICE_DIR=C:\\srv");
    s.argv[0] = "C:\\Forge\\forge.exe"; s.argv[3] = "x y\\"; CHECK(svc::render_scm(s).command_line == "C:\\Forge\\forge.exe \"a b\" \"q\\\"x\" \"x y\\\\\"");
    // Task XML: markup in any field stays text
    auto t = s; t.env.clear(); t.description = "x</Description></RegistrationInfo><Actions><Exec><Command>evil.exe</Command></Exec></Actions>";
    t.argv = {"C:\\a&b\\p.exe", "<x>", "it's \"q\""}; t.working_dir = "C:\\w&<>";
    auto x = svc::render_task_xml(t, "S-1-5-21-1\"<evil/>");
    CHECK(count_of(x, "<Exec>") == 1); CHECK(count_of(x, "<Command>") == 1); CHECK(x.find("evil.exe</Command>") == std::string::npos);
    CHECK(x.find("<Description>x&lt;/Description&gt;&lt;/RegistrationInfo&gt;") != std::string::npos);
    CHECK(x.find("<Command>C:\\a&amp;b\\p.exe</Command>") != std::string::npos);
    CHECK(x.find("<Arguments>&lt;x&gt; &quot;it&apos;s \\&quot;q\\&quot;&quot;</Arguments>") != std::string::npos);
    CHECK(x.find("<WorkingDirectory>C:\\w&amp;&lt;&gt;</WorkingDirectory>") != std::string::npos);
    CHECK(x.find("<UserId>S-1-5-21-1&quot;&lt;evil/&gt;</UserId>") != std::string::npos); CHECK(count_of(x, "<LogonTrigger>") == 1);
    CHECK(x.find("<RestartOnFailure>") != std::string::npos);
    t.start_at_login = false; t.restart_on_failure = false; x = svc::render_task_xml(t, "S-1-5-21-1");
    CHECK(x.find("<LogonTrigger>") == std::string::npos); CHECK(x.find("<RestartOnFailure>") == std::string::npos);
    t.description = "a\nb"; CHECK(!svc::validate(t)); CHECK(svc::render_task_xml(t, "S").find("a\nb") == std::string::npos);
    // env through PowerShell: literals single-quoted with every quote variant doubled, nothing on a command line
    CHECK(svc::ps_quote_("a'b") == "'a''b'"); CHECK(svc::ps_quote_("$(evil) `n") == "'$(evil) `n'");
    CHECK(svc::ps_quote_("x\xE2\x80\x99y") == "'x\xE2\x80\x99\xE2\x80\x99y'"); CHECK(svc::ps_quote_("\xE2\x80\x98") == "'\xE2\x80\x98\xE2\x80\x98'");
    CHECK(svc::ps_encode_("A") == "QQA="); CHECK(svc::ps_encode_("ab") == "YQBiAA=="); CHECK(svc::ps_encode_("\xC3\xA9") == "6QA=");
    CHECK(svc::ps_encode_("\xF0\x9F\x98\x80") == "PdgA3g==");   // U+1F600 as a surrogate pair
    auto e = s; e.env = {{"K", "v'; Remove-Item C:\\ -Recurse; '"}};
    auto ps = svc::task_powershell_script_(e);
    CHECK(ps.find("SetEnvironmentVariable('K','v''; Remove-Item C:\\ -Recurse; ''')") != std::string::npos);
    CHECK(ps.find("$i.Arguments='\"a b\" \"q\\\"x\" \"x y\\\\\"'") != std::string::npos);
    auto xe = svc::render_task_xml(e, "S"); CHECK(xe.find("<Command>powershell.exe</Command>") != std::string::npos);
    CHECK(xe.find("-EncodedCommand " + svc::ps_encode_(ps) + "</Arguments>") != std::string::npos); CHECK(xe.find("Remove-Item") == std::string::npos);
}
TEST(service_render_rcd) {
    svc::service_spec s; s.name = "forge-test.svc"; s.description = "it's"; s.argv = {"/usr/local/bin/forge", "a b", "'; rm -rf /; '", "$(id)", "`id`", "${pidfile}"};
    s.working_dir = "/srv/my dir'"; s.env = {{"GOOD", "x'y $HOME"}, {"BAD-NAME", "1"}};
    CHECK(svc::rc_name("forge-test.svc") == "forge_test_svc"); CHECK(svc::rc_name("9x") == "svc_9x");
    auto r = svc::render_rcd_script(s);
    CHECK(r.rfind("#!/bin/sh\n", 0) == 0); CHECK(r.find("\nname=forge_test_svc\n") != std::string::npos); CHECK(r.find("\nrcvar=forge_test_svc_enable\n") != std::string::npos);
    CHECK(r.find("\ndesc='it'\\''s'\n") != std::string::npos); CHECK(r.find("BAD-NAME") == std::string::npos);
    CHECK(r.find("\n\tcd -- '/srv/my dir'\\''' || return 1\n") != std::string::npos); CHECK(r.find("\n\texport GOOD='x'\\''y $HOME'\n") != std::string::npos);
    CHECK(r.find("-r -R 5") != std::string::npos); auto nr = s; nr.restart_on_failure = false; CHECK(svc::render_rcd_script(nr).find("-r -R 5") == std::string::npos);
    CHECK(svc::render_systemd_unit(s).find("WantedBy=default.target") != std::string::npos);
    auto sys = s; sys.scope = svc::service_scope::system; CHECK(svc::render_systemd_unit(sys).find("WantedBy=multi-user.target") != std::string::npos);
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
    {   // what rc.subr does: the assignment, then eval of "$command $command_args"; and the precmd's exports
        auto ln = r.substr(r.find("\ncommand_args=") + 1); ln = ln.substr(0, ln.find('\n'));
        auto ex = r.substr(r.find("\texport GOOD=") + 1); ex = ex.substr(0, ex.find('\n'));
        os::spawn_spec sp; sp.argv = {"/bin/sh", "-c", "pidfile=/var/run/p.pid; " + ln + "; eval \"set -- $command_args\"; for a; do printf '%s\\n' \"$a\"; done; " + ex + "; printf '%s\\n' \"$GOOD\""};
        auto o = os::spawn_wait(sp); CHECK(o && o->exit_code == 0);
        if (o) CHECK_MSG(o->out == "-P\n/var/run/p.pid\n-r\n-R\n5\n-S\n-T\nforge_test_svc\n--\n/usr/local/bin/forge\na b\n'; rm -rf /; '\n$(id)\n`id`\n${pidfile}\nx'y $HOME\n", "%s", o->out.c_str());
    }
#endif
#if defined(FORGE_OS_FREEBSD)
    {   // rc.d written to a directory, not registered; names that are not shell identifiers refused
        auto d = fs::make_temp_dir(); CHECK(d); auto t = s; t.dir = *d;
        CHECK(!svc::install(t)); t.env.erase("BAD-NAME");
        CHECK(svc::install(t)); CHECK(fs::is_file(svc::unit_path(t))); CHECK(svc::query(t) == svc::service_state::stopped);
        CHECK(svc::uninstall(t)); CHECK(svc::query(t) == svc::service_state::not_installed); fs::remove_all(*d);
        t.dir.clear(); t.scope = svc::service_scope::user; CHECK(os::is_unsupported(svc::install(t).err));
    }
    // real rc.d service (root only, opt-in): daemon(8) running sleep, started, queried, stopped, removed
    if (::geteuid() == 0 && os::getenv("FORGE_TEST_RCD") == "1") {
        svc::service_spec d; d.name = "forge-test-rcd-" + std::to_string(os::pid()); d.argv = {"/bin/sleep", "300"}; d.scope = svc::service_scope::system;
        d.working_dir = "/tmp"; d.env = {{"FORGE_X", "a'b"}};
        struct cleanup { svc::service_spec& s; ~cleanup() { svc::uninstall(s); } } guard{d};
        CHECK(svc::install(d)); CHECK(svc::query(d) == svc::service_state::stopped);
        auto st = svc::start(d); CHECK_MSG(st, "%s", st.err.what.c_str());
        auto i = svc::query_info(d); CHECK(i.state == svc::service_state::running); CHECK(i.pid > 0);
        std::printf("  rc.d service %s running, daemon pid %lld\n", d.name.c_str(), static_cast<long long>(i.pid));
        CHECK(svc::stop(d)); CHECK(svc::query(d) == svc::service_state::stopped);
        CHECK(svc::uninstall(d)); CHECK(svc::query(d) == svc::service_state::not_installed); CHECK(!fs::exists(svc::unit_path(d)));
    }
#endif
}
#if defined(FORGE_OS_WINDOWS)
TEST(service_windows_scm_query) {   // read-only SCM paths (no admin needed); installing a system service is CI / elevated only
    svc::service_spec s; s.name = "forge-test-no-such-svc"; s.argv = {"C:\\x.exe"}; s.scope = svc::service_scope::system;
    CHECK(svc::query(s) == svc::service_state::not_installed); CHECK(svc::uninstall(s));
    s.name = "EventLog"; auto i = svc::query_info(s); CHECK(i.state == svc::service_state::running); CHECK(i.pid > 0);
    if (!os::is_elevated()) { s.name = "forge-test-denied"; auto r = svc::install(s); CHECK(!r); CHECK(r.err.code == ERROR_ACCESS_DENIED); }
    s.name = "forge-test-bat"; s.argv = {"C:\\x.bat", "a"}; CHECK(!svc::install(s));
    svc::service_spec t; t.name = "forge-test-dir"; t.argv = {"C:\\Windows\\System32\\cmd.exe", "/c", "exit 0"};
    auto d = fs::make_temp_dir(); CHECK(d); t.dir = *d;
    CHECK(svc::install(t)); auto txt = os::read_file(svc::unit_path(t)); CHECK(txt && txt->size() > 2 && (*txt)[0] == '\xFF' && (*txt)[1] == '\xFE');
    CHECK(svc::query(t) == svc::service_state::stopped); CHECK(svc::uninstall(t)); CHECK(svc::query(t) == svc::service_state::not_installed);
    fs::remove_all(*d);
}
TEST(service_windows_task) {   // a real per-user scheduled task: install, start, query, stop, env + exit code, remove
    std::mt19937_64 rng(std::random_device{}() ^ os::monotonic_ns());
    svc::service_spec s; s.name = "forge-test-" + std::to_string(rng() % 1000000000ull); s.description = "FORGE test task (safe to delete)";
    s.argv = {"PING.EXE", "-n", "30", "127.0.0.1"}; s.start_at_login = false; s.working_dir = os::getenv("SystemRoot", "C:\\Windows");
    struct cleanup { svc::service_spec& s; ~cleanup() { svc::uninstall(s); } } guard{s};
    auto wait_for = [&](auto pred) { for (int i = 0; i < 300; ++i) { auto q = svc::query_info(s); if (pred(q)) return q; os::sleep_ns(100'000'000); } return svc::query_info(s); };
    CHECK(svc::query(s) == svc::service_state::not_installed);
    auto in = svc::install(s); CHECK_MSG(in, "%s", in.err.what.c_str()); if (!in) return;
    CHECK(svc::query(s) == svc::service_state::stopped);
    auto st = svc::start(s); CHECK_MSG(st, "%s", st.err.what.c_str());
    auto q = wait_for([](const svc::service_info& i) { return i.state == svc::service_state::running; });
    CHECK(q.state == svc::service_state::running); CHECK(q.pid > 0);
    CHECK(svc::start(s));   // already running: IgnoreNew, not an error
    auto sp = svc::stop(s); CHECK_MSG(sp, "%s", sp.err.what.c_str());
    q = svc::query_info(s); CHECK_MSG(q.state == svc::service_state::stopped, "state %s result 0x%lx", svc::state_name(q.state), (unsigned long)q.exit_code);
    CHECK(q.pid == 0);
    // environment through the PowerShell wrapper (with quoting traps), exit code reported as failed
    s.env = {{"FORGE_T", "7"}, {"FORGE_Q", "it's $(x) \xE2\x80\x99q\" `n"}}; s.argv = {"cmd.exe", "/d", "/c", "exit %FORGE_T%"};
    in = svc::install(s); CHECK_MSG(in, "%s", in.err.what.c_str());
    CHECK(svc::start(s));
    os::sleep_ns(500'000'000);
    q = wait_for([](const svc::service_info& i) { return i.state != svc::service_state::running; });
    CHECK_MSG(q.state == svc::service_state::failed && q.exit_code == 7, "state %s result 0x%lx", svc::state_name(q.state), (unsigned long)q.exit_code);
    // stop ends the wrapper's child too (Task Scheduler alone would leave it running)
    s.argv = {"PING.EXE", "-n", "60", "127.0.0.1"}; CHECK(svc::install(s)); CHECK(svc::start(s));
    q = wait_for([](const svc::service_info& i) { return i.state == svc::service_state::running; });
    CHECK(q.state == svc::service_state::running);
    os::sleep_ns(1'500'000'000);   // let PowerShell start the child
    auto kids = q.pid ? svc::open_descendants_(DWORD(q.pid)) : std::vector<HANDLE>{}; CHECK(!kids.empty());
    CHECK(svc::stop(s));
    for (HANDLE h : kids) { CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0); CloseHandle(h); }
    CHECK(svc::uninstall(s)); CHECK(svc::query(s) == svc::service_state::not_installed); CHECK(svc::uninstall(s));
}
#endif
TEST(load_average_emulation) {   // Windows: sampler started by the first call; elsewhere the kernel's average
    auto a = os::load_average(); CHECK(a); if (!a) return;
    CHECK(a->one >= 0 && a->five >= 0 && a->fifteen >= 0);
#if defined(FORGE_OS_WINDOWS)
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<bool> stop{false}; std::vector<std::thread> spin;
    for (unsigned i = 0; i < n; ++i) spin.emplace_back([&] { volatile std::uint64_t x = 0; while (!stop.load(std::memory_order_relaxed)) x = x + 1; });
    os::sleep_ns(11'000'000'000ull);   // two 5 s ticks
    auto b = os::load_average(); stop = true; for (auto& t : spin) t.join();
    CHECK(b); if (!b) return;
    CHECK(b->one >= 0 && b->five >= 0 && b->fifteen >= 0);
    CHECK_MSG(b->one > a->one + 0.05 * n,"before %.2f after %.2f (n=%u)", a->one, b->one, n);
    CHECK(b->one - a->one > b->fifteen - a->fifteen);   // the 1-minute average reacts fastest
#endif
}
