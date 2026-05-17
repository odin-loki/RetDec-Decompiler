/**
 * @file src/opencl/ocl_egraph_simplifier.cpp
 * @brief OCLEGraphSimplifier implementation — GPU and CPU paths.
 */

#include <memory>
#include "retdec/opencl/ocl_egraph_simplifier.h"
#include "retdec/opencl/ocl_context.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <CL/cl.h>
#include "retdec/opencl/kernel_sources.h"

namespace retdec {
namespace opencl {

// ─── C-distance lookup (mirrors kernel cdist()) ──────────────────────────────

static uint32_t cdist(EOpcode op) noexcept
{
    switch (op) {
        case EOpcode::LIT:      return 0;
        case EOpcode::VAR:      return 0;
        case EOpcode::ARRAY:    return 1;
        case EOpcode::FIELD:    return 1;
        case EOpcode::BITFIELD: return 2;
        case EOpcode::ADD:      return 3;
        case EOpcode::SUB:      return 3;
        case EOpcode::DEREF:    return 3;
        case EOpcode::MUL:      return 4;
        case EOpcode::DIV:      return 4;
        case EOpcode::CAST:     return 5;
        case EOpcode::AND:      return 6;
        case EOpcode::OR:       return 6;
        case EOpcode::XOR:      return 6;
        case EOpcode::SHL:      return 6;
        case EOpcode::SHR:      return 6;
        case EOpcode::ASHR:     return 6;
        default:                return 99;
    }
}

// ─── CPU union-find ──────────────────────────────────────────────────────────

static uint32_t cpuFind(std::vector<uint32_t> &parent, uint32_t x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]]; // path halving
        x = parent[x];
    }
    return x;
}

static bool cpuUnion(std::vector<uint32_t> &parent, std::vector<uint32_t> &rank,
                     uint32_t a, uint32_t b)
{
    a = cpuFind(parent, a);
    b = cpuFind(parent, b);
    if (a == b) return false;
    if (rank[a] < rank[b]) std::swap(a, b);
    parent[b] = a;
    if (rank[a] == rank[b]) ++rank[a];
    return true;
}

// ─── CPU literal query ───────────────────────────────────────────────────────

static bool classLiteral(const std::vector<uint32_t> &op,
                         const std::vector<uint32_t> &ec,
                         const std::vector<uint64_t> &lit,
                         std::vector<uint32_t>       &parent,
                         uint32_t cls, uint64_t &val)
{
    uint32_t root = cpuFind(parent, cls);
    for (size_t i = 0; i < op.size(); ++i) {
        if (op[i] == static_cast<uint32_t>(EOpcode::LIT) &&
            cpuFind(parent, ec[i]) == root)
        {
            val = lit[i];
            return true;
        }
    }
    return false;
}

// ─── CPU saturation ──────────────────────────────────────────────────────────

