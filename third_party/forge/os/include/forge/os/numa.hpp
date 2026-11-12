#pragma once
// NUMA topology, node-local allocation and thread binding.
//   Linux/Android: /sys/devices/system/node + mbind/sched_setaffinity.  Windows: GetNumaNodeProcessorMaskEx +
//   VirtualAllocExNuma + SetThreadGroupAffinity.  FreeBSD: vm.ndomains + cpuset_setdomain (first-touch under a
//   domain policy).  macOS/iOS are UMA: one node, allocation is plain, binding to node 0 trivially succeeds.
//   Windows: nodes spanning processor groups via GetNumaNodeProcessorMask2 (Windows 11 / Server 2022), the node
//   of a page via QueryWorkingSetEx. FreeBSD: free bytes per domain from vm.domain.N.stats.free_count.
//   total_bytes is filled where the OS reports it (Linux; one-node systems); 0 otherwise.
#include <forge/os/detail.hpp>
#include <forge/os/cpu_topology.hpp>
#if defined(FORGE_OS_FREEBSD)
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/domainset.h>
#include <sched.h>
#elif defined(FORGE_OS_WINDOWS)
#include <psapi.h>   // K32QueryWorkingSetEx lives in kernel32 (no psapi.lib)
#endif

namespace forge::os {
struct numa_node { unsigned id = 0; std::vector<unsigned> cpus; std::uint64_t total_bytes = 0, free_bytes = 0; };
#if defined(FORGE_OS_WINDOWS)
namespace detail {
// Every processor group of a node: GetNumaNodeProcessorMask2 where present, else the node's primary group.
inline std::vector<GROUP_AFFINITY> numa_node_groups(USHORT node) {
    using fn = BOOL(WINAPI*)(USHORT, PGROUP_AFFINITY, USHORT, PUSHORT);
    static const auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNumaNodeProcessorMask2")));
    std::vector<GROUP_AFFINITY> v;
    if (f) {
        USHORT need = 0; f(node, nullptr, 0, &need);
        if (need) { v.resize(need); if (f(node, v.data(), need, &need)) { v.resize(need); return v; } }
        v.clear();
    }
    GROUP_AFFINITY ga{}; if (GetNumaNodeProcessorMaskEx(node, &ga)) v.push_back(ga);
    return v;
}
}
#endif

inline std::vector<numa_node> numa_nodes() {
    std::vector<numa_node> out;
#if defined(__linux__)
    if (auto l = list_dir("/sys/devices/system/node")) {
        for (auto& e : l.value) {
            if (e.name.rfind("node", 0) || e.name.size() < 5 || e.name[4] < '0' || e.name[4] > '9') continue;
            numa_node n; n.id = unsigned(std::atoi(e.name.c_str() + 4));
            std::string d = "/sys/devices/system/node/" + e.name;
            n.cpus = detail::parse_cpulist(detail::slurp(d + "/cpulist"));
            auto mi = detail::slurp(d + "/meminfo");
            auto kb = [&](const char* key) -> std::uint64_t { auto p = mi.find(key); return p == std::string::npos ? 0 : std::strtoull(mi.c_str() + p + std::strlen(key), nullptr, 10) * 1024; };
            n.total_bytes = kb("MemTotal:"); n.free_bytes = kb("MemFree:");
            out.push_back(std::move(n));
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.id < b.id; });
#elif defined(FORGE_OS_WINDOWS)
    ULONG hi = 0;
    if (GetNumaHighestNodeNumber(&hi)) {
        for (USHORT i = 0; i <= USHORT(hi); ++i) {
            auto gs = detail::numa_node_groups(i); if (gs.empty()) continue;
            numa_node n; n.id = i;
            for (auto& ga : gs) for (unsigned b = 0; b < 64; ++b) if (ga.Mask & (KAFFINITY(1) << b)) n.cpus.push_back(unsigned(ga.Group) * 64 + b);
            ULONGLONG avail = 0; if (GetNumaAvailableMemoryNodeEx(i, &avail)) n.free_bytes = avail;
            if (n.cpus.empty() && !n.free_bytes) continue;
            out.push_back(std::move(n));
        }
    }
#elif defined(FORGE_OS_FREEBSD)
    long long nd = detail::sysctl_ll("vm.ndomains", 1);
    for (long long i = 0; i < nd; ++i) {
        numa_node n; n.id = unsigned(i);
        cpuset_t s; CPU_ZERO(&s);
        if (nd > 1 && ::cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_DOMAIN, id_t(i), sizeof s, &s) == 0) {
            for (unsigned c = 0; c < CPU_SETSIZE; ++c) if (CPU_ISSET(c, &s)) n.cpus.push_back(c);
        }
        long long fc = detail::sysctl_ll(("vm.domain." + std::to_string(i) + ".stats.free_count").c_str(), -1);
        if (fc >= 0) n.free_bytes = std::uint64_t(fc) * page_size();
        out.push_back(std::move(n));
    }
#endif
    if (out.empty()) {   // UMA (macOS/iOS, kernels without NUMA info): one node with every CPU and all RAM
        numa_node n; for (unsigned c = 0; c < cpu_count(); ++c) n.cpus.push_back(c);
        n.total_bytes = total_memory(); n.free_bytes = available_memory(); out.push_back(std::move(n));
    }
    if (out.size() == 1) {
        if (out[0].cpus.empty()) for (unsigned c = 0; c < cpu_count(); ++c) out[0].cpus.push_back(c);
        if (!out[0].total_bytes) out[0].total_bytes = total_memory();
    }
    return out;
}
inline unsigned numa_node_count() { return unsigned(numa_nodes().size()); }
inline int numa_node_of_cpu(unsigned cpu) {
    for (auto& n : numa_nodes()) for (auto c : n.cpus) if (c == cpu) return int(n.id);
    return -1;
}
// The logical CPU the calling thread runs on, numbered like cpu_topology (Windows: group * 64 + number).
inline int current_cpu() {
#if defined(FORGE_OS_WINDOWS)
    PROCESSOR_NUMBER pn{}; GetCurrentProcessorNumberEx(&pn); return int(pn.Group) * 64 + int(pn.Number);
#elif defined(__linux__) || (defined(FORGE_OS_FREEBSD) && __FreeBSD_version >= 1300000)
    return ::sched_getcpu();
#else
    return -1;
#endif
}
inline int current_numa_node() { int c = current_cpu(); return c < 0 ? 0 : numa_node_of_cpu(unsigned(c)); }

