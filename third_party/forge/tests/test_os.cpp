// forge::os: the unified OS layer, exercised on whatever OS this runs on.
#include "check.hpp"
#include <forge/os/os.hpp>
#include <forge/os/oserror.hpp>
#include <atomic>
using namespace forge;

static std::string scratch() { auto d = os::join(os::temp_dir(), "forge_os_" + std::to_string(os::pid())); os::remove_all(d); CHECK(os::mkdirs(d)); return d; }

TEST(identity_and_sysinfo) {
    auto s = os::sysinfo();
    std::printf("  %s/%s cpus=%u ram=%llu MiB avail=%llu MiB page=%zu host=%s\n", s.os.c_str(), s.arch.c_str(), s.logical_cpus,
                (unsigned long long)(s.total_ram >> 20), (unsigned long long)(s.avail_ram >> 20), s.page, s.hostname.c_str());
    CHECK(s.logical_cpus >= 1); CHECK(s.total_ram > 0); CHECK(s.page >= 4096); CHECK(s.os != "other");
}
TEST(files_and_fs) {
    auto d = scratch(); auto p = os::join(d, "a.bin");
    CHECK(os::write_file(p, "hello world"));
    auto r = os::read_file(p); CHECK(r); CHECK(r.value == "hello world");
    auto st = os::stat(p); CHECK(st.exists && st.is_file && st.size == 11 && st.mtime_ns > 0);
    { auto f = os::file::open(p, os::rdwr); CHECK(f); char b[5] = {}; CHECK(f->pread(b, 5, 6).value == 5); CHECK(std::string(b, 5) == "world");
      CHECK(f->pwrite("W", 1, 6)); CHECK(f->resize(8)); CHECK(f->sync()); CHECK(f->size().value == 8); }
    CHECK(os::read_file(p).value == "hello Wo");
    { auto f = os::file::open(p, os::wr | os::append); CHECK(f); CHECK(f->write_all("!!", 2)); }
    CHECK(os::read_file(p).value == "hello Wo!!");
    CHECK(!os::file::open(p, os::wr | os::excl));
    CHECK(os::mkdirs(os::join(d, "x/y/z")));
    CHECK(os::rename(p, os::join(d, "x/b.bin")));
    auto l = os::list_dir(os::join(d, "x")); CHECK(l); CHECK(l.value.size() == 2);
    bool sawdir = false; for (auto& e : l.value) if (e.name == "y") sawdir = e.is_dir; CHECK(sawdir);
    auto bad = os::file::open(os::join(d, "nope"), os::rd); CHECK(!bad); CHECK(!bad.err.what.empty());
    CHECK(os::remove_all(d)); CHECK(!os::exists(d));
}
TEST(paths) {
    CHECK(os::basename("a/b/c.txt") == "c.txt"); CHECK(os::dirname("a/b/c.txt") == "a/b"); CHECK(os::dirname("c") == ".");
    CHECK(os::join("a", "b") == "a/b"); CHECK(os::join("a/", "b") == "a/b");
    CHECK(!os::cwd().empty()); CHECK(!os::exe_path().empty()); CHECK(os::exists(os::exe_path()));
    CHECK(!os::temp_dir().empty()); CHECK(os::stat(os::temp_dir()).is_dir);
}
TEST(mmap_and_memory) {
    auto d = scratch(); auto p = os::join(d, "m.bin");
    CHECK(os::write_file(p, std::string(10000, 'x')));
    { auto m = os::mapping::map_file(p, os::prot_read | os::prot_write); CHECK(m); CHECK(m->size() == 10000); m->data()[5] = 'Y'; CHECK(m->flush()); }
    { auto m = os::mapping::map_file(p); CHECK(m); CHECK(m->data()[5] == 'Y' && m->data()[9999] == 'x'); }
    auto a = os::mapping::anonymous(1 << 20); CHECK(a); a->data()[(1 << 20) - 1] = 7; CHECK(a->data()[(1 << 20) - 1] == 7);
    std::size_t pg = os::page_size(); void* r = os::reserve(pg * 16); CHECK(r);
    CHECK(os::commit(r, pg * 2)); static_cast<char*>(r)[pg] = 1; CHECK(os::protect(r, pg * 2, os::prot_read)); os::release(r, pg * 16);
    void* al = os::aligned_alloc(4096, 256); CHECK(al && (reinterpret_cast<std::uintptr_t>(al) % 256) == 0); os::aligned_free(al);
    char buf[4096]; os::lock_memory(buf, sizeof buf); os::unlock_memory(buf, sizeof buf);   // may fail without privilege; must not crash
    os::remove_all(d);
}
TEST(time_and_sleep) {
    auto t0 = os::monotonic_ns(); os::sleep_ns(20'000'000); auto dt = os::monotonic_ns() - t0;
    CHECK(dt >= 15'000'000 && dt < 2'000'000'000);
    CHECK(os::wall_ns() > 1'700'000'000'000'000'000LL);
    // Busy-spin >= 100 ms of wall time: Windows process times tick at ~15.6 ms, so a short loop can read 0.
    volatile double x = 0; auto s0 = os::monotonic_ns();
    while (os::monotonic_ns() - s0 < 100'000'000) for (int i = 0; i < 10'000; ++i) x = x + i;
    CHECK(os::cpu_time_ns() > 0);
}
TEST(threads_and_sync) {
    os::mutex m; os::condvar cv; int counter = 0; std::atomic<std::uint64_t> other{0};
    { std::vector<os::thread> ts;
      for (int t = 0; t < 4; ++t) ts.emplace_back([&] { other = os::thread_id(); for (int i = 0; i < 10000; ++i) { os::lock_guard g(m); ++counter; } }, "forge-w");
    }
    CHECK(counter == 40000); CHECK(other != 0 && other != os::thread_id());
    os::semaphore s; os::thread t([&] { os::sleep_ns(5'000'000); s.post(); });
    CHECK(s.wait_for(2'000'000'000)); CHECK(!s.wait_for(10'000'000)); t.join();
    { os::lock_guard g(m); CHECK(!cv.wait_for(m, 5'000'000)); }
    os::set_thread_name("forge-test"); os::set_thread_priority(os::thread_priority::normal); os::yield();
}
TEST(env_and_dl) {
    CHECK(os::setenv("FORGE_OS_T", "v1")); CHECK(os::getenv("FORGE_OS_T") == "v1"); CHECK(os::has_env("FORGE_OS_T"));
    CHECK(os::unsetenv("FORGE_OS_T")); CHECK(os::getenv("FORGE_OS_T", "dflt") == "dflt");
    auto self = os::library::open(""); CHECK(self);
#if defined(_WIN32)
    auto k = os::library::open("kernel32.dll"); CHECK(k); CHECK(k->symbol("GetTickCount") != nullptr);
#elif defined(__APPLE__)
    auto k = os::library::open("/usr/lib/libSystem.B.dylib"); CHECK(k); CHECK(k->symbol("malloc") != nullptr);
#else
    auto k = os::library::open(
#if defined(__ANDROID__)
        "libc.so"
#elif defined(__FreeBSD__)
        "libc.so.7"   // /usr/lib/libc.so is a linker script that dlopen rejects
#else
        "libc.so.6"
#endif
    ); CHECK(k); CHECK(k->symbol("malloc") != nullptr);
#endif
    CHECK(!os::library::open("definitely_missing_forge_lib"));
    CHECK(os::library::decorate("x").find('x') != std::string::npos);
}
TEST(entropy) {
    std::uint8_t a[64] = {}, b[64] = {}; CHECK(os::entropy(a, 64)); CHECK(os::entropy(b, 64)); CHECK(std::memcmp(a, b, 64) != 0);
}
static std::atomic<int> g_sig{0};
TEST(signals) {
    CHECK(os::on_signal(os::sig::interrupt, [](int) { g_sig = 1; }));
    os::ignore_sigpipe();
#if !defined(_WIN32)
    ::raise(SIGINT); CHECK(g_sig == 1);
#endif
    os::on_signal(os::sig::interrupt, nullptr);
}
TEST(process) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    CHECK(!os::run({"true"}));
#elif defined(_WIN32)
    auto r = os::run({"cmd", "/c", "echo forge&& exit 3"}); CHECK(r); CHECK(r->exit_code == 3); CHECK(r->out.find("forge") != std::string::npos);
#else
    auto r = os::run({"sh", "-c", "echo forge; echo err >&2; exit 3"}, true); CHECK(r); CHECK(r->exit_code == 3);
    CHECK(r->out.find("forge") != std::string::npos && r->out.find("err") != std::string::npos);
    CHECK(!os::run({"/nonexistent/forge-binary"}) || os::run({"/nonexistent/forge-binary"})->exit_code != 0);
#endif
    CHECK(os::pid() > 0);
}
TEST(sockets_and_poller) {
    auto l = os::socket::listen("127.0.0.1", 0); CHECK(l); int port = l->local_port(); CHECK(port > 0);
    os::poller p; std::printf("  poller backend: %s\n", p.backend());
    CHECK(p.add(l->native(), os::readable, &*l));
    auto c = os::socket::connect("127.0.0.1", port); CHECK(c);
    auto ev = p.wait(2000); CHECK(ev.size() == 1 && (ev[0].events & os::readable) && ev[0].user == &*l);
    auto s = l->accept(); CHECK(s);
    CHECK(p.add(s->native(), os::readable)); CHECK(c->send_all("ping", 4));
    ev = p.wait(2000); bool got = false; for (auto& e : ev) if (e.fd == s->native() && (e.events & os::readable)) got = true; CHECK(got);
    char b[4]; CHECK(s->recv_all(b, 4)); CHECK(std::memcmp(b, "ping", 4) == 0);
    CHECK(p.modify(s->native(), os::writable)); ev = p.wait(1000); CHECK(!ev.empty() && (ev[0].events & os::writable));
    CHECK(p.remove(s->native())); CHECK(p.size() == 1);
    CHECK(p.wait(10).empty());
    c->close(); CHECK(s->wait_readable(2000)); CHECK(s->recv(b, 4) == 0);
    CHECK(!os::socket::connect("127.0.0.1", 1, 300));
}
TEST(completion_port) {
    os::completion_port cp; std::printf("  completion backend: %s\n", cp.backend());
    auto d = scratch(); auto p = os::join(d, "c.bin"); CHECK(os::write_file(p, "0123456789"));
    // Windows IOCP needs FILE_FLAG_OVERLAPPED handles for true async; positional synchronous handles still complete.
    auto f = os::file::open(p, os::rdwr); CHECK(f);
    char b[4] = {}; CHECK(cp.read_async(*f, b, 4, 3, 42));
    os::completion c; CHECK(cp.wait(c, 2000)); CHECK(c.key == 42 && c.bytes == 4 && c.error == 0); CHECK(std::memcmp(b, "3456", 4) == 0);
    CHECK(cp.write_async(*f, "XY", 2, 0, 7)); CHECK(cp.wait(c, 2000)); CHECK(c.key == 7 && c.bytes == 2);
    CHECK(cp.post(99)); CHECK(cp.wait(c, 2000)); CHECK(c.key == 99);
    CHECK(!cp.wait(c, 20));
    cp.forget(*f); f->close(); CHECK(os::read_file(p).value == "XY23456789"); os::remove_all(d);
}
// ---------------------------------------------------------------- audit W1-B regressions
TEST(non_ascii_paths) {
    // "é" and CJK "日本": UTF-8 in, UTF-16 to the Windows APIs, UTF-8 back out.
    auto d = os::join(scratch(), "d\xC3\xA9j\xC3\xA0-\xE6\x97\xA5\xE6\x9C\xAC"); CHECK(os::mkdirs(d));
    auto p = os::join(d, "f\xC3\xA9.txt");
    CHECK(os::write_file(p, "caf\xC3\xA9")); CHECK(os::read_file(p).value == "caf\xC3\xA9");
    auto st = os::stat(p); CHECK(st.exists && st.is_file && !st.is_link && st.size == 5);
    auto l = os::list_dir(d); CHECK(l && l->size() == 1 && (*l)[0].name == "f\xC3\xA9.txt");
    CHECK(os::rename(p, os::join(d, "\xE6\x97\xA5.txt"))); CHECK(os::exists(os::join(d, "\xE6\x97\xA5.txt")));
    { auto m = os::mapping::map_file(os::join(d, "\xE6\x97\xA5.txt")); CHECK(m && m->size() == 5); }
    CHECK(os::remove_all(os::dirname(d))); CHECK(!os::exists(d));
}
TEST(stat_reparse_points_are_not_links) {
    // This repository may live under OneDrive, whose files are cloud reparse points: they are plain files.
    auto st = os::stat("CMakeLists.txt");
    if (st.exists) { CHECK(st.is_file && !st.is_link && !st.is_dir && st.size > 0); }
    auto d = os::stat("os"); if (d.exists) { CHECK(d.is_dir && !d.is_link); }
}
TEST(file_append_is_atomic_per_write) {
    auto d = scratch(); auto p = os::join(d, "a.log");
    auto a = os::file::open(p, os::wr | os::create | os::append), b = os::file::open(p, os::wr | os::append); CHECK(a && b);
    CHECK(a->write_all("a", 1)); CHECK(b->write_all("b", 1)); CHECK(a->write_all("c", 1));   // a must not overwrite b
    a->close(); b->close(); CHECK(os::read_file(p).value == "abc");
    { auto r = os::file::open(p, os::rd | os::append); CHECK(r); char x[3]; CHECK(r->read(x, 3).value == 3); CHECK(r->write_all("d", 1)); }
    CHECK(os::read_file(p).value == "abcd"); os::remove_all(d);
}
TEST(map_file_range_checks) {
    auto d = scratch(); auto p = os::join(d, "m.bin"); std::size_t g = os::allocation_granularity(); CHECK(g >= 4096);
    std::string s(g * 2 + 100, 'a'); s[g] = 'B'; CHECK(os::write_file(p, s));
    { auto m = os::mapping::map_file(p, os::prot_read, g); CHECK(m); if (m) { CHECK(m->size() == g + 100); CHECK(m->data()[0] == 'B'); } }
    CHECK(!os::mapping::map_file(p, os::prot_read, 1));                 // unaligned offset
    CHECK(!os::mapping::map_file(p, os::prot_read, g * 4));             // offset past the end
    CHECK(!os::mapping::map_file(p, os::prot_read, 0, s.size() + 1));   // range past the end
    { auto r = os::mapping::anonymous(1 << 16, os::prot_read); CHECK(r); }
    os::remove_all(d);
}
TEST(memory_and_cpu_sanity) {
    auto t = os::total_memory(), a = os::available_memory();
    std::printf("  total=%llu MiB available=%llu MiB\n", (unsigned long long)(t >> 20), (unsigned long long)(a >> 20));
    CHECK(t > 0); CHECK(a > 0); CHECK(a <= t);
#if defined(_WIN32) || defined(__linux__) || defined(__FreeBSD__)
    os::thread th([] { CHECK(os::pin_thread(0)); CHECK(!os::pin_thread(100000)); }); th.join();
#endif
    os::thread th2([] { CHECK(os::set_thread_priority(os::thread_priority::low)); }); th2.join();
}
TEST(thread_move_assign_joins) {
    std::atomic<int> done{0};
    os::thread a([&] { os::sleep_ns(20'000'000); done = 1; });
    a = os::thread([] {});   // the running thread is joined, not leaked
    CHECK(done == 1); a.join(); CHECK(!a.joinable());
}
TEST(sleep_is_fine_grained) {
    auto t0 = os::monotonic_ns(); for (int i = 0; i < 20; ++i) os::sleep_ns(100'000); auto dt = os::monotonic_ns() - t0;
    std::printf("  20 x sleep_ns(100us) = %.2f ms\n", double(dt) / 1e6);
    CHECK(dt >= 2'000'000); CHECK(dt < 200'000'000);   // a 1 ms / 15.6 ms floor would take 20-312 ms
    os::mutex m; os::condvar c; { os::lock_guard g(m); CHECK(!c.wait_for(m, 1'000'000)); }
}
TEST(socket_errors_carry_codes) {
    auto l = os::socket::listen("127.0.0.1", 0); CHECK(l); int port = l->local_port(); l->close();
    auto c = os::socket::connect("127.0.0.1", port, 2000); CHECK(!c);
    if (!c) {
        auto k = os::classify(c.err); std::printf("  connect: %s -> %s\n", c.err.what.c_str(), os::name(k));
        CHECK(c.err.code != -1); CHECK(k == os::os_errc::connection_refused || k == os::os_errc::timed_out);
    }
    auto r = os::socket::resolve("no-such-host.invalid", 80, false); CHECK(!r);
    if (!r) {
        auto k = os::classify(r.err); std::printf("  resolve: %s -> %s\n", r.err.what.c_str(), os::name(k));
        CHECK(k == os::os_errc::not_found || k == os::os_errc::would_block || k == os::os_errc::network_unreachable);
    }
    os::poller p; os::error e; CHECK(p.wait(0, e).empty()); CHECK(!e);
}
TEST(completion_port_errors_and_reuse) {
    auto d = scratch(); os::completion_port cp; os::completion c;
    // post: any key, user pointer returned untouched (~0 used to collide with the internal I/O key)
    int token = 7; CHECK(cp.post(~0ull, &token, 3)); CHECK(cp.wait(c, 2000)); CHECK(c.key == ~0ull && c.user == &token && c.bytes == 3 && !c.error);
    auto pa = os::join(d, "a.bin"), pb = os::join(d, "b.bin"); CHECK(os::write_file(pa, "AAAA")); CHECK(os::write_file(pb, "BBBB"));
    // read at end of file: 0 bytes, no error
    { auto f = os::file::open(pa, os::rd); char b[4]; CHECK(cp.read_async(*f, b, 4, 100, 1)); CHECK(cp.wait(c, 2000)); CHECK(c.key == 1 && c.bytes == 0 && c.error == 0); cp.forget(*f); }
    // an error at submission is this key's completion, not a stray one
    { auto f = os::file::open(pa, os::wr); char b[4]; bool ok = cp.read_async(*f, b, 4, 0, 2);
      if (ok) { CHECK(cp.wait(c, 2000)); CHECK(c.key == 2 && c.error != 0); } CHECK(!cp.wait(c, 20)); cp.forget(*f); }
    // handle reuse: a new file that gets the old handle value must not be served from the old file (no forget())
    for (int i = 0; i < 6; ++i) {
        auto& path = (i & 1) ? pb : pa; char b[5] = {};
        auto f = os::file::open(path, os::rd); CHECK(f);
        CHECK(cp.read_async(*f, b, 4, 0, 10 + std::uint64_t(i))); CHECK(cp.wait(c, 2000)); CHECK(c.key == 10 + std::uint64_t(i) && c.bytes == 4);
        CHECK(std::string(b) == ((i & 1) ? "BBBB" : "AAAA"));
    }
    os::remove_all(d);
}
