#pragma once
// Detailed CPU topology: packages, physical cores vs logical CPUs, caches, and performance/efficiency core
// classification (Intel hybrid via sysfs cpu_core/cpu_atom or Windows EfficiencyClass; ARM big.LITTLE via
// cpu_capacity / max frequency; Apple via hw.perflevelN).  Plus pinning helpers for P- or E-cores.
#include <forge/os/detail.hpp>
#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
#include <cpuid.h>
#define FORGE_OS_HAVE_CPUID 1
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define FORGE_OS_HAVE_CPUID 1
#endif
#if defined(FORGE_OS_FREEBSD)
#include <sys/param.h>
#include <sys/cpuset.h>
#endif

namespace forge::os {
enum class core_kind { unknown, performance, efficiency };
inline const char* core_kind_name(core_kind k) { return k == core_kind::performance ? "P" : k == core_kind::efficiency ? "E" : "?"; }
struct cpu_cache { unsigned level = 0; char type = 'U'; /* D, I, U */ std::uint64_t size = 0; unsigned line = 0; std::vector<unsigned> shared_cpus; };
struct logical_cpu { unsigned id = 0, core = 0, package = 0; core_kind kind = core_kind::unknown; std::uint64_t max_khz = 0; unsigned capacity = 0; };
struct cpu_topology {
    unsigned logical = 0, physical = 0, packages = 1; bool hybrid = false;
    std::vector<logical_cpu> cpus; std::vector<cpu_cache> caches;   // caches: one entry per distinct cache instance
    std::string brand;
    std::vector<unsigned> of_kind(core_kind k) const { std::vector<unsigned> v; for (auto& c : cpus) if (c.kind == k) v.push_back(c.id); return v; }
    std::uint64_t cache_size(unsigned level, char type = 'U') const {   // per-instance size of the first matching cache
        for (auto& c : caches) if (c.level == level && (c.type == type || (type == 'D' && c.type == 'U') || (type == 'U' && c.type == 'D'))) return c.size;
        return 0;
    }
};
namespace detail {
#if defined(FORGE_OS_HAVE_CPUID)
// cpuid(leaf, subleaf) for GCC/Clang (<cpuid.h>) and MSVC (<intrin.h>).
inline void cpuid(unsigned leaf, unsigned sub, unsigned& a, unsigned& b, unsigned& c, unsigned& d) {
#if defined(_MSC_VER)
    int r[4] = {}; __cpuidex(r, int(leaf), int(sub)); a = unsigned(r[0]); b = unsigned(r[1]); c = unsigned(r[2]); d = unsigned(r[3]);
#else
    __cpuid_count(leaf, sub, a, b, c, d);
#endif
}
inline unsigned cpuid_max(unsigned base) { unsigned a, b, c, d; cpuid(base, 0, a, b, c, d); return a; }
#endif
inline std::string cpu_brand() {
#if defined(FORGE_OS_HAVE_CPUID)
    unsigned r[12] = {}; if (cpuid_max(0x80000000) < 0x80000004) return {};
    for (unsigned i = 0; i < 3; ++i) cpuid(0x80000002 + i, 0, r[i * 4], r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3]);
    return trim(std::string(reinterpret_cast<char*>(r), 48).c_str());
#elif defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
    auto s = sysctl_str("machdep.cpu.brand_string"); return s.empty() ? sysctl_str("hw.model") : s;
#elif defined(__linux__)
    auto s = slurp("/proc/cpuinfo");
    for (const char* k : {"model name", "Hardware", "Processor"}) { auto p = s.find(k); if (p != std::string::npos) { auto c = s.find(':', p), e = s.find('\n', p); if (c < e) return trim(s.substr(c + 1, e - c - 1)); } }
    return {};
#else
    return {};
#endif
}
#if defined(FORGE_OS_HAVE_CPUID)
inline void cpuid_caches(std::vector<cpu_cache>& out) {   // deterministic cache parameters (Intel leaf 4 / AMD 0x8000001D)
    unsigned a, b, c, d; cpuid(0, 0, a, b, c, d); bool amd = b == 0x68747541; unsigned max_std = a;
    unsigned max_ext = cpuid_max(0x80000000);
    unsigned leaf = amd ? 0x8000001D : 4;
    std::size_t first = out.size();
    // Only walk the deterministic leaf when the CPU reports it (QEMU's qemu64 is "AMD" without 0x8000001D/topoext).
    bool have = amd ? max_ext >= 0x8000001D : max_std >= 4;
    for (unsigned i = 0; have && i < 16; ++i) {
        cpuid(leaf, i, a, b, c, d); unsigned t = a & 31; if (!t) break;
        cpu_cache k; k.level = (a >> 5) & 7; k.type = t == 1 ? 'D' : t == 2 ? 'I' : 'U';
        k.line = (b & 0xfff) + 1; k.size = std::uint64_t(((b >> 22) & 0x3ff) + 1) * (((b >> 12) & 0x3ff) + 1) * k.line * (c + 1);
        out.push_back(k);
    }
    if (out.size() > first || max_ext < 0x80000006) return;
    // Legacy AMD-style leaves (also filled by QEMU/TCG CPU models): L1D/L1I from 0x80000005, L2/L3 from 0x80000006.
    auto add = [&](unsigned lvl, char ty, std::uint64_t bytes, unsigned line) { if (bytes) { cpu_cache k; k.level = lvl; k.type = ty; k.size = bytes; k.line = line ? line : 64; out.push_back(k); } };
    cpuid(0x80000005, 0, a, b, c, d);
    add(1, 'D', std::uint64_t(c >> 24) << 10, c & 0xff); add(1, 'I', std::uint64_t(d >> 24) << 10, d & 0xff);
    cpuid(0x80000006, 0, a, b, c, d);
    add(2, 'U', std::uint64_t(c >> 16) << 10, c & 0xff); add(3, 'U', std::uint64_t(d >> 18) * 512 * 1024, d & 0xff);
}
#endif
inline void classify_by_capacity(cpu_topology& t) {   // heterogeneous capacity/frequency => big.LITTLE
    std::uint64_t hi = 0, lo = ~0ull; bool cap = false;
    for (auto& c : t.cpus) { std::uint64_t v = c.capacity ? c.capacity : c.max_khz; if (c.capacity) cap = true; if (!v) { hi = 0; break; } hi = std::max(hi, v); lo = std::min(lo, v); }
    if (t.cpus.empty() || !hi || hi == lo || (!cap && hi < lo * 11 / 10)) {
        for (auto& c : t.cpus) c.kind = core_kind::performance; return;
    }
    t.hybrid = true;
    for (auto& c : t.cpus) { std::uint64_t v = c.capacity ? c.capacity : c.max_khz; c.kind = v == hi ? core_kind::performance : core_kind::efficiency; }
}
}