// Bind the calling thread to the CPUs of one node.
inline status bind_thread_to_node(unsigned node) {
    auto ns = numa_nodes(); const numa_node* n = nullptr; for (auto& x : ns) if (x.id == node) n = &x;
    if (!n) return {{}, {22, "numa: no node " + std::to_string(node)}};
#if defined(__linux__)
    return pin_thread_to(n->cpus);   // bounds-checked, CPU_ALLOC beyond CPU_SETSIZE
#elif defined(FORGE_OS_WINDOWS)
    // A thread's affinity lives in one processor group: a node spanning groups binds to its first one.
    for (auto& ga : detail::numa_node_groups(USHORT(node))) if (ga.Mask && SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr)) return {};
    DWORD_PTR m = 0; for (auto c : n->cpus) if (c < sizeof(DWORD_PTR) * 8) m |= DWORD_PTR(1) << c;   // group-0 fallback (wine, old kernels)
    if (m && SetThreadAffinityMask(GetCurrentThread(), m)) return {};
    return {{}, last_error("SetThreadAffinityMask")};
#elif defined(FORGE_OS_FREEBSD)
    if (ns.size() > 1) {
        if (node >= DOMAINSET_SETSIZE) return {{}, {22, "numa: node out of range"}};
        domainset_t d; DOMAINSET_ZERO(&d); DOMAINSET_SET(node, &d);
        if (::cpuset_setdomain(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof d, &d, DOMAINSET_POLICY_PREFER)) return {{}, last_error("cpuset_setdomain")};
        if (!n->cpus.empty()) return pin_thread_to(n->cpus);
    }
    return {};
#else
    return {};   // UMA: node 0 is everything
#endif
}

// Node-local allocation (page-granular). Free with numa_free(p, n).
inline result<void*> numa_alloc(std::size_t n, unsigned node) {
    if (!n) return {nullptr, {22, "numa_alloc: size 0"}};
#if defined(FORGE_OS_WINDOWS)
    void* p = VirtualAllocExNuma(GetCurrentProcess(), nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, DWORD(node));
    if (!p) return {nullptr, last_error("VirtualAllocExNuma")};
    return {p, {}};
#else
    void* p = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return {nullptr, last_error("mmap")};
#if defined(__linux__)
    if (numa_node_count() > 1) {
        unsigned long mask[16] = {}; if (node < 1024) mask[node / 64] |= 1ul << (node % 64);
        constexpr int MPOL_BIND_ = 2;
        if (::syscall(SYS_mbind, p, n, MPOL_BIND_, mask, 1024ul, 0u)) { auto e = last_error("mbind"); ::munmap(p, n); return {nullptr, e}; }
    }
#elif defined(FORGE_OS_FREEBSD)
    if (numa_node_count() > 1) {   // first-touch every page under a domain policy for this thread, then restore
        domainset_t old; int pol = 0; DOMAINSET_ZERO(&old);
        bool saved = ::cpuset_getdomain(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof old, &old, &pol) == 0;
        domainset_t d; DOMAINSET_ZERO(&d); DOMAINSET_SET(node, &d);
        ::cpuset_setdomain(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof d, &d, DOMAINSET_POLICY_PREFER);
        for (std::size_t o = 0; o < n; o += page_size()) static_cast<volatile char*>(p)[o] = 0;
        if (saved) ::cpuset_setdomain(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof old, &old, pol);
    }
#endif
    (void)node; return {p, {}};
#endif
}
inline void numa_free(void* p, std::size_t n) {
    if (!p) return;
#if defined(FORGE_OS_WINDOWS)
    (void)n; VirtualFree(p, 0, MEM_RELEASE);
#else
    ::munmap(p, n);
#endif
}
// Which node backs the page at p (Linux: move_pages; Windows: QueryWorkingSetEx); -1 when unknown or the page is
// not resident (touch it first).
inline int numa_node_of_address(const void* p) {
#if defined(__linux__)
    void* pages[1] = {const_cast<void*>(p)}; int st[1] = {-1};
    if (::syscall(SYS_move_pages, 0, 1ul, pages, nullptr, st, 0)) return -1;
    return st[0] < 0 ? -1 : st[0];
#elif defined(FORGE_OS_WINDOWS)
    PSAPI_WORKING_SET_EX_INFORMATION w{}; w.VirtualAddress = const_cast<void*>(p);
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), &w, sizeof w)) return -1;
    auto bits = std::uint64_t(w.VirtualAttributes.Flags);   // Valid:1 ShareCount:3 Win32Protection:11 Shared:1 Node:6
    return (bits & 1) ? int((bits >> 16) & 0x3f) : -1;
#else
    (void)p; return numa_node_count() == 1 ? 0 : -1;
#endif
}
} // namespace forge::os
