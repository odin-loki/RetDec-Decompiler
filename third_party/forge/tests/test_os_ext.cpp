// forge::os extension modules: one test per feature, run on whatever OS this is.
// Child-process scenarios (crash dump, sandbox, cross-process shared memory) re-exec this binary with
// FORGE_OS_EXT_CHILD=<mode> and are dispatched from a static initializer before the test runner starts.
#include "check.hpp"
#include <forge/os/all.hpp>
#include <forge/os/oserror.hpp>
#include <atomic>
using namespace forge;

static std::string scratch(const char* tag) { auto d = os::join(os::temp_dir(), std::string("forge_osx_") + tag + "_" + std::to_string(os::pid())); os::remove_all(d); os::mkdirs(d); return d; }
static void skip(const char* what, const std::string& why) { std::printf("  skip %s: %s\n", what, why.c_str()); }

// ---------------------------------------------------------------- child modes
static int child_main(const std::string& mode) {
    auto arg = mode.substr(mode.find(':') == std::string::npos ? mode.size() : mode.find(':') + 1);
    if (mode.rfind("crash_chain", 0) == 0) {   // a handler installed first must still run (exit 42) after the report
#if defined(_WIN32)
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG { ExitProcess(42); });
#else
        struct sigaction sa{}; sa.sa_handler = [](int) { ::_exit(42); }; sigemptyset(&sa.sa_mask); ::sigaction(SIGSEGV, &sa, nullptr);
#endif
        os::crash_handler::config c; c.dir = arg; c.app = "osx"; if (!os::crash_handler::install(c)) return 2;
        volatile int* p = nullptr; *p = 42; return 3;
    }
    if (mode.rfind("crash_default", 0) == 0) {   // no dir: the private per-user directory
        os::crash_handler::config c; c.app = arg; if (!os::crash_handler::install(c)) return 2;
        volatile int* p = nullptr; *p = 42; return 3;
    }
    if (mode.rfind("abort", 0) == 0) {
        os::crash_handler::config c; c.dir = arg; c.app = "osx"; if (!os::crash_handler::install(c)) return 2;
        std::abort();
    }
#if defined(_WIN32)
    if (mode == "argv")   // run_sandboxed quoting: exactly these arguments
        return __argc == 4 && std::string(__argv[1]) == "a \"b\" c\\" && std::string(__argv[2]).empty() && std::string(__argv[3]) == "x\\\"y" ? 0 : 5;
#endif
    if (mode.rfind("crash", 0) == 0) {
        os::crash_handler::config c; c.dir = arg; c.app = "osx"; if (!os::crash_handler::install(c)) return 2;
        volatile int* p = nullptr; *p = 42; return 3;
    }
    if (mode.rfind("shm", 0) == 0) {
        auto s = os::shared_memory::open(arg); if (!s) return 2;
        std::memcpy(s->data() + 64, "child-was-here", 15); return 0;
    }
    if (mode.rfind("ks:", 0) == 0) {   // keystore store-key creation race: wait for the go file, then put + get
        auto d = os::getenv("FORGE_KEYSTORE_DIR"); for (int i = 0; i < 1000 && !os::exists(os::join(d, "go")); ++i) os::sleep_ns(5000000);
        if (auto p = os::keystore::put("race", arg, "v" + arg); !p) { std::printf("put: %s\n", p.err.what.c_str()); return 2; }
        auto g = os::keystore::get("race", arg); if (!g) std::printf("get: %s\n", g.err.what.c_str()); return g && g.value == "v" + arg ? 0 : 3;
    }