inline cpu_topology cpu_topology_query() {
    cpu_topology t; t.brand = detail::cpu_brand();
#if defined(__linux__)
    std::vector<unsigned> online = detail::parse_cpulist(detail::slurp("/sys/devices/system/cpu/online"));
    if (online.empty()) for (unsigned i = 0; i < cpu_count(); ++i) online.push_back(i);
    std::map<std::pair<unsigned, unsigned>, unsigned> cores; std::map<unsigned, int> pk;
    std::map<std::string, cpu_cache> cmap;
    for (unsigned id : online) {
        std::string d = "/sys/devices/system/cpu/cpu" + std::to_string(id);
        logical_cpu c; c.id = id;
        long long core = detail::slurp_ll(d + "/topology/core_id", id), pkg = detail::slurp_ll(d + "/topology/physical_package_id", 0);
        c.package = unsigned(pkg < 0 ? 0 : pkg); c.core = unsigned(core < 0 ? id : core);
        auto key = std::make_pair(c.package, c.core); if (!cores.count(key)) { unsigned n = unsigned(cores.size()); cores[key] = n; }
        c.core = cores[key]; pk[c.package] = 1;
        long long khz = detail::slurp_ll(d + "/cpufreq/cpuinfo_max_freq", 0); c.max_khz = khz > 0 ? std::uint64_t(khz) : 0;
        long long cap = detail::slurp_ll(d + "/cpu_capacity", 0); c.capacity = cap > 0 ? unsigned(cap) : 0;
        t.cpus.push_back(c);
        for (int i = 0; i < 16; ++i) {
            std::string cd = d + "/cache/index" + std::to_string(i);
            auto lvl = detail::slurp_ll(cd + "/level", -1); if (lvl < 0) break;
            auto shared = detail::trim(detail::slurp(cd + "/shared_cpu_list")); auto type = detail::trim(detail::slurp(cd + "/type"));
            std::string k = std::to_string(lvl) + type + "@" + (shared.empty() ? std::to_string(id) : shared);
            if (cmap.count(k)) continue;
            cpu_cache cc; cc.level = unsigned(lvl); cc.type = type == "Data" ? 'D' : type == "Instruction" ? 'I' : 'U';
            auto sz = detail::trim(detail::slurp(cd + "/size")); std::uint64_t v = std::strtoull(sz.c_str(), nullptr, 10);
            if (!sz.empty() && (sz.back() == 'K')) v <<= 10; else if (!sz.empty() && sz.back() == 'M') v <<= 20;
            cc.size = v; cc.line = unsigned(std::max(0ll, detail::slurp_ll(cd + "/coherency_line_size", 0))); cc.shared_cpus = detail::parse_cpulist(shared);
            cmap[k] = cc;
        }
    }
    for (auto& kv : cmap) t.caches.push_back(kv.second);
    t.physical = unsigned(cores.size()); t.packages = unsigned(pk.size() ? pk.size() : 1);
    auto pcores = detail::parse_cpulist(detail::slurp("/sys/devices/cpu_core/cpus")), ecores = detail::parse_cpulist(detail::slurp("/sys/devices/cpu_atom/cpus"));
    if (!pcores.empty() && !ecores.empty()) {   // Intel hybrid (Alder Lake+)
        t.hybrid = true;
        for (auto& c : t.cpus) { for (auto p : pcores) if (p == c.id) c.kind = core_kind::performance; for (auto e : ecores) if (e == c.id) c.kind = core_kind::efficiency; }
    } else detail::classify_by_capacity(t);
#elif defined(FORGE_OS_WINDOWS)
    DWORD len = 0; GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    std::vector<std::uint8_t> buf(len);
    if (len && GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
        BYTE maxeff = 0, mineff = 255; unsigned core = 0, pkg = 0;
        std::vector<std::pair<unsigned, BYTE>> eff;
        // PROCESSOR_RELATIONSHIP.EfficiencyClass is the byte after Flags; older MinGW headers still call it Reserved[0].
        auto effc = [](const PROCESSOR_RELATIONSHIP& p) { return reinterpret_cast<const BYTE*>(&p)[1]; };
        for (DWORD o = 0; o < len;) {
            auto* e = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + o);
            if (e->Relationship == RelationProcessorCore) {
                for (WORD g = 0; g < e->Processor.GroupCount; ++g)
                    for (unsigned b = 0; b < 64; ++b) if (e->Processor.GroupMask[g].Mask & (KAFFINITY(1) << b)) {
                        logical_cpu c; c.id = unsigned(e->Processor.GroupMask[g].Group) * 64 + b; c.core = core;
                        t.cpus.push_back(c); eff.push_back({c.id, effc(e->Processor)});
                    }
                maxeff = std::max(maxeff, effc(e->Processor)); mineff = std::min(mineff, effc(e->Processor)); ++core;
            } else if (e->Relationship == RelationProcessorPackage) {
                for (WORD g = 0; g < e->Processor.GroupCount; ++g)
                    for (auto& c : t.cpus) if (c.id / 64 == e->Processor.GroupMask[g].Group && (e->Processor.GroupMask[g].Mask & (KAFFINITY(1) << (c.id % 64)))) c.package = pkg;
                ++pkg;
            } else if (e->Relationship == RelationCache) {
                cpu_cache k; k.level = e->Cache.Level; k.size = e->Cache.CacheSize; k.line = e->Cache.LineSize;
                k.type = e->Cache.Type == CacheData ? 'D' : e->Cache.Type == CacheInstruction ? 'I' : 'U';
                for (unsigned b = 0; b < 64; ++b) if (e->Cache.GroupMask.Mask & (KAFFINITY(1) << b)) k.shared_cpus.push_back(unsigned(e->Cache.GroupMask.Group) * 64 + b);
                t.caches.push_back(k);
            }
            o += e->Size;
        }
        t.physical = core; t.packages = pkg ? pkg : 1; t.hybrid = maxeff != mineff;
        for (std::size_t i = 0; i < t.cpus.size(); ++i) t.cpus[i].kind = !t.hybrid || eff[i].second == maxeff ? core_kind::performance : core_kind::efficiency;
    }