static bool cpuSaturateOnce(EGraph &g)
{
    auto &op     = g.op();
    auto &ec     = g.eclass();
    auto &lhs    = g.lhs();
    auto &rhs    = g.rhs();
    auto &aux    = g.aux();
    auto &lit    = g.lit();
    auto &parent = g.ufParent();
    auto &rank   = g.ufRank();

    const uint32_t n = g.nodeCount();
    bool dirty = false;

    for (uint32_t id = 0; id < n; ++id) {
        auto myop  = static_cast<EOpcode>(op[id]);
        uint32_t mycls = cpuFind(parent, ec[id]);
        uint32_t l = (lhs[id] != kNoClass) ? cpuFind(parent, lhs[id]) : kNoClass;
        uint32_t r = (rhs[id] != kNoClass) ? cpuFind(parent, rhs[id]) : kNoClass;

        // Rule 1: x+0→x, x-0→x, x|0→x, x^0→x
        if ((myop == EOpcode::ADD || myop == EOpcode::SUB ||
             myop == EOpcode::OR  || myop == EOpcode::XOR)
            && l != kNoClass && r != kNoClass)
        {
            uint64_t rv = 0;
            if (classLiteral(op, ec, lit, parent, r, rv) && rv == 0)
                dirty |= cpuUnion(parent, rank, mycls, l);
        }

        // x & ~0 → x
        if (myop == EOpcode::AND && l != kNoClass && r != kNoClass) {
            uint64_t rv = 0;
            if (classLiteral(op, ec, lit, parent, r, rv) && rv == ~0ULL)
                dirty |= cpuUnion(parent, rank, mycls, l);
        }

        // Rule 2: x*1 → x, 1*x → x
        if (myop == EOpcode::MUL && l != kNoClass && r != kNoClass) {
            uint64_t rv = 0, lv = 0;
            if (classLiteral(op, ec, lit, parent, r, rv) && rv == 1)
                dirty |= cpuUnion(parent, rank, mycls, l);
            if (classLiteral(op, ec, lit, parent, l, lv) && lv == 1)
                dirty |= cpuUnion(parent, rank, mycls, r);
        }

        // Rule 3: x*0 → 0
        if (myop == EOpcode::MUL && l != kNoClass && r != kNoClass) {
            uint64_t rv = 0, lv = 0;
            bool rz = classLiteral(op, ec, lit, parent, r, rv) && rv == 0;
            bool lz = classLiteral(op, ec, lit, parent, l, lv) && lv == 0;
            if (rz || lz) {
                for (size_t i = 0; i < op.size(); ++i) {
                    if (op[i] == static_cast<uint32_t>(EOpcode::LIT) && lit[i] == 0) {
                        dirty |= cpuUnion(parent, rank, mycls, cpuFind(parent, ec[i]));
                        break;
                    }
                }
            }
        }

        // Rule 4: x>>0 → x, x<<0 → x
        if ((myop == EOpcode::SHL || myop == EOpcode::SHR || myop == EOpcode::ASHR)
            && l != kNoClass && r != kNoClass)
        {
            uint64_t rv = 0;
            if (classLiteral(op, ec, lit, parent, r, rv) && rv == 0)
                dirty |= cpuUnion(parent, rank, mycls, l);
        }

        // Rule 5: (x >> k) & mask → BITFIELD
        if (myop == EOpcode::AND && l != kNoClass && r != kNoClass) {
            for (uint32_t si = 0; si < n; ++si) {
                auto sop = static_cast<EOpcode>(op[si]);
                if ((sop == EOpcode::SHR || sop == EOpcode::ASHR)
                    && cpuFind(parent, ec[si]) == l
                    && lhs[si] != kNoClass && rhs[si] != kNoClass)
                {
                    uint64_t kv = 0, mv = 0;
                    if (classLiteral(op, ec, lit, parent, cpuFind(parent, rhs[si]), kv) &&
                        classLiteral(op, ec, lit, parent, r, mv) && mv != 0)
                    {
                        uint64_t m = mv >> kv;
                        bool contiguous = (m != 0) && ((m & (m + 1)) == 0);
                        if (contiguous) {
                            op[id]  = static_cast<uint32_t>(EOpcode::BITFIELD);
                            lhs[id] = cpuFind(parent, lhs[si]);
                            rhs[id] = kNoClass;
                            aux[id] = static_cast<uint32_t>(kv);
                            dirty   = true;
                        }
                    }
                    break;
                }
            }
        }

        // Rule 6: CAST(CAST(x, w1), w2) → CAST(x, w2) when w2 <= w1
        if (myop == EOpcode::CAST && l != kNoClass) {
            for (uint32_t ci = 0; ci < n; ++ci) {
                if (op[ci] == static_cast<uint32_t>(EOpcode::CAST)
                    && cpuFind(parent, ec[ci]) == l
                    && lhs[ci] != kNoClass)
                {
                    if (aux[id] <= aux[ci]) {
                        lhs[id] = lhs[ci];
                        dirty   = true;
                    }
                    break;
                }
            }
        }

        // Rule 7: DEREF(ADD(base, idx/offset)) → ARRAY/FIELD
        if (myop == EOpcode::DEREF && l != kNoClass) {
            for (uint32_t ai = 0; ai < n; ++ai) {
                if (op[ai] == static_cast<uint32_t>(EOpcode::ADD)
                    && cpuFind(parent, ec[ai]) == l
                    && lhs[ai] != kNoClass && rhs[ai] != kNoClass)
                {
                    uint64_t fld = 0;
                    if (classLiteral(op, ec, lit, parent, cpuFind(parent, rhs[ai]), fld)) {
                        op[id]  = static_cast<uint32_t>(EOpcode::FIELD);
                        lhs[id] = cpuFind(parent, lhs[ai]);
                        rhs[id] = kNoClass;
                        aux[id] = static_cast<uint32_t>(fld);
                    } else {
                        op[id]  = static_cast<uint32_t>(EOpcode::ARRAY);
                        lhs[id] = cpuFind(parent, lhs[ai]);
                        rhs[id] = cpuFind(parent, rhs[ai]);
                    }
                    dirty = true;
                    break;
                }
            }
        }
    }
    return dirty;
}

