// docs/graph.md example: build a graph, optimise it, compile a plan, run it, differentiate it.
#include <forge/graph/autodiff.hpp>
#include <forge/graph/graph.hpp>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace forge;
using namespace forge::graph;

int main() {
    tensor_graph g;
    auto x = g.input("x", {2, 4});
    auto y = g.mul(g.apply(ops::op::silu, {x}), g.apply(ops::op::exp, {x}));   // fused to silu_mul by optimise()
    g.output("y", y);
    auto flat = g.apply(ops::op::reshape, {y}, {{"shape", std::vector<i64>{8}}});
    auto loss = g.apply(ops::op::reduce_sum, {flat}, {{"axis", 0.0}});          // scalar loss = sum(y)
    auto grads = grad(g, loss, {x});                                              // reverse-mode autodiff
    g.output("dx", grads[x]);

    auto plan = compile(g);                 // passes -> memory plan -> lowering (inference caches plans per shape bucket)
    executor ex(*plan);
    std::vector<float> in{0, 1, -1, 2, 0.5f, -0.5f, 3, -3};
    ex.set_input("x", in);
    ex.run();
    tensor out = ex.output("y");
    double ref = (1.0 / (1 + std::exp(-1.0))) * std::exp(1.0);     // silu(1) * exp(1)
    std::printf("y[1]=%.5f (ref %.5f), dx[0]=%.5f\n", out.data[1], ref, ex.output("dx").data[0]);
    // d/dx silu(x)exp(x) at x=0 is 0.5
    return std::fabs(out.data[1] - ref) < 1e-4 && std::fabs(ex.output("dx").data[0] - 0.5f) < 1e-4 ? 0 : 1;
}
