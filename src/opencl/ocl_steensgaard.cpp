/**
 * @file src/opencl/ocl_steensgaard.cpp
 * @brief OCLSteensgaard implementation: GPU and CPU paths.
 */

#include <memory>
#include "retdec/opencl/ocl_steensgaard.h"
#include "retdec/opencl/ocl_context.h"
#include "retdec/opencl/kernel_sources.h"

#include <CL/cl.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

namespace retdec {
namespace opencl {

// ─── CPU DSU helpers ─────────────────────────────────────────────────────────
namespace cpu {

static std::uint32_t find(std::vector<std::uint32_t>& parent, std::uint32_t x)
{
    std::uint32_t root = x;
    while (parent[root] != root) root = parent[root];
    while (parent[x] != root) {
        std::uint32_t n = parent[x];
        parent[x] = root;
        x = n;
    }
    return root;
}

static bool unite(std::vector<std::uint32_t>& parent,
                  std::vector<std::uint32_t>& rank,
                  std::uint32_t ra, std::uint32_t rb)
{
    if (ra == rb) return false;
    if (rank[ra] > rank[rb])       { parent[rb] = ra; }
    else if (rank[rb] > rank[ra])  { parent[ra] = rb; }
    else                           { parent[rb] = ra; rank[ra]++; }
    return true;
}

} // namespace cpu

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct OCLSteensgaard::Impl {
    OCLContext*   ctx      = nullptr;
    bool          useGPU   = false;
    std::string   lastErr;
    std::uint32_t lastIter = 0;

    static constexpr std::uint32_t kNoTarget = AliasResult::kNoTarget;

    bool ensureKernels()
    {
        if (!ctx || !ctx->isReady()) return false;
        std::string log;
        bool ok = ctx->ensureProgram(
            "retdec_steensgaard",
            steensgaardClSource(),
            "",
            &log);
        if (!ok) {
            lastErr = "steensgaard build failed: " + log;
        }
        return ok;
    }

    // ── CPU fallback ──────────────────────────────────────────────────────────

    AliasResult runCPU(std::uint32_t                     numVars,
                       const std::vector<PtsConstraint>& cons)
    {
        std::vector<std::uint32_t> parent(numVars);
        std::iota(parent.begin(), parent.end(), 0u);
        std::vector<std::uint32_t> rank(numVars, 0u);
        std::vector<std::uint32_t> ptsTo(numVars, kNoTarget);

        // Helper lambdas
        auto find = [&](std::uint32_t x) { return cpu::find(parent, x); };
        auto unite = [&](std::uint32_t a, std::uint32_t b) {
            return cpu::unite(parent, rank, a, b);
        };

        // Join: merge two ECRs and schedule pts_to merges
        std::vector<std::pair<std::uint32_t,std::uint32_t>> pending;

        std::function<bool(std::uint32_t, std::uint32_t)> join =
            [&](std::uint32_t a, std::uint32_t b) -> bool {
            std::uint32_t ra = find(a), rb = find(b);
            if (ra == rb) return false;
            std::uint32_t pa = ptsTo[ra], pb = ptsTo[rb];
            bool ch = unite(ra, rb);
            if (pa != kNoTarget && pb != kNoTarget) {
                pending.push_back({pa, pb});
            } else {
                std::uint32_t winner = find(a);
                if (ptsTo[winner] == kNoTarget) {
                    if (pa != kNoTarget) ptsTo[winner] = pa;
                    else if (pb != kNoTarget) ptsTo[winner] = pb;
                }
            }
            return ch;
        };

        lastIter = 0;
        bool changed = true;
        while (changed && lastIter < kMaxIterations) {
            ++lastIter;
            changed = false;

            // Wave 1: copy
            for (const auto& c : cons) {
                if (c.kind != ConstraintKind::Copy) continue;
                pending.push_back({c.varA, c.varB});
            }

            // Flush pending joins
            while (!pending.empty()) {
                auto pairs = std::move(pending);
                pending.clear();
                for (auto [a, b] : pairs) {
                    if (join(a, b)) changed = true;
                }
            }

            // Wave 2: addr-of
            for (const auto& c : cons) {
                if (c.kind != ConstraintKind::AddrOf) continue;
                std::uint32_t ra  = find(c.varA);
                std::uint32_t rb  = find(c.varB);
                std::uint32_t pra = ptsTo[ra];
                if (pra == kNoTarget) {
                    ptsTo[ra] = rb;
                    changed   = true;
                } else {
                    std::uint32_t rpra = find(pra);
                    if (unite(rpra, rb)) changed = true;
                }
            }

            // Wave 3: load/store
            for (const auto& c : cons) {
                if (c.kind == ConstraintKind::Store) {
                    std::uint32_t ra  = find(c.varA);
                    std::uint32_t pra = ptsTo[ra];
                    if (pra != kNoTarget) {
                        if (join(find(pra), find(c.varB))) changed = true;
                    }
                } else if (c.kind == ConstraintKind::Load) {
                    std::uint32_t rb  = find(c.varB);
                    std::uint32_t prb = ptsTo[rb];
                    if (prb != kNoTarget) {
                        if (join(find(c.varA), find(prb))) changed = true;
                    }
                }
            }
            while (!pending.empty()) {
                auto pairs = std::move(pending);
                pending.clear();
                for (auto [a, b] : pairs) {
                    if (join(a, b)) changed = true;
                }
            }
        }

        // Build result
        AliasResult result;
        result.aliasClass.resize(numVars);
        result.pointsTo.resize(numVars, kNoTarget);
        for (std::uint32_t i = 0; i < numVars; ++i) {
            std::uint32_t r = cpu::find(parent, i);
            result.aliasClass[i] = r;
            std::uint32_t pt = ptsTo[r];
            result.pointsTo[i] = (pt != kNoTarget) ? cpu::find(parent, pt) : kNoTarget;
        }
        return result;
    }

    // ── GPU path ──────────────────────────────────────────────────────────────

    AliasResult runGPU(std::uint32_t                     numVars,
                       const std::vector<PtsConstraint>& cons)
    {
        const std::uint32_t N = numVars;
        const std::uint32_t C = static_cast<std::uint32_t>(cons.size());
        if (N == 0) return {};

        cl_int err = CL_SUCCESS;

        // ── Buffers ───────────────────────────────────────────────────────
        std::vector<std::uint32_t> parent(N);
        std::iota(parent.begin(), parent.end(), 0u);
        std::vector<std::uint32_t> rank(N, 0u);
        std::vector<std::uint32_t> ptsTo(N, kNoTarget);

        std::vector<std::uint8_t>  hConType(C);
        std::vector<std::uint32_t> hConA(C), hConB(C);
        for (std::uint32_t i = 0; i < C; ++i) {
            hConType[i] = static_cast<std::uint8_t>(cons[i].kind);
            hConA[i]    = cons[i].varA;
            hConB[i]    = cons[i].varB;
        }

        const std::uint32_t maxPending = std::max(C, 1u);

        auto mkBuf = [&](std::size_t bytes) {
            return ctx->createBuffer(bytes ? bytes : 4, &err);
        };

        cl_mem dParent   = mkBuf(N * sizeof(std::uint32_t));
        cl_mem dRank     = mkBuf(N * sizeof(std::uint32_t));
        cl_mem dPtsTo    = mkBuf(N * sizeof(std::uint32_t));
        cl_mem dConType  = mkBuf(C ? C : 1);
        cl_mem dConA     = mkBuf(C ? C * sizeof(std::uint32_t) : 4);
        cl_mem dConB     = mkBuf(C ? C * sizeof(std::uint32_t) : 4);
        cl_mem dPendA    = mkBuf(maxPending * sizeof(std::uint32_t));
        cl_mem dPendB    = mkBuf(maxPending * sizeof(std::uint32_t));
        cl_mem dPendCnt  = mkBuf(sizeof(std::uint32_t));
        cl_mem dChanged  = mkBuf(sizeof(std::uint32_t));

        if (!dParent || !dRank || !dPtsTo) {
            lastErr = "steensgaard: GPU buffer alloc failed";
            for (cl_mem m : {dParent,dRank,dPtsTo,dConType,dConA,dConB,
                              dPendA,dPendB,dPendCnt,dChanged}) {
                if (m) clReleaseMemObject(m);
            }
            return {};
        }

        ctx->writeBuffer(dParent,  parent.data(),  N * sizeof(std::uint32_t));
        ctx->writeBuffer(dRank,    rank.data(),    N * sizeof(std::uint32_t));
        ctx->writeBuffer(dPtsTo,   ptsTo.data(),   N * sizeof(std::uint32_t));
        if (C) {
            ctx->writeBuffer(dConType, hConType.data(), C);
            ctx->writeBuffer(dConA,    hConA.data(),    C * sizeof(std::uint32_t));
            ctx->writeBuffer(dConB,    hConB.data(),    C * sizeof(std::uint32_t));
        }

        lastIter = 0;
        const std::size_t gs = C ? C : 1;

        for (std::uint32_t iter = 0; iter < kMaxIterations; ++iter) {
            ++lastIter;

            // Reset changed & pending count.
            std::uint32_t zero = 0u;
            ctx->writeBuffer(dChanged, &zero, sizeof(std::uint32_t));
            ctx->writeBuffer(dPendCnt, &zero, sizeof(std::uint32_t));

            // Wave 1: copy
            if (C > 0) {
                OCLKernelLaunch k;
                k.programKey = "retdec_steensgaard";
                k.kernelName = "retdec_steensgaard_copy";
                k.globalSize = gs;
                k.localSize  = 0;
                const std::uint32_t cNum = C;
                k.setArgs = [&](cl_kernel kk) {
                    clSetKernelArg(kk, 0, sizeof(cl_mem),  &dParent);
                    clSetKernelArg(kk, 1, sizeof(cl_mem),  &dRank);
                    clSetKernelArg(kk, 2, sizeof(cl_mem),  &dPtsTo);
                    clSetKernelArg(kk, 3, sizeof(cl_mem),  &dConType);
                    clSetKernelArg(kk, 4, sizeof(cl_mem),  &dConA);
                    clSetKernelArg(kk, 5, sizeof(cl_mem),  &dConB);
                    clSetKernelArg(kk, 6, sizeof(cl_uint), &cNum);
                    clSetKernelArg(kk, 7, sizeof(cl_mem),  &dPendA);
                    clSetKernelArg(kk, 8, sizeof(cl_mem),  &dPendB);
                    clSetKernelArg(kk, 9, sizeof(cl_mem),  &dPendCnt);
                    clSetKernelArg(kk, 10, sizeof(cl_uint), &maxPending);
                    clSetKernelArg(kk, 11, sizeof(cl_mem), &dChanged);
                };
                ctx->runKernel(k, true);
            }

            // Wave 2: addr-of
            if (C > 0) {
                OCLKernelLaunch k;
                k.programKey = "retdec_steensgaard";
                k.kernelName = "retdec_steensgaard_addr";
                k.globalSize = gs;
                k.localSize  = 0;
                const std::uint32_t cNum = C;
                k.setArgs = [&](cl_kernel kk) {
                    clSetKernelArg(kk, 0, sizeof(cl_mem),  &dParent);
                    clSetKernelArg(kk, 1, sizeof(cl_mem),  &dRank);
                    clSetKernelArg(kk, 2, sizeof(cl_mem),  &dPtsTo);
                    clSetKernelArg(kk, 3, sizeof(cl_mem),  &dConType);
                    clSetKernelArg(kk, 4, sizeof(cl_mem),  &dConA);
                    clSetKernelArg(kk, 5, sizeof(cl_mem),  &dConB);
                    clSetKernelArg(kk, 6, sizeof(cl_uint), &cNum);
                    clSetKernelArg(kk, 7, sizeof(cl_mem),  &dChanged);
                };
                ctx->runKernel(k, true);
            }

            // Wave 3: load/store
            if (C > 0) {
                OCLKernelLaunch k;
                k.programKey = "retdec_steensgaard";
                k.kernelName = "retdec_steensgaard_deref";
                k.globalSize = gs;
                k.localSize  = 0;
                const std::uint32_t cNum = C;
                k.setArgs = [&](cl_kernel kk) {
                    clSetKernelArg(kk, 0, sizeof(cl_mem),  &dParent);
                    clSetKernelArg(kk, 1, sizeof(cl_mem),  &dRank);
                    clSetKernelArg(kk, 2, sizeof(cl_mem),  &dPtsTo);
                    clSetKernelArg(kk, 3, sizeof(cl_mem),  &dConType);
                    clSetKernelArg(kk, 4, sizeof(cl_mem),  &dConA);
                    clSetKernelArg(kk, 5, sizeof(cl_mem),  &dConB);
                    clSetKernelArg(kk, 6, sizeof(cl_uint), &cNum);
                    clSetKernelArg(kk, 7, sizeof(cl_mem),  &dPendA);
                    clSetKernelArg(kk, 8, sizeof(cl_mem),  &dPendB);
                    clSetKernelArg(kk, 9, sizeof(cl_mem),  &dPendCnt);
                    clSetKernelArg(kk, 10, sizeof(cl_uint), &maxPending);
                    clSetKernelArg(kk, 11, sizeof(cl_mem), &dChanged);
                };
                ctx->runKernel(k, true);
            }

            // Check convergence.
            std::uint32_t changed = 0;
            ctx->readBuffer(dChanged, &changed, sizeof(std::uint32_t));
            if (!changed) break;
        }

        // Read back.
        ctx->readBuffer(dParent, parent.data(), N * sizeof(std::uint32_t));
        ctx->readBuffer(dPtsTo,  ptsTo.data(),  N * sizeof(std::uint32_t));

        for (cl_mem m : {dParent,dRank,dPtsTo,dConType,dConA,dConB,
                          dPendA,dPendB,dPendCnt,dChanged}) {
            if (m) clReleaseMemObject(m);
        }

        AliasResult result;
        result.aliasClass.resize(N);
        result.pointsTo.resize(N, kNoTarget);
        for (std::uint32_t i = 0; i < N; ++i) {
            std::uint32_t r = i;
            for (int d = 0; d < 64 && parent[r] != r; ++d) r = parent[r];
            result.aliasClass[i] = r;
            std::uint32_t pt = ptsTo[r];
            if (pt != kNoTarget) {
                for (int d = 0; d < 64 && parent[pt] != pt; ++d) pt = parent[pt];
                result.pointsTo[i] = pt;
            }
        }
        return result;
    }
};

// ─── OCLSteensgaard ───────────────────────────────────────────────────────────

OCLSteensgaard::OCLSteensgaard(OCLContext* ctx)
    : _impl(std::make_unique<Impl>())
{
    _impl->ctx = ctx;
    if (ctx && ctx->isReady()) {
        _impl->useGPU = _impl->ensureKernels();
    }
}

OCLSteensgaard::~OCLSteensgaard()                                      = default;
OCLSteensgaard::OCLSteensgaard(OCLSteensgaard&&) noexcept              = default;
OCLSteensgaard& OCLSteensgaard::operator=(OCLSteensgaard&&) noexcept   = default;

bool OCLSteensgaard::usesGPU() const noexcept         { return _impl && _impl->useGPU; }
std::uint32_t OCLSteensgaard::lastIterations() const noexcept { return _impl ? _impl->lastIter : 0u; }

const std::string& OCLSteensgaard::lastError() const noexcept
{
    static const std::string kEmpty;
    return _impl ? _impl->lastErr : kEmpty;
}

AliasResult OCLSteensgaard::analyze(std::uint32_t                     numVars,
                                     const std::vector<PtsConstraint>& constraints)
{
    if (numVars == 0) return {};

    if (_impl->useGPU) {
        auto r = _impl->runGPU(numVars, constraints);
        if (!r.aliasClass.empty()) return r;
    }
    return _impl->runCPU(numVars, constraints);
}

} // namespace opencl
} // namespace retdec