// ─── CPU extraction ──────────────────────────────────────────────────────────

static std::vector<EClassResult> cpuExtract(EGraph &g)
{
    const uint32_t nc = g.classCount();
    auto &parent = g.ufParent();

    std::vector<EClassResult> results(nc);
    for (uint32_t c = 0; c < nc; ++c) {
        results[c].classId = c;
    }

    const uint32_t nn = g.nodeCount();
    const auto &op = g.op();
    const auto &ec = g.eclass();

    for (uint32_t i = 0; i < nn; ++i) {
        uint32_t root = cpuFind(parent, ec[i]);
        if (root >= nc) continue;
        uint32_t sc = cdist(static_cast<EOpcode>(op[i]));
        if (sc < results[root].score) {
            results[root].score    = sc;
            results[root].bestNode = i;
            results[root].opcode   = static_cast<EOpcode>(op[i]);
        }
    }
    return results;
}

// ─── Impl ────────────────────────────────────────────────────────────────────

struct OCLEGraphSimplifier::Impl {
    OCLContext *ctx = nullptr;

    explicit Impl(OCLContext *c) : ctx(c) {}

    bool gpuReady() const { return ctx != nullptr && ctx->isReady(); }

    bool ensureProgram()
    {
        std::string log;
        bool ok = ctx->ensureProgram("retdec_egraph", egraphSimplifyClSource(), "", &log);
        return ok;
    }

