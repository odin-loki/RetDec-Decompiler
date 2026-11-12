// docs/loadbalancer.md example: a pool with power-of-two-choices, leases, and retries on another backend.
#include <forge/loadbalancer/loadbalancer.hpp>
#include <cstdio>
#include <set>
#include <string>
using namespace forge::loadbalancer;

int main() {
    config c;                               // default strategy: power_of_two (P2C)
    c.strat = *parse_strategy("p2c");       // same names as forge-serve --strategy
    pool p(c);
    p.add({"gpu0", "10.0.0.5:7000", 1.0});
    p.add({"gpu1", "10.0.0.6:7000", 1.0});

    {   // RAII lease: report the outcome, or it counts as a failure when dropped
        lease l = p.acquire();
        if (!l) return 1;
        std::printf("picked %s\n", l.backend().id.c_str());
        l.success_with_latency(1500);       // nanoseconds of latency, feeds the EWMA
    }

    // pool::call retries on a different backend (attempts, base backoff ns, max backoff ns).
    std::set<std::string> tried;
    bool ok = p.call({}, [&](const backend_spec& b) { tried.insert(b.id); return tried.size() == 2; }, {3, 1000, 10000});
    std::printf("call ok=%d after %zu backends\n", int(ok), tried.size());
    return ok && tried.size() == 2 ? 0 : 1;
}
