// docs/os.md example: query topology and NUMA, use shared memory, and handle "unsupported" gracefully.
#include <forge/os/all.hpp>
#include <cstdio>
#include <cstring>
#include <string>
using namespace forge;

int main() {
    auto t = os::cpu_topology_query();
    std::printf("%s: %u logical / %u physical, hybrid=%d\n", t.brand.c_str(), t.logical, t.physical, int(t.hybrid));

    auto nodes = os::numa_nodes();                              // always at least one node
    std::printf("numa nodes: %zu\n", nodes.size());

    std::string name = "doc-example-" + std::to_string(os::pid());
    auto shm = os::shared_memory::create(name, 4096);          // result<shared_memory>
    if (!shm) { std::printf("shm failed: %s\n", shm.err.what.c_str()); return 1; }
    std::memcpy(shm->data(), "hi", 3);
    auto again = os::shared_memory::open(name, 4096);
    bool same = again && std::strcmp(reinterpret_cast<char*>(again->data()), "hi") == 0;
    shm->close();

    // Features a platform lacks report is_unsupported() instead of failing hard.
    auto pin = os::pin_thread_to_kind(os::core_kind::performance);
    std::printf("pin to P-core: %s\n", pin ? "ok" : (os::is_unsupported(pin.err) ? "unsupported" : pin.err.what.c_str()));
    return same && !nodes.empty() ? 0 : 1;
}