#elif defined(__APPLE__)
    long long ncpu = detail::sysctl_ll("hw.logicalcpu", (long long)cpu_count()), nphys = detail::sysctl_ll("hw.physicalcpu", ncpu);
    long long levels = detail::sysctl_ll("hw.nperflevels", 1);
    t.packages = unsigned(std::max(1ll, detail::sysctl_ll("hw.packages", 1)));
    t.physical = unsigned(nphys); t.hybrid = levels > 1;
    // XNU numbers efficiency cores first on Apple Silicon (perflevel1 = E, perflevel0 = P).
    long long ecount = levels > 1 ? detail::sysctl_ll("hw.perflevel1.logicalcpu", 0) : 0;
    unsigned smt = unsigned(std::max(1ll, ncpu / std::max(1ll, nphys)));
    for (long long i = 0; i < ncpu; ++i) {
        logical_cpu c; c.id = unsigned(i); c.core = unsigned(i) / smt; c.kind = i < ecount ? core_kind::efficiency : core_kind::performance; t.cpus.push_back(c);
    }
    unsigned line = unsigned(std::max(0ll, detail::sysctl_ll("hw.cachelinesize", 64)));
    auto add = [&](unsigned lvl, char ty, const char* k) { long long v = detail::sysctl_ll(k, 0); if (v > 0) { cpu_cache c; c.level = lvl; c.type = ty; c.size = std::uint64_t(v); c.line = line; t.caches.push_back(c); } };
    if (levels > 1) { add(1, 'D', "hw.perflevel0.l1dcachesize"); add(1, 'I', "hw.perflevel0.l1icachesize"); add(2, 'U', "hw.perflevel0.l2cachesize"); add(1, 'D', "hw.perflevel1.l1dcachesize"); add(2, 'U', "hw.perflevel1.l2cachesize"); }
    else { add(1, 'D', "hw.l1dcachesize"); add(1, 'I', "hw.l1icachesize"); add(2, 'U', "hw.l2cachesize"); add(3, 'U', "hw.l3cachesize"); }