    // ── GPU path ──────────────────────────────────────────────────────────
    std::vector<EClassResult> runGPU(EGraph &g, uint32_t maxIter)
    {
        if (!ensureProgram())
            return runCPU(g, maxIter);

        g.initUnionFind();

        const uint32_t nn = g.nodeCount();
        const uint32_t nc = g.classCount();
        if (nn == 0 || nc == 0) return runCPU(g, maxIter);

        cl_int err = CL_SUCCESS;

        // Allocate device buffers.
        auto mkBuf = [&](std::size_t bytes) -> cl_mem {
            return ctx->createBuffer(bytes ? bytes : 4, &err);
        };

        cl_mem dOp      = mkBuf(nn * sizeof(uint32_t));
        cl_mem dEc      = mkBuf(nn * sizeof(uint32_t));
        cl_mem dLhs     = mkBuf(nn * sizeof(uint32_t));
        cl_mem dRhs     = mkBuf(nn * sizeof(uint32_t));
        cl_mem dAux     = mkBuf(nn * sizeof(uint32_t));
        cl_mem dLit     = mkBuf(nn * sizeof(uint64_t));
        cl_mem dPar     = mkBuf(nc * sizeof(uint32_t));
        cl_mem dRank    = mkBuf(nc * sizeof(uint32_t));
        cl_mem dDirty   = mkBuf(sizeof(uint32_t));
        cl_mem dBNode   = mkBuf(nc * sizeof(uint32_t));
        cl_mem dBScore  = mkBuf(nc * sizeof(uint32_t));

        std::vector<cl_mem> bufs = {dOp,dEc,dLhs,dRhs,dAux,dLit,dPar,dRank,dDirty,dBNode,dBScore};
        bool anyNull = false;
        for (auto b : bufs) if (!b) { anyNull = true; break; }
        if (anyNull || err != CL_SUCCESS) {
            for (auto b : bufs) if (b) clReleaseMemObject(b);
            return runCPU(g, maxIter);
        }

        // Upload data.
        ctx->writeBuffer(dOp,   g.op().data(),     nn * sizeof(uint32_t));
        ctx->writeBuffer(dEc,   g.eclass().data(),  nn * sizeof(uint32_t));
        ctx->writeBuffer(dLhs,  g.lhs().data(),     nn * sizeof(uint32_t));
        ctx->writeBuffer(dRhs,  g.rhs().data(),     nn * sizeof(uint32_t));
        ctx->writeBuffer(dAux,  g.aux().data(),     nn * sizeof(uint32_t));
        ctx->writeBuffer(dLit,  g.lit().data(),     nn * sizeof(uint64_t));
        ctx->writeBuffer(dPar,  g.ufParent().data(), nc * sizeof(uint32_t));
        ctx->writeBuffer(dRank, g.ufRank().data(),   nc * sizeof(uint32_t));

        // Iterative saturation.
        for (uint32_t iter = 0; iter < maxIter; ++iter) {
            uint32_t zero = 0;
            ctx->writeBuffer(dDirty, &zero, sizeof(zero));

            OCLKernelLaunch sat;
            sat.programKey = "retdec_egraph";
            sat.kernelName = "retdec_egraph_saturate";
            sat.globalSize = nn;
            sat.localSize  = 0;
            sat.setArgs = [&](cl_kernel kk) {
                clSetKernelArg(kk, 0, sizeof(cl_mem), &dOp);
                clSetKernelArg(kk, 1, sizeof(cl_mem), &dEc);
                clSetKernelArg(kk, 2, sizeof(cl_mem), &dLhs);
                clSetKernelArg(kk, 3, sizeof(cl_mem), &dRhs);
                clSetKernelArg(kk, 4, sizeof(cl_mem), &dAux);
                clSetKernelArg(kk, 5, sizeof(cl_mem), &dLit);
                clSetKernelArg(kk, 6, sizeof(cl_mem), &dPar);
                clSetKernelArg(kk, 7, sizeof(cl_mem), &dRank);
                clSetKernelArg(kk, 8, sizeof(cl_mem), &dDirty);
                clSetKernelArg(kk, 9, sizeof(uint32_t), &nn);
                clSetKernelArg(kk,10, sizeof(uint32_t), &nc);
            };
            ctx->runKernel(sat, true);

            uint32_t dirty = 0;
            ctx->readBuffer(dDirty, &dirty, sizeof(dirty));
            if (!dirty) break;
        }

        // Extraction pass.
        OCLKernelLaunch ext;
        ext.programKey = "retdec_egraph";
        ext.kernelName = "retdec_egraph_extract";
        ext.globalSize = nc;
        ext.localSize  = 0;
        ext.setArgs = [&](cl_kernel kk) {
            clSetKernelArg(kk, 0, sizeof(cl_mem), &dOp);
            clSetKernelArg(kk, 1, sizeof(cl_mem), &dEc);
            clSetKernelArg(kk, 2, sizeof(cl_mem), &dPar);
            clSetKernelArg(kk, 3, sizeof(cl_mem), &dBNode);
            clSetKernelArg(kk, 4, sizeof(cl_mem), &dBScore);
            clSetKernelArg(kk, 5, sizeof(uint32_t), &nn);
            clSetKernelArg(kk, 6, sizeof(uint32_t), &nc);
        };
        ctx->runKernel(ext, true);

        // Read back op, lhs, rhs, aux (modified by saturation), union-find, and extract results.
        std::vector<uint32_t> hOp(nn), hBNode(nc), hBScore(nc);
        ctx->readBuffer(dOp,    hOp.data(),     nn * sizeof(uint32_t));
        ctx->readBuffer(dBNode, hBNode.data(),  nc * sizeof(uint32_t));
        ctx->readBuffer(dBScore,hBScore.data(), nc * sizeof(uint32_t));

        for (auto b : bufs) clReleaseMemObject(b);

        const uint32_t kNoClassGPU = 0xFFFFFFFFu;
        std::vector<EClassResult> results(nc);
        for (uint32_t c = 0; c < nc; ++c) {
            results[c].classId  = c;
            results[c].bestNode = hBNode[c];
            results[c].score    = (hBScore[c] <= 99u) ? hBScore[c] : 99u;
            if (hBNode[c] != kNoClassGPU && hBNode[c] < nn)
                results[c].opcode = static_cast<EOpcode>(hOp[hBNode[c]]);
        }
        return results;
    }

    // ── CPU path ──────────────────────────────────────────────────────────
    std::vector<EClassResult> runCPU(EGraph &g, uint32_t maxIter)
    {
        g.initUnionFind();
        for (uint32_t iter = 0; iter < maxIter; ++iter) {
            if (!cpuSaturateOnce(g)) break;
        }
        return cpuExtract(g);
    }
};

// ─── Public API ──────────────────────────────────────────────────────────────

OCLEGraphSimplifier::OCLEGraphSimplifier(OCLContext *ctx)
    : impl_(std::make_unique<Impl>(ctx))
{}

OCLEGraphSimplifier::~OCLEGraphSimplifier() = default;

bool OCLEGraphSimplifier::gpuAvailable() const
{
    return impl_->gpuReady();
}

std::vector<EClassResult> OCLEGraphSimplifier::simplify(EGraph &g, uint32_t maxIter)
{
    if (g.nodeCount() == 0 || g.classCount() == 0)
        return {};

    if (impl_->gpuReady())
        return impl_->runGPU(g, maxIter);
    return impl_->runCPU(g, maxIter);
}

} // namespace opencl
} // namespace retdec