#if defined(__linux__)
    if (mode.rfind("landlock", 0) == 0) {   // Landlock alone (no seccomp): its denials are EACCES, seccomp's EPERM
        if (!os::sandbox_status().landlock_abi) return 77;
        os::sandbox_policy lp; lp.deny_fs_write = mode.rfind("landlock_write", 0) == 0; lp.deny_exec = !lp.deny_fs_write;
        int fd = os::detail::ll::ruleset(lp, {}, nullptr); if (fd < 0) return 2;
        if (!os::detail::ll::restrict_self(fd)) return 3;
        ::close(fd);
        if (lp.deny_fs_write) {
            int f = ::open((arg + "/ll.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (f >= 0 || errno != EACCES) return 4;
            if (::mkdir((arg + "/lld").c_str(), 0700) == 0) return 5;
            int r = ::open("/etc/hostname", O_RDONLY); if (r < 0 && ::access("/etc/hostname", F_OK) == 0) return 6;   // reads still work
            if (r >= 0) ::close(r);
            return 0;
        }
        char* av[] = {const_cast<char*>("/bin/true"), nullptr};
        ::execv("/bin/true", av);
        return errno == EACCES ? 0 : 7;
    }
#endif
    os::sandbox_policy p;
    if (mode == "noop") return 0;
    if (mode == "sandbox_net") p.deny_network = true;
    else if (mode == "sandbox_exec") p.deny_exec = true;
    else if (mode.rfind("sandbox_write", 0) == 0) p.deny_fs_write = true;
    else return 9;
    auto st = os::apply_sandbox(p);
    if (!st) { std::fprintf(stderr, "sandbox: %s\n", st.err.what.c_str()); return os::is_unsupported(st.err) ? 77 : 2; }
#if defined(__linux__)
    {   // Landlock is stacked on seccomp when the kernel has it (deny_network needs ABI 4)
        auto s = os::sandbox_status(); const bool want = s.landlock_abi && (!p.deny_network || s.landlock_abi >= 4);
        std::printf("  %s: %s (landlock abi %d)\n", mode.c_str(), s.mechanism.c_str(), s.landlock_abi);
        if (want != (s.mechanism.find("landlock") != std::string::npos) || s.mechanism.rfind("seccomp-bpf", 0) != 0) return 8;
    }
#endif
    if (p.deny_network) {
        auto r = os::socket::listen("127.0.0.1", 0); if (r) return 4;   // must be refused
#if !defined(_WIN32)
        int u = ::socket(AF_UNIX, SOCK_STREAM, 0); if (u < 0) return 5; ::close(u);
#endif
        return 0;
    }
    if (p.deny_exec) { os::setenv("FORGE_OS_EXT_CHILD", "noop"); auto r = os::run({os::exe_path()}); return r && r->exit_code == 0 ? 4 : 0; }
    if (p.deny_fs_write) {
        if (os::write_file(os::join(arg, "nope.txt"), "x")) return 4;
        if (!os::read_file(os::exe_path())) return 5;
        return 0;
    }
    return 9;
}
static const int child_dispatch = [] {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto m = os::getenv("FORGE_OS_EXT_CHILD"); if (m.empty()) return 0;
    os::unsetenv("FORGE_OS_EXT_CHILD"); std::fflush(stdout); std::exit(child_main(m));
}();
static int run_child(const std::string& mode) {
    os::setenv("FORGE_OS_EXT_CHILD", mode); auto r = os::run({os::exe_path()}, true); os::unsetenv("FORGE_OS_EXT_CHILD");
    if (!r) { std::printf("  run child failed: %s\n", r.err.what.c_str()); return -1; }
    if (!r->out.empty()) std::printf("  child[%s]: %s", mode.c_str(), r->out.c_str());
    return r->exit_code;
}

// ---------------------------------------------------------------- 1 NUMA
TEST(numa) {
    auto ns = os::numa_nodes(); CHECK(!ns.empty());
    for (auto& n : ns) std::printf("  node %u: %zu cpus, %llu MiB, %llu MiB free\n", n.id, n.cpus.size(), (unsigned long long)(n.total_bytes >> 20), (unsigned long long)(n.free_bytes >> 20));
    CHECK(!ns[0].cpus.empty());
    auto a = os::numa_alloc(1 << 20, ns[0].id); CHECK(a);
    if (a) {
        std::memset(a.value, 7, 1 << 20); int nd = os::numa_node_of_address(a.value); std::printf("  numa_alloc(node %u) -> page on node %d\n", ns[0].id, nd);
#if defined(_WIN32)
        CHECK(nd >= 0 && nd < int(ns.size() + 64));   // VirtualAllocExNuma's node is a preference: a short node spills to another
#else
        CHECK(nd == -1 || nd == int(ns[0].id));
#endif
        os::numa_free(a.value, 1 << 20);
    }
    std::thread t([&] { CHECK(os::bind_thread_to_node(ns[0].id)); int c = os::current_numa_node(); CHECK(c == -1 || c == int(ns[0].id)); }); t.join();
    CHECK(!os::bind_thread_to_node(9999));
    int cpu = os::current_cpu();   // numbered like cpu_topology (Windows: group * 64 + number)
    if (cpu >= 0) { bool known = false; for (auto& c : os::cpu_topology_query().cpus) known |= int(c.id) == cpu; CHECK_MSG(known, "current_cpu %d not in the topology", cpu); }
#if defined(__linux__) || defined(_WIN32) || defined(__FreeBSD__)
    CHECK(cpu >= 0);
#endif
}
// ---------------------------------------------------------------- 2 huge pages
TEST(huge_pages) {
    auto sz = os::huge_page_sizes(); std::printf("  sizes:"); for (auto s : sz) std::printf(" %s", os::detail::format_size(s).c_str()); std::printf(" thp=%d\n", int(os::thp_enabled()));
    auto r = os::huge_alloc(3 << 20, os::page_kind::huge_2m); CHECK(r); CHECK(r->size >= (3u << 20));
    if (r) { std::printf("  2M request -> %s\n", os::page_kind_name(r->kind)); std::memset(r->ptr, 1, r->size); os::huge_free(r.value); CHECK(!r->ptr); }
    auto g = os::huge_alloc(1 << 20, os::page_kind::huge_1g, false);   // strict: may legitimately fail, never degrades
    std::printf("  strict 1G -> %s\n", g ? os::page_kind_name(g->kind) : g.err.what.c_str());
    if (g) { CHECK(g->kind == os::page_kind::huge_1g); std::memset(g->ptr, 1, 1 << 20); os::huge_free(g.value); }
    auto s2 = os::huge_alloc(3 << 20, os::page_kind::huge_2m, false);
    std::printf("  strict 2M -> %s\n", s2 ? os::page_kind_name(s2->kind) : s2.err.what.c_str());
    if (s2) { CHECK(s2->kind == os::page_kind::huge_2m); CHECK(s2->size >= (3u << 20)); std::memset(s2->ptr, 2, s2->size); os::huge_free(s2.value); }
    auto m = os::mapping::anonymous(4 << 20); CHECK(m);
    auto a = os::advise_huge(m->data(), m->size()); if (!a) std::printf("  advise_huge: %s\n", a.err.what.c_str());
}
// ---------------------------------------------------------------- 3 async file I/O
static void aio_roundtrip(os::async_file_io& io, const std::string& path) {
    const std::size_t blk = 64 << 10, nblk = 16;
    std::vector<std::uint8_t> src(blk * nblk); for (std::size_t i = 0; i < src.size(); ++i) src[i] = std::uint8_t(i * 31 + 7);
    auto f = os::file::open(path, os::rdwr | os::create | os::truncate); CHECK(f); if (!f) return;
    for (std::size_t i = 0; i < nblk; ++i) CHECK(io.write(f.value, src.data() + i * blk, blk, i * blk, 100 + i));
    std::size_t got = 0; while (got < nblk) { auto c = io.wait(1, 5000); if (c.empty()) break; for (auto& x : c) { CHECK(x.result == std::int64_t(blk)); CHECK(x.user >= 100 && x.user < 100 + nblk); } got += c.size(); }
    CHECK(got == nblk); CHECK(f->sync());
    std::vector<std::uint8_t> dst(src.size());
    for (std::size_t i = 0; i < nblk; ++i) CHECK(io.read(f.value, dst.data() + i * blk, blk, i * blk, i));
    got = 0; while (got < nblk) { auto c = io.wait(nblk - got, 5000); if (c.empty()) break; for (auto& x : c) CHECK(x.result == std::int64_t(blk)); got += c.size(); }
    CHECK(got == nblk); CHECK(dst == src); CHECK(io.in_flight() == 0);
    CHECK(io.wait(1, 10).empty());
}
TEST(async_file_io) {
    auto d = scratch("aio");
    { os::async_file_io io; std::printf("  backend %s\n", io.backend_name()); aio_roundtrip(io, os::join(d, "a.bin")); }
    {   // a failing request completes with its own user value and a negative result; nothing stays in flight
        os::async_file_io io; auto f = os::file::open(os::join(d, "a.bin"), os::wr); CHECK(f); char b[16];
        auto st = io.read(f.value, b, sizeof b, 0, 4242);
        if (st) { auto c = io.wait(1, 5000); CHECK(c.size() == 1); if (!c.empty()) { CHECK(c[0].user == 4242); CHECK(c[0].result < 0); } }
        CHECK(io.in_flight() == 0); io.forget(f.value);
    }
    os::setenv("FORGE_AIO", "threads");
    { os::async_file_io io; CHECK(io.backend() == os::aio_backend::threads); aio_roundtrip(io, os::join(d, "b.bin")); }
    os::unsetenv("FORGE_AIO"); os::remove_all(d);
}
// ---------------------------------------------------------------- 4 CPU topology
TEST(cpu_topology) {
    auto t = os::cpu_topology_query();
    std::printf("  %s: %u logical, %u physical, %u pkg, hybrid=%d P=%zu E=%zu\n", t.brand.c_str(), t.logical, t.physical, t.packages, int(t.hybrid),
                t.of_kind(os::core_kind::performance).size(), t.of_kind(os::core_kind::efficiency).size());
    for (auto& c : t.caches) if (c.shared_cpus.empty() || c.shared_cpus[0] == t.cpus[0].id) std::printf("  L%u%c %s line %u\n", c.level, c.type, os::detail::format_size(c.size).c_str(), c.line);
    CHECK(t.logical >= 1); CHECK(t.physical >= 1 && t.physical <= t.logical); CHECK(t.packages >= 1);
    CHECK(t.logical == t.cpus.size());
    for (auto& c : t.cpus) CHECK(c.kind != os::core_kind::unknown);
#if defined(__x86_64__) || defined(__linux__) || defined(_WIN32)
    CHECK(!t.caches.empty()); CHECK(t.cache_size(1, 'D') > 0);
#endif
    std::thread th([] { auto s = os::pin_thread_to_kind(os::core_kind::performance); CHECK(s || os::is_unsupported(s.err)); }); th.join();
    std::thread th2([&] {   // the highest-numbered cpu (group > 0 on Windows hosts with more than 64)
        auto s = os::pin_thread_to({t.cpus.back().id});
        if (!s) skip("pin_thread_to last cpu", s.err.what);   // a container cpuset may exclude it; macOS has no hard affinity
#if defined(_WIN32)
        CHECK(s);
#elif defined(__linux__)
        if (s) CHECK(unsigned(os::current_cpu()) == t.cpus.back().id);
#endif
    }); th2.join();
    CHECK(!os::pin_thread_to({}));
    std::thread th3([] {   // ids past CPU_SETSIZE / any processor group: an error, never a stack overwrite
        CHECK(!os::pin_thread_to({5000})); CHECK(!os::pin_thread_to({1u << 20}));
    }); th3.join();
    CHECK(os::detail::parse_cpulist("0-2,5,7-8") == (std::vector<unsigned>{0, 1, 2, 5, 7, 8}));
}
// ---------------------------------------------------------------- 5 power / thermal
TEST(power_thermal) {
    auto p = os::power_status();
    std::printf("  [%s] battery=%d %d%% on_battery=%d charging=%d low_power=%d thermal=%s throttled=%d temp=%.1fC\n", p.source.c_str(), int(p.has_battery), p.battery_percent,
                int(p.on_battery), int(p.charging), int(p.low_power_mode), os::thermal_name(p.thermal), int(p.throttled), p.temperature_c);
    CHECK(p.battery_percent >= -1 && p.battery_percent <= 100);
    os::power_state h; h.has_battery = true; h.battery_percent = 12; h.low_power_mode = true; h.thermal = os::thermal_state::serious; os::power_hooks::report(h);
    auto q = os::power_status(); CHECK(q.battery_percent == 12 && q.low_power_mode && q.thermal == os::thermal_state::serious && q.source == "hook");
    os::power_hooks::clear(); CHECK(os::power_status().source != "hook");
}
// ---------------------------------------------------------------- 6 memory pressure
TEST(memory_pressure) {
    auto& mp = os::memory_pressure::instance();
    std::atomic<int> seen{0}; std::atomic<int> last{-1};
    int id = mp.subscribe([&](os::pressure_level l) { ++seen; last = int(l); });
    os::sleep_ns(50000000);
    std::printf("  source %s, sampled %s\n", mp.source().c_str(), os::pressure_name(os::memory_pressure::sample()));
    int s0 = seen;
    mp.notify(os::pressure_level::moderate); CHECK(seen == s0 + 1); CHECK(last == int(os::pressure_level::moderate));
    os::memory_pressure::on_trim_memory(80); CHECK(last == int(os::pressure_level::critical));
    os::memory_pressure::on_trim_memory(5); CHECK(last == int(os::pressure_level::moderate));
    os::memory_pressure::on_trim_memory(20); CHECK(last == int(os::pressure_level::normal));
    mp.unsubscribe(id); int s1 = seen; mp.notify(os::pressure_level::critical); CHECK(seen == s1);
    CHECK(mp.monitoring());
    mp.stop(); CHECK(!mp.monitoring());
    // A second subscription after stop() gets a live monitor again (the wake byte of the first stop is gone).
    for (int round = 0; round < 3; ++round) {
        int id2 = mp.subscribe([](os::pressure_level) {}); os::sleep_ns(100000000);
        CHECK_MSG(mp.monitoring(), "monitor dead after restart %d (source %s)", round, mp.source().c_str());
        mp.unsubscribe(id2); mp.stop(); CHECK(!mp.monitoring());
    }
}
// ---------------------------------------------------------------- 7 shared memory
TEST(shared_memory) {
    std::string name = "osx" + std::to_string(os::pid());
    auto a = os::shared_memory::create(name, 1 << 16); CHECK_MSG(a, "%s", a.err.what.c_str()); if (!a) return;
    std::memcpy(a->data(), "hello", 6);
    { auto b = os::shared_memory::open(name, 1 << 16); CHECK(b); if (b) { CHECK(std::string(reinterpret_cast<char*>(b->data())) == "hello"); b->data()[10] = 'Z'; } }
    CHECK(a->data()[10] == 'Z');
#if !defined(__APPLE__) || !TARGET_OS_IPHONE
    int rc = run_child("shm:" + name); CHECK(rc == 0);
    CHECK(std::string(reinterpret_cast<char*>(a->data() + 64)) == "child-was-here");
#endif
    CHECK(!os::shared_memory::create(name, 1 << 16));   // taken: never silently reopened / truncated
    a->close(); CHECK(!os::shared_memory::open("osx-missing-" + std::to_string(os::pid())));
    CHECK(!os::shared_memory::create("osx-zero-" + std::to_string(os::pid()), 0));
    // Long names that share a prefix stay distinct (Apple truncated them to 26 chars before).
    std::string base = "osx-long-name-shared-prefix-" + std::to_string(os::pid()) + "-abcdefghijklmnop-";
    auto x = os::shared_memory::create(base + "one", 4096), y = os::shared_memory::create(base + "two", 4096);
    CHECK_MSG(x && y, "%s %s", x.err.what.c_str(), y.err.what.c_str());
    if (x && y) { x->data()[0] = 1; y->data()[0] = 2; CHECK(x->data()[0] == 1); }
#if defined(__APPLE__)
    CHECK(os::shared_memory::os_name(base + "one").size() <= 31); CHECK(os::shared_memory::os_name(base + "one") != os::shared_memory::os_name(base + "two"));
#endif
}
// ---------------------------------------------------------------- 8 GPU discovery
TEST(gpu_discovery) {
    auto g = os::gpu_devices();
    for (auto& d : g) std::printf("  %s [%04x:%04x] %s driver=%s bus=%s vram=%llu MiB%s\n", d.name.c_str(), d.vendor_id, d.device_id, d.vendor.c_str(), d.driver.c_str(), d.bus.c_str(),
                                  (unsigned long long)(d.dedicated_memory >> 20), d.integrated ? " (integrated)" : "");
    if (g.empty()) std::printf("  no GPUs visible\n");
    for (auto& d : g) CHECK(!d.name.empty());
#if defined(_WIN32)
    for (auto& d : g) {   // DXCore answers integrated/discrete for every hardware adapter on Windows 10 2004+
        unsigned long hi = 0, lo = 0; if (std::sscanf(d.bus.c_str(), "luid:%8lx%8lx", &hi, &lo) != 2) continue;
        LUID l{}; l.HighPart = LONG(hi); l.LowPart = DWORD(lo);
        int v = os::detail::dxcore_is_integrated(l); std::printf("  %s: dxcore integrated=%d\n", d.name.c_str(), v);
        CHECK(v == -1 || v == int(d.integrated));
    }
#endif
    CHECK(!os::intel_discrete_id(0x9a49)); CHECK(os::intel_discrete_id(0x56a0)); CHECK(os::intel_discrete_id(0xe20b));   // Iris Xe (TGL) / Arc A770 / Arc B580
    CHECK(os::gpu_integrated_guess(0x8086, 0x9a49, 0, 2, 0)); CHECK(!os::gpu_integrated_guess(0x8086, 0x56a0, 3, 0, 0)); CHECK(!os::gpu_integrated_guess(0x10de, 0x2204, 1, 0, 0));
    CHECK(std::string(os::gpu_vendor_name(0x10de)) == "NVIDIA");
}
// ---------------------------------------------------------------- 9 local IPC
TEST(local_ipc) {
    auto ep = os::local_endpoint("osx" + std::to_string(os::pid()));   // abstract "@forge-osx<pid>" on Linux/Android
    auto l = os::local_listener::listen(ep); CHECK_MSG(l, "%s", l.err.what.c_str()); if (!l) return;
    std::thread cl([&] {
        auto c = os::local_stream::connect(ep); CHECK(c); if (!c) return;
        CHECK(c->write_all("ping", 4)); char b[5] = {}; CHECK(c->read_exact(b, 4)); CHECK(std::string(b) == "pong");
    });
    auto s = l->accept(5000); CHECK_MSG(s, "%s", s.err.what.c_str());
    if (s) {
        char b[5] = {}; CHECK(s->read_exact(b, 4)); CHECK(std::string(b) == "ping"); CHECK(s->write_all("pong", 4));
        auto pc = s->peer(); CHECK_MSG(pc, "%s", pc.err.what.c_str());
        if (pc) {
            std::printf("  peer pid=%lld uid=%lld sid=%s\n", (long long)pc->pid, (long long)pc->uid, pc->sid.c_str());
#if defined(_WIN32)
            CHECK(pc->pid == std::int64_t(os::pid())); CHECK(pc->sid.rfind("S-1-", 0) == 0);
#else
            CHECK(pc->uid == std::int64_t(::getuid()));
#if defined(__linux__) || defined(__APPLE__)
            CHECK(pc->pid == std::int64_t(os::pid()));
#endif
#endif
        }
    }
    cl.join();
    // Repeated timeouts (Windows: each cancels an overlapped ConnectNamedPipe whose OVERLAPPED is on the stack),
    // then a real client still gets through.
    for (int i = 0; i < 5; ++i) { auto t = l->accept(20); CHECK(!t); if (!t) CHECK(os::classify(t.err) == os::os_errc::timed_out); }
    std::thread cl2([&] { auto c = os::local_stream::connect(ep); CHECK(c); if (c) CHECK(c->write_all("x", 1)); });
    { auto s2 = l->accept(5000); CHECK_MSG(s2, "%s", s2.err.what.c_str()); char b = 0; if (s2) { CHECK(s2->read_exact(&b, 1)); CHECK(b == 'x'); } }
    cl2.join();
#if defined(__linux__)
    auto ab = os::local_listener::listen("@forge-osx-" + std::to_string(os::pid())); CHECK(ab);
#endif
#if defined(FORGE_OS_POSIX)
    {   // listen() on a path holding a regular file fails and leaves the file alone
        auto d = scratch("ipc"); auto f = os::join(d, "not-a-socket"); CHECK(os::write_file(f, "keep me"));
        auto bad = os::local_listener::listen(f); CHECK(!bad);
        CHECK(os::read_file(f).value == "keep me");
#if !defined(__ANDROID__)   // the emulator's shell user may not create sockets under /data/local/tmp (SELinux)
        // a stale socket from a dead listener is replaced
        auto sp = os::join(d, "s.sock");
        { sockaddr_un a{}; socklen_t len = 0; CHECK(!os::local_stream::addr_(sp, a, len)); int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
          CHECK(fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&a), len) == 0); ::close(fd); }   // left behind, as by a crash
        auto l2 = os::local_listener::listen(sp); CHECK_MSG(l2, "%s", l2.err.what.c_str());
        if (l2) { l2->close(); CHECK(!os::exists(sp)); }
#endif
        os::remove_all(d);
    }