#else   // FreeBSD and others
    unsigned n = cpu_count(); long long cores = -1, tpc = 1;
#if defined(FORGE_OS_FREEBSD)
    cores = detail::sysctl_ll("kern.smp.cores", -1); tpc = std::max(1ll, detail::sysctl_ll("kern.smp.threads_per_core", 1));
#endif
    t.physical = cores > 0 ? unsigned(cores) : n;
    for (unsigned i = 0; i < n; ++i) { logical_cpu c; c.id = i; c.core = unsigned(i / tpc); c.kind = core_kind::performance; t.cpus.push_back(c); }
#if defined(FORGE_OS_HAVE_CPUID)
    detail::cpuid_caches(t.caches);
#endif
#endif
    if (t.cpus.empty()) for (unsigned i = 0; i < cpu_count(); ++i) { logical_cpu c; c.id = c.core = i; c.kind = core_kind::performance; t.cpus.push_back(c); }
#if defined(FORGE_OS_HAVE_CPUID) && !defined(FORGE_OS_FREEBSD)
    if (t.caches.empty()) detail::cpuid_caches(t.caches);
#endif
    t.logical = unsigned(t.cpus.size()); if (!t.physical) t.physical = t.logical;
    return t;
}

// Restrict the calling thread to a set of logical CPUs. Ids past what the kernel's mask can hold are EINVAL.
inline status pin_thread_to(const std::vector<unsigned>& cpus) {
    if (cpus.empty()) return {{}, {22, "pin_thread_to: empty set"}};
#if defined(__linux__)
    unsigned hi = 0; for (auto c : cpus) hi = std::max(hi, c);
    if (hi >= (1u << 16)) return {{}, {22, "pin_thread_to: cpu " + std::to_string(hi) + " out of range"}};
    std::size_t n = std::max<std::size_t>(CPU_SETSIZE, hi + 1), sz = CPU_ALLOC_SIZE(n);   // a dynamic set beyond CPU_SETSIZE
    cpu_set_t* s = CPU_ALLOC(n); if (!s) return {{}, {12, "pin_thread_to: CPU_ALLOC"}};
    CPU_ZERO_S(sz, s); for (auto c : cpus) CPU_SET_S(c, sz, s);
    int rc = ::sched_setaffinity(0, sz, s); auto e = rc ? last_error("sched_setaffinity") : error{}; CPU_FREE(s);
    if (rc) return {{}, e};
    return {};
#elif defined(FORGE_OS_WINDOWS)
    // cpu id = group * 64 + bit (as cpu_topology_query numbers them); a thread's affinity lives in one processor group.
    GROUP_AFFINITY ga{}; ga.Group = WORD(cpus[0] / 64);
    for (auto c : cpus) { if (c / 64 != ga.Group) return {{}, {22, "pin_thread_to: cpus span processor groups"}}; ga.Mask |= KAFFINITY(1) << (c % 64); }
    if (!SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr)) return {{}, last_error("SetThreadGroupAffinity")};
    return {};
#elif defined(FORGE_OS_FREEBSD)
    cpuset_t s; CPU_ZERO(&s);
    for (auto c : cpus) { if (c >= CPU_SETSIZE) return {{}, {22, "pin_thread_to: cpu " + std::to_string(c) + " out of range"}}; CPU_SET(c, &s); }
    if (::cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof s, &s)) return {{}, last_error("cpuset_setaffinity")};
    return {};
#else
    (void)cpus; return {{}, unsupported("hard CPU affinity (use pin_thread_to_kind: QoS)")};
#endif
}
// Prefer P-cores (or E-cores) for the calling thread. Apple has no affinity: QoS class steers scheduling instead.
inline status pin_thread_to_kind(core_kind k) {
#if defined(__APPLE__)
    return pthread_set_qos_class_self_np(k == core_kind::efficiency ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0) == 0 ? status{} : status{{}, last_error("qos")};
#else
    auto t = cpu_topology_query(); auto v = t.of_kind(k);
    if (v.empty()) { if (!t.hybrid) return {}; return {{}, {22, "no cores of that kind"}}; }
    if (!t.hybrid) return {};   // homogeneous: nothing to steer
    return pin_thread_to(v);
#endif
}
} // namespace forge::os