#endif
}
// ---------------------------------------------------------------- 10 crash handling
#if defined(_MSC_VER) && !defined(__clang__)
#define FORGE_TEST_NOINLINE __declspec(noinline)
#else
#define FORGE_TEST_NOINLINE __attribute__((noinline))
#endif
static FORGE_TEST_NOINLINE std::vector<os::stack_frame> deep_trace(int n) { if (n) { auto v = deep_trace(n - 1); std::atomic_signal_fence(std::memory_order_seq_cst); return v; } return os::stack_trace(); }
TEST(crash_handling) {
    auto st = deep_trace(3); CHECK(st.size() >= 3);
    std::printf("%s", os::format_stack_trace(std::vector<os::stack_frame>(st.begin(), st.begin() + std::ptrdiff_t(std::min<std::size_t>(st.size(), 5)))).c_str());
#if !defined(__APPLE__) || !TARGET_OS_IPHONE
    auto d = scratch("crash");
    int rc = run_child("crash:" + d); CHECK(rc != 0);
    auto l = os::list_dir(d); bool report = false, dump = false;
    if (l) for (auto& e : l.value) {
        auto p = os::join(d, e.name);
        if (e.name.size() > 6 && e.name.substr(e.name.size() - 6) == ".crash") { auto t = os::read_file(p); report = t && (t->find("SIGSEGV") != std::string::npos || t->find("exception 0xc0000005") != std::string::npos) && t->find("backtrace") != std::string::npos; }
        if (e.name.size() > 4 && e.name.substr(e.name.size() - 4) == ".dmp") { auto t = os::read_file(p); dump = t && t->size() > 32 && t->compare(0, 4, "MDMP") == 0; }
    }
    CHECK(report);
#if defined(_WIN32)
    CHECK(dump);
#else
    (void)dump;
#endif
    os::remove_all(d);
#endif
}
// ---------------------------------------------------------------- 11 sandboxing
TEST(sandboxing) {
    auto s = os::sandbox_status(); std::printf("  current: sandboxed=%d %s\n", int(s.sandboxed), s.mechanism.c_str());
#if !defined(__APPLE__) || !TARGET_OS_IPHONE
    auto d = scratch("sbx");
    for (std::string m : {std::string("sandbox_net"), std::string("sandbox_exec"), "sandbox_write:" + d}) {
        int rc = run_child(m);
        if (rc == 77) { skip(m.c_str(), "unsupported here"); continue; }
#if defined(__FreeBSD__)
        if (rc != 0) { skip(m.c_str(), "capsicum is all-or-nothing"); continue; }
#endif
#if defined(_WIN32)
        if (rc == 4 && GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version")) { skip(m.c_str(), "wine does not enforce job limits"); continue; }
#endif
        CHECK_MSG(rc == 0, "%s rc=%d", m.c_str(), rc);
    }
#if defined(__linux__)
    if (s.landlock_abi == 0) skip("landlock", "kernel has no Landlock (seccomp only)");
    else for (std::string m : {"landlock_write:" + d, std::string("landlock_exec")}) { int rc = run_child(m); CHECK_MSG(rc == 0, "%s rc=%d", m.c_str(), rc); }
#endif
    os::remove_all(d);
#endif
}
// run_sandboxed on every desktop platform: exit codes pass through (FreeBSD: capability mode + fexecve of the
// run-time linker, which used to exit 127), writes are refused under deny_fs_write, arguments arrive intact.
TEST(run_sandboxed_child) {
#if defined(_WIN32)
    os::sandbox_policy x; x.deny_exec = true;
    auto r = os::run_sandboxed({"cmd", "/d", "/c", "exit 3"}, x); CHECK_MSG(r, "%s", r.err.what.c_str()); if (r) CHECK(r.value == 3);
    // exec denied: the nested cmd cannot start (ActiveProcessLimit 1)
    auto n = os::run_sandboxed({"cmd", "/d", "/c", "cmd /d /c exit 0 || exit 9"}, x); CHECK(n); if (n) CHECK(n.value == 9);
    // quoting goes through the spawn_wait command-line builder: quotes, spaces, trailing backslash, empty
    os::setenv("FORGE_OS_EXT_CHILD", "argv");
    auto a = os::run_sandboxed({os::exe_path(), "a \"b\" c\\", "", "x\\\"y"}, x);
    os::unsetenv("FORGE_OS_EXT_CHILD");
    CHECK_MSG(a, "%s", a.err.what.c_str()); if (a) CHECK_MSG(a.value == 0, "argv child rc=%d", a.value);
    os::sandbox_policy net; net.deny_network = true;   // AppContainer: may be unavailable (e.g. some CI images)
    auto c = os::run_sandboxed({"cmd", "/d", "/c", "exit 4"}, net);
    if (!c) skip("AppContainer run_sandboxed", c.err.what); else CHECK(c.value == 4);
    CHECK(!os::run_sandboxed({"no-such-forge-cmd-xyz"}, x));
#elif !defined(__APPLE__) || !TARGET_OS_IPHONE
    os::sandbox_policy none, net, w, x; net.deny_network = true; w.deny_fs_write = true; x.deny_exec = true;
    auto r0 = os::run_sandboxed({"sh", "-c", "exit 5"}, none); CHECK(r0); if (r0) CHECK(r0.value == 5);
    auto r = os::run_sandboxed({"sh", "-c", "exit 3"}, net); CHECK_MSG(r, "%s", r.err.what.c_str()); if (r) CHECK_MSG(r.value == 3, "rc=%d", r.value);
    auto d = scratch("rsb"); std::string f = os::join(d, "w.txt");
    auto rw = os::run_sandboxed({"sh", "-c", "echo x > " + f}, w); CHECK(rw); if (rw) CHECK(rw.value != 0);
    CHECK(!os::read_file(f));
    auto ok = os::run_sandboxed({"sh", "-c", "echo x > " + f}, none); CHECK(ok); if (ok) CHECK(ok.value == 0);
    CHECK(os::read_file(f));
    auto rx = os::run_sandboxed({"sh", "-c", "exit 7"}, x); CHECK(rx); if (rx) CHECK(rx.value == 7);
#if defined(__FreeBSD__)
    auto ex = os::run_sandboxed({"sh", "-c", "/bin/echo hi"}, x); CHECK(ex); if (ex) CHECK(ex.value != 0);   // capability mode: no exec by path
#elif defined(__linux__)
    // Landlock: the target (and its ld.so) may execute, nothing else; without Landlock the target may still exec.
    auto ex = os::run_sandboxed({"sh", "-c", "/bin/echo hi"}, x); CHECK(ex);
    auto tgt = os::run_sandboxed({"/bin/echo", "direct"}, x); CHECK(tgt); if (tgt) CHECK(tgt.value == 0);   // dynamic ELF target runs
    std::string scr = os::join(d, "s.sh"); CHECK(os::write_file(scr, "#!/bin/sh\nexit 4\n")); ::chmod(scr.c_str(), 0755);
    auto sr = os::run_sandboxed({scr}, x); CHECK(sr); if (sr) CHECK_MSG(sr.value == 4, "rc=%d", sr.value);   // #! interpreter allowed
    os::sandbox_policy xw = x; xw.deny_fs_write = true;
    auto exw = os::run_sandboxed({"sh", "-c", "exit 6"}, xw); CHECK(exw); if (exw) CHECK(exw.value == 6);
    if (os::sandbox_status().landlock_abi) { if (ex) CHECK_MSG(ex.value != 0, "rc=%d", ex.value); }
    else { skip("run_sandboxed deny_exec on the target", "no Landlock"); if (ex) CHECK(ex.value == 0); }
#endif
    CHECK(!os::run_sandboxed({"no-such-forge-cmd-xyz"}, net));
    os::remove_all(d);
#endif
}
// The crash handler chains to a handler installed before it (after writing its report), reports abort(), and
// by default writes into a private per-user directory.
TEST(crash_handler_chain_abort_private_dir) {
#if !defined(__APPLE__) || !TARGET_OS_IPHONE
    auto find_report = [](const std::string& dir, const std::string& prefix, const char* needle) {
        auto l = os::list_dir(dir); bool found = false;
        if (l) for (auto& e : l.value)
            if (e.name.rfind(prefix, 0) == 0 && e.name.size() > 6 && e.name.substr(e.name.size() - 6) == ".crash") {
                auto t = os::read_file(os::join(dir, e.name));
                if (t && t->find(needle) != std::string::npos && t->find("backtrace") != std::string::npos) found = true;
            }
        return found;
    };
    auto d = scratch("crashc");
    int rc = run_child("crash_chain:" + d); CHECK_MSG(rc == 42, "rc=%d", rc);
    CHECK(find_report(d, "osx-", "pid "));
    auto d2 = scratch("crasha");
    rc = run_child("abort:" + d2); CHECK(rc != 0);
    CHECK(find_report(d2, "osx-", "abort") || find_report(d2, "osx-", "SIGABRT"));
    os::remove_all(d); os::remove_all(d2);
#if !defined(_WIN32)
    const std::string app = "osxd" + std::to_string(os::pid());
    rc = run_child("crash_default:" + app); CHECK(rc != 0);
    const std::string pd = os::join(os::temp_dir(), "forge-crash-" + std::to_string(::geteuid()));
    struct stat sb{}; CHECK(::lstat(pd.c_str(), &sb) == 0); CHECK(S_ISDIR(sb.st_mode)); CHECK((sb.st_mode & 077) == 0);
    CHECK(find_report(pd, app + "-", "SIGSEGV"));
    if (auto l = os::list_dir(pd)) for (auto& e : l.value) if (e.name.rfind(app, 0) == 0) std::remove(os::join(pd, e.name).c_str());
#endif
#endif
}
// ---------------------------------------------------------------- 12 secure key storage
TEST(key_storage) {
    auto d = scratch("keys"); os::setenv("FORGE_KEYSTORE_DIR", d);
    std::printf("  default backend %s\n", os::keystore::backend().c_str());
    std::string svc = "forge-test-" + std::to_string(os::pid()), secret = std::string("s3cr\0et\xff", 8);
    // The platform backend must work: a failing put is a test failure, not a skip.
    auto p = os::keystore::put(svc, "alice", secret); CHECK_MSG(p, "default keystore put: %s", p.err.what.c_str());
    { auto g = os::keystore::get(svc, "alice"); CHECK(g); CHECK(g.value == secret); CHECK(os::keystore::remove(svc, "alice")); CHECK(!os::keystore::get(svc, "alice")); CHECK(os::keystore::remove(svc, "alice")); }
    os::setenv("FORGE_KEYSTORE", "file"); CHECK(os::keystore::backend() == "file");
    CHECK(os::keystore::put(svc, "bob", secret)); CHECK(os::keystore::put(svc, "bob", "v2-plaintext-marker"));
    auto g = os::keystore::get(svc, "bob"); CHECK(g && g.value == "v2-plaintext-marker");
    CHECK(os::keystore::put(svc, "carol", secret)); CHECK(os::keystore::get(svc, "carol").value == secret);
    auto fnv = [&](const std::string& s, const std::string& a) { std::uint64_t h = 1469598103934665603ull; for (char c : s + '\x1f' + a) { h ^= std::uint8_t(c); h *= 1099511628211ull; }
        char b[24]; std::snprintf(b, sizeof b, "%016llx.key", (unsigned long long)h); return os::join(d, b); };
    auto raw = os::list_dir(d); bool plain = false, tmp_left = false;
    if (raw) for (auto& e : raw.value) { auto t = os::read_file(os::join(d, e.name)); plain |= t && t->find("plaintext-marker") != std::string::npos; tmp_left |= e.name.find(".tmp") != std::string::npos; }
    CHECK(!plain); CHECK(!tmp_left); CHECK(os::exists(os::join(d, ".master")));
#if defined(FORGE_OS_POSIX)
    { struct stat st{}; CHECK(::stat(os::join(d, ".master").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600); CHECK(::stat(fnv(svc, "bob").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600); }
#endif
    {   // tamper: any flipped byte (header, nonce, tag or ciphertext) fails authentication
        auto f = fnv(svc, "bob"); auto orig = os::read_file(f); CHECK(orig && orig->size() > 44);
        for (std::size_t i : {std::size_t(5), std::size_t(30), orig->size() - 1}) { auto t = *orig; t[i] = char(t[i] ^ 1); CHECK(os::write_file(f, t)); auto r = os::keystore::get(svc, "bob"); CHECK(!r); CHECK(r.err.code != 2); }
        CHECK(os::write_file(f, orig->substr(0, 40))); CHECK(!os::keystore::get(svc, "bob"));
        CHECK(os::write_file(f, os::read_file(fnv(svc, "carol")).value)); CHECK(!os::keystore::get(svc, "bob"));   // swapped entries: AD binds (service, account)
        CHECK(os::write_file(f, *orig)); CHECK(os::keystore::get(svc, "bob").value == "v2-plaintext-marker");
    }
    {   // old XOR-pad format: read once, migrated to the encrypted format, pad removed when no old file remains
        std::string pad(64, '\0'); for (int i = 0; i < 64; ++i) pad[std::size_t(i)] = char(i * 37 + 11);
        CHECK(os::write_file(os::join(d, ".pad"), pad));
        std::string old = std::string("legacy\0sec", 10); for (std::size_t i = 0; i < old.size(); ++i) old[i] = char(old[i] ^ pad[i % 64]);
        CHECK(os::write_file(fnv(svc, "old1"), old)); CHECK(os::write_file(fnv(svc, "old2"), old));
        auto r = os::keystore::get(svc, "old1"); CHECK(r && r.value == std::string("legacy\0sec", 10));
        CHECK(os::read_file(fnv(svc, "old1"))->rfind("FKS1", 0) == 0); CHECK(os::exists(os::join(d, ".pad")));   // old2 still needs it
        r = os::keystore::get(svc, "old2"); CHECK(r && r.value == std::string("legacy\0sec", 10)); CHECK(!os::exists(os::join(d, ".pad")));
        CHECK(os::keystore::get(svc, "old1").value == std::string("legacy\0sec", 10));
        CHECK(os::keystore::remove(svc, "old1")); CHECK(os::keystore::remove(svc, "old2"));
    }
    {   // replaced store key: existing secrets fail loudly instead of decoding to garbage
        auto mk = os::read_file(os::join(d, ".master")).value; auto bad = mk; bad.back() = char(bad.back() ^ 1);
        CHECK(os::write_file(os::join(d, ".master"), bad)); CHECK(!os::keystore::get(svc, "bob"));
        CHECK(os::write_file(os::join(d, ".master"), mk)); CHECK(os::keystore::get(svc, "bob"));
    }
    CHECK(os::keystore::remove(svc, "bob")); CHECK(!os::keystore::get(svc, "bob")); CHECK(os::keystore::remove(svc, "carol"));
#if !defined(FORGE_OS_IOS)
    {   // several processes create the store key at once: all must end up with the same key
        auto rd = scratch("keys_race"); std::vector<os::child> kids;
        for (int i = 0; i < 6; ++i) {
            os::spawn_spec sp; sp.argv = {os::exe_path()}; sp.capture_stdout = sp.capture_stderr = false;
            sp.env_set = {{"FORGE_OS_EXT_CHILD", "ks:" + std::to_string(i)}, {"FORGE_KEYSTORE", "file"}, {"FORGE_KEYSTORE_DIR", rd}};
            auto c = os::child::start(sp); CHECK(c); if (c) kids.push_back(std::move(c.value));
        }
        os::sleep_ns(100000000); CHECK(os::write_file(os::join(rd, "go"), "1"));
        for (auto& k : kids) CHECK(k.wait(60000) == 0);
        os::setenv("FORGE_KEYSTORE_DIR", rd);
        for (int i = 0; i < 6; ++i) { auto r = os::keystore::get("race", std::to_string(i)); CHECK_MSG(r && r.value == "v" + std::to_string(i), "race %d: %s", i, r.err.what.c_str()); }
        os::setenv("FORGE_KEYSTORE_DIR", d); os::remove_all(rd);
    }
#endif
    {   // store key wrapped by an Argon2id passphrase (small Argon2 cost to keep the test fast)
        auto wd = scratch("keys_wrap"); os::setenv("FORGE_KEYSTORE_DIR", wd); auto mp = os::join(wd, ".master"); auto old_kdf = os::keystore::kdf();
        CHECK(!os::keystore::set_kdf({1024, 3, 1})); CHECK(!os::keystore::set_kdf({8192, 1, 1})); CHECK(!os::keystore::set_kdf({8192, 2, 0}));
        CHECK(os::keystore::kdf().memory_kib == old_kdf.memory_kib); CHECK(os::keystore::set_kdf({8192, 2, 1}));
        CHECK(os::keystore::put(svc, "w", secret)); CHECK(os::keystore::backend() == "file");   // plain store first
#if !defined(FORGE_OS_WINDOWS)
        CHECK(os::read_file(mp)->rfind("FKK1", 0) == 0);
#endif
        os::keystore::set_passphrase("correct horse"); CHECK(os::keystore::backend() == "file+argon2id");
        { auto r = os::keystore::get(svc, "w"); CHECK(r && r.value == secret); }   // migration: wrapped on first use
        auto wrapped = os::read_file(mp).value;
#if !defined(FORGE_OS_WINDOWS)
        CHECK(wrapped.size() == 104 && wrapped.rfind("FKKP", 0) == 0); CHECK(wrapped.find(secret) == std::string::npos);
#else
        CHECK(wrapped.rfind("FKKD", 0) == 0);
#endif
        os::keystore::clear_passphrase(); os::keystore::set_passphrase("correct horse");   // fresh cache: derives again
        CHECK(os::keystore::put(svc, "w2", "two")); CHECK(os::keystore::get(svc, "w2").value == "two"); CHECK(os::keystore::get(svc, "w").value == secret);
        os::keystore::set_passphrase("wrong horse"); { auto r = os::keystore::get(svc, "w"); CHECK(!r); CHECK_MSG(r.err.code == EACCES, "wrong passphrase: %s", r.err.what.c_str()); }
        os::keystore::clear_passphrase(); { auto r = os::keystore::get(svc, "w"); CHECK(!r); CHECK(r.err.code == EACCES); CHECK(!os::keystore::put(svc, "w3", "x")); }
        os::setenv("FORGE_KEYSTORE_PASSPHRASE", "correct horse"); CHECK(os::keystore::get(svc, "w").value == secret); os::unsetenv("FORGE_KEYSTORE_PASSPHRASE");
        int asked = 0; os::keystore::set_passphrase_provider([&] { ++asked; return std::string("correct horse"); });
        CHECK(os::keystore::get(svc, "w").value == secret); CHECK(os::keystore::get(svc, "w2").value == "two"); CHECK(asked == 1);   // asked once, then cached
        os::keystore::clear_passphrase(); os::keystore::set_passphrase("correct horse");
#if !defined(FORGE_OS_WINDOWS)
        // header tamper: parameters, salt, nonce, tag and ciphertext are all authenticated
        for (std::size_t i : {std::size_t(8), std::size_t(20), std::size_t(40), std::size_t(60), std::size_t(90)}) {
            auto t = wrapped; t[i] = char(t[i] ^ 1); CHECK(os::write_file(mp, t)); auto r = os::keystore::get(svc, "w"); CHECK(!r); CHECK_MSG(r.err.code == EACCES, "tamper %zu: %s", i, r.err.what.c_str());
        }
        {   // Argon2 parameters below the minimum in a stored header are refused, not run
            auto t = wrapped; t[4] = 0; t[5] = 4; t[6] = 0; t[7] = 0; CHECK(os::write_file(mp, t)); auto r = os::keystore::get(svc, "w"); CHECK(!r); CHECK(r.err.what.find("Argon2") != std::string::npos);
        }
        CHECK(os::write_file(mp, wrapped)); CHECK(os::keystore::get(svc, "w").value == secret);
#else
        { auto t = wrapped; t[t.size() / 2] = char(t[t.size() / 2] ^ 1); CHECK(os::write_file(mp, t)); CHECK(!os::keystore::get(svc, "w")); CHECK(os::write_file(mp, wrapped)); }
#endif
        // rewrap: change the passphrase, then remove it
        { auto r = os::keystore::rewrap("bad", "new pass"); CHECK(!r); CHECK(r.err.code == EACCES); }
        CHECK(os::keystore::rewrap("correct horse", "new pass")); CHECK(os::keystore::get(svc, "w").value == secret);
        os::keystore::set_passphrase("correct horse"); CHECK(!os::keystore::get(svc, "w"));
        os::keystore::set_passphrase("new pass"); CHECK(os::keystore::get(svc, "w2").value == "two");
        CHECK(os::keystore::rewrap("new pass", "")); CHECK(os::keystore::backend() == "file");
#if !defined(FORGE_OS_WINDOWS)
        CHECK(os::read_file(mp)->rfind("FKK1", 0) == 0);
#endif
        CHECK(os::keystore::get(svc, "w").value == secret);
        os::keystore::clear_passphrase(); CHECK(os::keystore::set_kdf(old_kdf)); os::setenv("FORGE_KEYSTORE_DIR", d); os::remove_all(wd);
    }
    {   // TPM wrapping (opt-in): skipped where no TPM is usable, which must then fail cleanly as unsupported
        auto ta = os::keystore::tpm_available(); std::printf("  TPM wrapping: %s\n", ta ? "available" : ta.err.what.c_str());
        auto td = scratch("keys_tpm"); os::setenv("FORGE_KEYSTORE_DIR", td); os::setenv("FORGE_KEYSTORE_TPM", "1");
        auto mp = os::join(td, ".master"), tp = os::join(td, ".master.tpm");
        if (!ta) { auto r = os::keystore::put(svc, "t", "x"); CHECK(!r); CHECK(os::is_unsupported(r.err)); CHECK(os::keystore::backend() == "file"); }
        else {
            os::setenv("FORGE_KEYSTORE_TPM_KEY_PREFIX", "forge-test-"); CHECK(os::keystore::set_kdf({8192, 2, 1}));
            CHECK(os::keystore::backend() == "file+tpm");
            auto p = os::keystore::put(svc, "t", "tpm-secret"); CHECK_MSG(p, "TPM put: %s", p.err.what.c_str());
            CHECK(os::read_file(mp).value == "FKKT"); CHECK(os::exists(tp));
            os::keystore::clear_passphrase();   // drop the cached key: next get unseals
            { auto g2 = os::keystore::get(svc, "t"); CHECK_MSG(g2 && g2.value == "tpm-secret", "TPM get: %s", g2.err.what.c_str()); }
            auto tb = os::read_file(tp).value;
            { auto t = tb; t.back() = char(t.back() ^ 1); CHECK(os::write_file(tp, t)); os::keystore::clear_passphrase(); CHECK(!os::keystore::get(svc, "t")); CHECK(os::write_file(tp, tb)); }
            os::keystore::set_passphrase("rescue"); CHECK(os::keystore::backend() == "file+tpm+argon2id");
            CHECK(os::keystore::get(svc, "t").value == "tpm-secret"); CHECK(os::read_file(mp).value != "FKKT"); CHECK(os::read_file(tp).value == tb);   // recovery copy added
            // TPM copy lost (cleared TPM): the passphrase copy still opens the store
            os::unsetenv("FORGE_KEYSTORE_TPM"); CHECK(os::remove(tp)); os::keystore::clear_passphrase(); os::keystore::set_passphrase("rescue");
            CHECK(os::keystore::get(svc, "t").value == "tpm-secret");
            CHECK(os::write_file(tp, tb)); os::setenv("FORGE_KEYSTORE_TPM", "1");
            os::keystore::clear_passphrase(); CHECK(os::keystore::get(svc, "t").value == "tpm-secret");   // TPM first, no passphrase needed
            os::keystore::set_passphrase("rescue"); CHECK(os::keystore::disable_tpm()); CHECK(!os::exists(tp));   // also deletes the NCrypt key
            os::unsetenv("FORGE_KEYSTORE_TPM"); os::keystore::clear_passphrase(); CHECK(!os::keystore::get(svc, "t"));
            os::keystore::set_passphrase("rescue"); CHECK(os::keystore::get(svc, "t").value == "tpm-secret");
            os::keystore::clear_passphrase(); os::unsetenv("FORGE_KEYSTORE_TPM_KEY_PREFIX"); CHECK(os::keystore::set_kdf({}));
        }
        os::unsetenv("FORGE_KEYSTORE_TPM"); os::setenv("FORGE_KEYSTORE_DIR", d); os::remove_all(td);
    }
    os::unsetenv("FORGE_KEYSTORE");
    std::map<std::string, std::string> mem;
    os::keystore::set_backend({"mem", [&](auto& s, auto& a, auto& v) { mem[s + "/" + a] = v; return os::status{}; },
        [&](auto& s, auto& a) { auto it = mem.find(s + "/" + a); return it == mem.end() ? os::result<std::string>{{}, {2, "none"}} : os::result<std::string>{it->second, {}}; },
        [&](auto& s, auto& a) { mem.erase(s + "/" + a); return os::status{}; }});
    CHECK(os::keystore::backend() == "mem"); CHECK(os::keystore::put("x", "y", "z")); CHECK(mem["x/y"] == "z"); CHECK(os::keystore::get("x", "y").value == "z");
    os::keystore::reset_backend(); os::unsetenv("FORGE_KEYSTORE_DIR"); os::remove_all(d);
}
// ---------------------------------------------------------------- 13 file watching
static bool saw(const std::vector<os::watch_event>& v, const std::string& name) { for (auto& e : v) if (os::basename(e.path) == name) return true; return false; }
static std::vector<os::watch_event> collect(os::file_watcher& w, const std::string& name, int ms) {
    std::vector<os::watch_event> all; auto end = os::monotonic_ns() + std::uint64_t(ms) * 1000000ull;
    while (os::monotonic_ns() < end) { auto e = w.poll(100); all.insert(all.end(), e.begin(), e.end()); if (saw(all, name)) break; }
    return all;
}
TEST(file_watching) {
    auto d = scratch("watch"); auto f = os::join(d, "model.fpk"); CHECK(os::write_file(f, "v1"));
    os::file_watcher w; std::printf("  backend %s\n", w.backend());
    CHECK(w.add(f));
    CHECK(os::write_file(f + ".tmp", "v2")); CHECK(os::rename(f + ".tmp", f));   // atomic replace
    auto ev = collect(w, "model.fpk", 3000); CHECK(saw(ev, "model.fpk"));
    for (auto& e : ev) std::printf("  %s %s\n", os::watch_kind_name(e.kind), os::basename(e.path).c_str());
    { auto fh = os::file::open(f, os::wr | os::append); CHECK(fh); fh->write_all("!", 1); }
    CHECK(saw(collect(w, "model.fpk", 3000), "model.fpk"));
    os::file_watcher wd; CHECK(wd.add(d)); CHECK(os::write_file(os::join(d, "new.txt"), "x"));
    auto ev2 = collect(wd, "new.txt", 3000);
    CHECK(saw(ev2, "new.txt"));   // kqueue: by rescanning the directory
    bool created = false; for (auto& e : ev2) if (os::basename(e.path) == "new.txt" && e.kind == os::watch_kind::created) created = true;
    CHECK(created);
    CHECK(os::remove(os::join(d, "new.txt")));
    bool removed = false;   // events left over from the write may come first
    for (auto end = os::monotonic_ns() + 3000000000ull; !removed && os::monotonic_ns() < end;)
        for (auto& e : wd.poll(100)) if (os::basename(e.path) == "new.txt" && e.kind == os::watch_kind::removed) removed = true;
    CHECK(removed);
    CHECK(!w.add(os::join(d, "no/such/dir/x")));
    os::remove_all(d);
}
// ---------------------------------------------------------------- 14 timers + perf counters
TEST(timers_and_perf) {
    // spin for 1 ms of wall time (a fixed loop count can finish inside one tick of a 24 MHz counter on a fast core)
    auto c0 = os::cycle_counter(); volatile double x = 1;
    for (auto s0 = os::monotonic_ns(); os::monotonic_ns() - s0 < 1000000;) x = x * 1.0000001 + 1e-9;
    auto c1 = os::cycle_counter();
    CHECK(c1 > c0); auto hz = os::counter_hz(); CHECK(hz > 1000000);
    std::printf("  counter %.1f MHz, timer res %llu ns\n", double(hz) / 1e6, (unsigned long long)os::timer_resolution_ns());
    auto t0 = os::monotonic_ns(); os::precise_sleep_ns(2000000); auto dt = os::monotonic_ns() - t0;
    std::printf("  precise_sleep 2ms -> %.3f ms\n", double(dt) / 1e6); CHECK(dt >= 2000000 && dt < 50000000);
    os::perf_counters pc; auto o = pc.open({os::perf_event::cycles, os::perf_event::instructions});
    if (!o) { skip("hw counters", o.err.what); o = pc.open({os::perf_event::cycles}); }
    if (o) {
        pc.start(); for (int i = 0; i < 1000000; ++i) x = x * 1.0000001 + 1e-9; auto r = pc.stop();
        for (auto& v : r) std::printf("  [%s] %s = %llu%s%s\n", pc.backend(), os::perf_event_name(v.event), (unsigned long long)v.value, v.valid ? "" : " (invalid)", v.scaled ? " (scaled)" : "");
        CHECK(!r.empty() && r[0].valid && r[0].value > 0);
    } else skip("cycle counter", o.err.what);
}
// ---------------------------------------------------------------- 15 background execution
TEST(background_execution) {
    auto t = os::background::begin("forge-test");
    if (!t) { skip("background", t.err.what); CHECK(os::is_unsupported(t.err)); }
    else {
        auto k = os::background::kind(t.value); std::printf("  kind %s\n", k.c_str());
#if defined(_WIN32)
        CHECK(k == "power-request");
#elif defined(FORGE_OS_LINUX)
        CHECK(k == "logind-inhibit" || k == "not_required");
#endif
        CHECK(os::background::active() == 1);
        auto t2 = os::background::begin("forge-test-2"); CHECK(t2); CHECK(os::background::active() == 2);
        std::thread other([&] { CHECK(os::background::end(t.value)); });   // tokens are per process, not per thread
        other.join(); CHECK(os::background::active() == 1); CHECK(!os::background::end(t.value));
        if (t2) CHECK(os::background::end(t2.value)); CHECK(os::background::active() == 0);
    }
    bool began = false, ended = false;
    os::background::set_hooks({[&](auto&, auto&, auto&) { began = true; return true; }, [&](auto&) { ended = true; }});
#if defined(__ANDROID__)
    { os::background_scope s("hooked"); CHECK(began && s.token); } CHECK(ended);
#else
    (void)began; (void)ended;
#endif
}
// ---------------------------------------------------------------- 16 service integration
TEST(service_integration) {
    std::printf("  manager '%s'\n", os::service::manager().c_str());
    CHECK(os::service::ready());   // no-op outside a service manager
#if defined(__linux__)
    auto path = "@osx-notify-" + std::to_string(os::pid());   // abstract socket, as systemd may use
    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0); sockaddr_un a{}; a.sun_family = AF_UNIX; std::memcpy(a.sun_path + 1, path.data() + 1, path.size() - 1);
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&a), socklen_t(offsetof(sockaddr_un, sun_path) + path.size())) == 0);
    os::setenv("NOTIFY_SOCKET", path); os::setenv("WATCHDOG_USEC", "2000000");
    CHECK(os::service::ready("loaded")); char b[256] = {}; auto n = ::recv(fd, b, sizeof b - 1, 0);
    CHECK(n > 0 && std::string(b) == "READY=1\nSTATUS=loaded"); CHECK(os::service::watchdog_interval_us() == 1000000);
    CHECK(os::service::watchdog()); std::memset(b, 0, sizeof b); ::recv(fd, b, sizeof b - 1, 0); CHECK(std::string(b) == "WATCHDOG=1");
    CHECK(os::service::manager() == "systemd" || os::platform_name() == std::string("android"));
    os::unsetenv("NOTIFY_SOCKET"); os::unsetenv("WATCHDOG_USEC"); ::close(fd);
#endif
    auto l = os::service::log(os::log_level::info, "forge test_os_ext service log line");
    if (!l) skip("service log", l.err.what);
#if !defined(_WIN32)
    {   // concurrent idents: openlog must never see a freed ident string
        std::vector<std::thread> ts; std::atomic<int> bad{0};
        for (int t = 0; t < 4; ++t) ts.emplace_back([t, &bad] { for (int i = 0; i < 25; ++i) if (!os::service::log(os::log_level::debug, "forge concurrent log", "forge-test-" + std::to_string(t) + "-" + std::to_string(i % 3))) ++bad; });
        for (auto& t : ts) t.join(); CHECK(bad == 0 || !l);
    }
#endif
    int ran = 0; auto r = os::service::run_as_service("forge-test", [&] { ++ran; return 0; }, [] {});
    CHECK(r); CHECK(ran == 1);
}
