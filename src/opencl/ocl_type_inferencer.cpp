/**
 * @file src/opencl/ocl_type_inferencer.cpp
 * @brief OCLTypeInferencer: parallel union-find type propagation.
 *
 * GPU path:
 *   1. Build flat SOA from FunctionTypeData vector.
 *   2. Upload to device.
 *   3. Run retdec_type_seed once (operand hints → width/sign/ptr arrays).
 *   4. Repeat retdec_type_propagation until global_done stays 1 or max iters.
 *   5. Read back parent[], width[], signedness[], is_pointer[].
 *   6. Walk DSU roots to fill output TypeSlot vector.
 *
 * CPU fallback:
 *   Same algorithm in plain C++.
 */

#include <memory>
#include "retdec/opencl/ocl_type_inferencer.h"
#include "retdec/opencl/ocl_context.h"
#include "retdec/opencl/kernel_sources.h"

#include <CL/cl.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

namespace retdec {
namespace opencl {

// ─── CPU-side DSU helpers ─────────────────────────────────────────────────────
namespace cpu_dsu {

static std::uint8_t mergeWidth(std::uint8_t a, std::uint8_t b) noexcept
{
    if (a == 0) return b;
    if (b == 0) return a;
    return std::max(a, b);
}

static TypeSign mergeSign(TypeSign a, TypeSign b) noexcept
{
    if (a == TypeSign::Unknown) return b;
    if (b == TypeSign::Unknown) return a;
    if (a == b) return a;
    return TypeSign::Signed;
}

static std::uint32_t find(std::vector<std::uint32_t>& parent, std::uint32_t x)
{
    std::uint32_t root = x;
    while (parent[root] != root) root = parent[root];
    // Path compress
    while (parent[x] != root) {
        std::uint32_t next = parent[x];
        parent[x] = root;
        x = next;
    }
    return root;
}

static bool unite(std::vector<std::uint32_t>& parent,
                  std::vector<std::uint32_t>& rank,
                  std::vector<std::uint8_t>&  width,
                  std::vector<TypeSign>&       sign,
                  std::vector<bool>&           ptr,
                  std::uint32_t                ra,
                  std::uint32_t                rb)
{
    if (ra == rb) return false;

    std::uint8_t mw  = mergeWidth(width[ra], width[rb]);
    TypeSign     ms  = mergeSign(sign[ra], sign[rb]);
    bool         mp  = ptr[ra] || ptr[rb];

    std::uint32_t winner, loser;
    if (rank[ra] > rank[rb]) { winner = ra; loser = rb; }
    else if (rank[rb] > rank[ra]) { winner = rb; loser = ra; }
    else { winner = ra; loser = rb; rank[ra]++; }

    parent[loser] = winner;
    width[winner] = mw;
    sign[winner]  = ms;
    ptr[winner]   = mp;
    return true;
}

} // namespace cpu_dsu

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct OCLTypeInferencer::Impl {
    OCLContext*   ctx     = nullptr;
    bool          useGPU  = false;
    std::string   lastErr;
    std::uint32_t lastIter = 0;

    bool ensureKernels()
    {
        if (!ctx || !ctx->isReady()) return false;
        std::string log;
        bool ok = ctx->ensureProgram(
            "retdec_type_propagation",
            typePropagationClSource(),
            "",
            &log);
        if (!ok) {
            lastErr = "type_propagation build failed: " + log;
        }
        return ok;
    }

    // ── GPU path ──────────────────────────────────────────────────────────────

    std::vector<TypeSlot> runGPU(const std::vector<FunctionTypeData>& funcs,
                                 const std::vector<std::uint32_t>&    funcOffsets,
                                 std::uint32_t                         totalSlots,
                                 const std::vector<std::uint32_t>&    conOffsets,
                                 const std::vector<std::uint32_t>&    conA,
                                 const std::vector<std::uint32_t>&    conB,
                                 const std::vector<std::uint32_t>&    opSlot,
                                 const std::vector<std::uint8_t>&     opWidth,
                                 const std::vector<std::uint8_t>&     opSign,
                                 const std::vector<std::uint8_t>&     opPtr)
    {
        const std::uint32_t F = static_cast<std::uint32_t>(funcs.size());
        const std::uint32_t S = totalSlots;
        const std::uint32_t C = static_cast<std::uint32_t>(conA.size());
        const std::uint32_t O = static_cast<std::uint32_t>(opSlot.size());

        // ── Initialize DSU parent[] = identity, rank[] = 0 ───────────────
        std::vector<std::uint32_t> parent(S);
        std::iota(parent.begin(), parent.end(), 0u);
        std::vector<std::uint32_t> rank(S, 0u);
        std::vector<std::uint8_t>  width(S, 0u);
        std::vector<std::uint8_t>  signedness(S, 0u);
        std::vector<std::uint8_t>  isPtrVec(S, 0u);

        cl_int err = CL_SUCCESS;

        auto makeBuf = [&](std::size_t bytes) -> cl_mem {
            cl_mem b = ctx->createBuffer(bytes ? bytes : 4, &err);
            return b;
        };

        cl_mem dParent  = makeBuf(S * sizeof(std::uint32_t));
        cl_mem dRank    = makeBuf(S * sizeof(std::uint32_t));
        cl_mem dWidth   = makeBuf(S);
        cl_mem dSign    = makeBuf(S);
        cl_mem dPtr     = makeBuf(S);
        cl_mem dConOff  = makeBuf((F + 1) * sizeof(std::uint32_t));
        cl_mem dConA    = makeBuf(C ? C * sizeof(std::uint32_t) : 4);
        cl_mem dConB    = makeBuf(C ? C * sizeof(std::uint32_t) : 4);
        cl_mem dDirty   = makeBuf(F * sizeof(std::uint32_t));
        cl_mem dDone    = makeBuf(sizeof(std::uint32_t));
        cl_mem dOpSlot  = makeBuf(O ? O * sizeof(std::uint32_t) : 4);
        cl_mem dOpWidth = makeBuf(O ? O : 4);
        cl_mem dOpSign  = makeBuf(O ? O : 4);
        cl_mem dOpPtr   = makeBuf(O ? O : 4);

        if (!dParent || !dRank || !dWidth || !dSign || !dPtr) {
            lastErr = "GPU buffer allocation failed";
            // cleanup
            for (cl_mem m : {dParent,dRank,dWidth,dSign,dPtr,dConOff,dConA,dConB,
                              dDirty,dDone,dOpSlot,dOpWidth,dOpSign,dOpPtr}) {
                if (m) clReleaseMemObject(m);
            }
            return {};
        }

        ctx->writeBuffer(dParent, parent.data(), S * sizeof(std::uint32_t));
        ctx->writeBuffer(dRank,   rank.data(),   S * sizeof(std::uint32_t));
        ctx->writeBuffer(dWidth,  width.data(),  S);
        ctx->writeBuffer(dSign,   signedness.data(), S);
        ctx->writeBuffer(dPtr,    isPtrVec.data(), S);
        ctx->writeBuffer(dConOff, conOffsets.data(), (F + 1) * sizeof(std::uint32_t));
        if (C) {
            ctx->writeBuffer(dConA, conA.data(), C * sizeof(std::uint32_t));
            ctx->writeBuffer(dConB, conB.data(), C * sizeof(std::uint32_t));
        }
        if (O) {
            ctx->writeBuffer(dOpSlot,  opSlot.data(),  O * sizeof(std::uint32_t));
            ctx->writeBuffer(dOpWidth, opWidth.data(), O);
            ctx->writeBuffer(dOpSign,  opSign.data(),  O);
            ctx->writeBuffer(dOpPtr,   opPtr.data(),   O);
        }

        // ── Run seed kernel ───────────────────────────────────────────────
        if (O > 0) {
            const std::uint32_t numOps = O;
            OCLKernelLaunch seedLaunch;
            seedLaunch.programKey = "retdec_type_propagation";
            seedLaunch.kernelName = "retdec_type_seed";
            seedLaunch.globalSize = O;
            seedLaunch.localSize  = 0;
            seedLaunch.setArgs    = [&](cl_kernel k) {
                clSetKernelArg(k, 0, sizeof(cl_mem), &dWidth);
                clSetKernelArg(k, 1, sizeof(cl_mem), &dSign);
                clSetKernelArg(k, 2, sizeof(cl_mem), &dPtr);
                clSetKernelArg(k, 3, sizeof(cl_mem), &dOpSlot);
                clSetKernelArg(k, 4, sizeof(cl_mem), &dOpWidth);
                clSetKernelArg(k, 5, sizeof(cl_mem), &dOpSign);
                clSetKernelArg(k, 6, sizeof(cl_mem), &dOpPtr);
                clSetKernelArg(k, 7, sizeof(cl_uint), &numOps);
            };
            ctx->runKernel(seedLaunch, true);
        }

        // ── Propagation loop until convergence ────────────────────────────
        lastIter = 0;
        for (std::uint32_t iter = 0; iter < kMaxIterations; ++iter) {
            ++lastIter;

            // Reset dirty flags and global_done.
            std::vector<std::uint32_t> zeros(F, 0u);
            ctx->writeBuffer(dDirty, zeros.data(), F * sizeof(std::uint32_t));
            std::uint32_t one = 1u;
            ctx->writeBuffer(dDone, &one, sizeof(std::uint32_t));

            OCLKernelLaunch propLaunch;
            propLaunch.programKey = "retdec_type_propagation";
            propLaunch.kernelName = "retdec_type_propagation";
            propLaunch.globalSize = F;
            propLaunch.localSize  = 0;
            propLaunch.setArgs    = [&](cl_kernel k) {
                clSetKernelArg(k, 0, sizeof(cl_mem), &dParent);
                clSetKernelArg(k, 1, sizeof(cl_mem), &dRank);
                clSetKernelArg(k, 2, sizeof(cl_mem), &dWidth);
                clSetKernelArg(k, 3, sizeof(cl_mem), &dSign);
                clSetKernelArg(k, 4, sizeof(cl_mem), &dPtr);
                clSetKernelArg(k, 5, sizeof(cl_mem), &dConOff);
                clSetKernelArg(k, 6, sizeof(cl_mem), &dConA);
                clSetKernelArg(k, 7, sizeof(cl_mem), &dConB);
                clSetKernelArg(k, 8, sizeof(cl_mem), &dDirty);
                clSetKernelArg(k, 9, sizeof(cl_mem), &dDone);
                clSetKernelArg(k, 10, sizeof(cl_uint), &F);
            };
            ctx->runKernel(propLaunch, true);

            std::uint32_t done = 0;
            ctx->readBuffer(dDone, &done, sizeof(std::uint32_t));
            if (done != 0) break;
        }

        // ── Read back results ─────────────────────────────────────────────
        ctx->readBuffer(dParent, parent.data(), S * sizeof(std::uint32_t));
        ctx->readBuffer(dWidth,  width.data(),  S);
        ctx->readBuffer(dSign,   signedness.data(), S);
        ctx->readBuffer(dPtr,    isPtrVec.data(), S);

        for (cl_mem m : {dParent,dRank,dWidth,dSign,dPtr,dConOff,dConA,dConB,
                          dDirty,dDone,dOpSlot,dOpWidth,dOpSign,dOpPtr}) {
            if (m) clReleaseMemObject(m);
        }

        // ── Build output: for each slot, read from its DSU root ───────────
        std::vector<TypeSlot> out(S);
        for (std::uint32_t i = 0; i < S; ++i) {
            std::uint32_t root = i;
            for (int d = 0; d < 64 && parent[root] != root; ++d) root = parent[root];
            out[i].widthBytes = width[root];
            out[i].sign       = static_cast<TypeSign>(signedness[root]);
            out[i].isPointer  = (isPtrVec[root] != 0);
        }
        return out;
    }

    // ── CPU fallback ──────────────────────────────────────────────────────────

    std::vector<TypeSlot> runCPU(const std::vector<FunctionTypeData>& funcs,
                                 const std::vector<std::uint32_t>&    funcOffsets,
                                 std::uint32_t                         totalSlots,
                                 const std::vector<std::uint32_t>&    conOffsets,
                                 const std::vector<std::uint32_t>&    conA,
                                 const std::vector<std::uint32_t>&    conB,
                                 const std::vector<std::uint32_t>&    opSlot,
                                 const std::vector<std::uint8_t>&     opWidth,
                                 const std::vector<std::uint8_t>&     opSign,
                                 const std::vector<std::uint8_t>&     opPtr)
    {
        const std::uint32_t S = totalSlots;
        const std::uint32_t F = static_cast<std::uint32_t>(funcs.size());

        std::vector<std::uint32_t> parent(S);
        std::iota(parent.begin(), parent.end(), 0u);
        std::vector<std::uint32_t> rank(S, 0u);
        std::vector<std::uint8_t>  width(S, 0u);
        std::vector<TypeSign>      sign(S, TypeSign::Unknown);
        std::vector<bool>          ptr(S, false);

        // Seed
        for (std::size_t i = 0; i < opSlot.size(); ++i) {
            std::uint32_t sl = opSlot[i];
            width[sl] = cpu_dsu::mergeWidth(width[sl], opWidth[i]);
            sign[sl]  = cpu_dsu::mergeSign(sign[sl], static_cast<TypeSign>(opSign[i]));
            ptr[sl]  |= (opPtr[i] != 0);
        }

        // Propagation loop
        lastIter = 0;
        for (std::uint32_t iter = 0; iter < kMaxIterations; ++iter) {
            ++lastIter;
            bool changed = false;
            for (std::size_t ci = 0; ci < conA.size(); ++ci) {
                std::uint32_t ra = cpu_dsu::find(parent, conA[ci]);
                std::uint32_t rb = cpu_dsu::find(parent, conB[ci]);
                if (cpu_dsu::unite(parent, rank, width, sign, ptr, ra, rb)) {
                    changed = true;
                }
            }
            if (!changed) break;
        }

        // Build output
        std::vector<TypeSlot> out(S);
        for (std::uint32_t i = 0; i < S; ++i) {
            std::uint32_t root = cpu_dsu::find(parent, i);
            out[i].widthBytes = width[root];
            out[i].sign       = sign[root];
            out[i].isPointer  = ptr[root];
        }
        return out;
    }
};

// ─── OCLTypeInferencer ────────────────────────────────────────────────────────

OCLTypeInferencer::OCLTypeInferencer(OCLContext* ctx)
    : _impl(std::make_unique<Impl>())
{
    _impl->ctx = ctx;
    if (ctx && ctx->isReady()) {
        _impl->useGPU = _impl->ensureKernels();
    }
}

OCLTypeInferencer::~OCLTypeInferencer() = default;

OCLTypeInferencer::OCLTypeInferencer(OCLTypeInferencer&&) noexcept            = default;
OCLTypeInferencer& OCLTypeInferencer::operator=(OCLTypeInferencer&&) noexcept = default;

bool OCLTypeInferencer::usesGPU() const noexcept
{
    return _impl && _impl->useGPU;
}

const std::string& OCLTypeInferencer::lastError() const noexcept
{
    static const std::string kEmpty;
    return _impl ? _impl->lastErr : kEmpty;
}

std::uint32_t OCLTypeInferencer::lastIterations() const noexcept
{
    return _impl ? _impl->lastIter : 0u;
}

std::vector<TypeSlot>
OCLTypeInferencer::infer(const std::vector<FunctionTypeData>& functions)
{
    if (functions.empty()) return {};

    const std::uint32_t F = static_cast<std::uint32_t>(functions.size());

    // ── Build flat SOA ────────────────────────────────────────────────────────
    std::vector<std::uint32_t> funcOffsets(F + 1, 0u);
    std::vector<std::uint32_t> conOffsets(F + 1, 0u);

    for (std::uint32_t i = 0; i < F; ++i) {
        funcOffsets[i + 1] = funcOffsets[i] + functions[i].numSlots;
        conOffsets [i + 1] = conOffsets[i]  + static_cast<std::uint32_t>(functions[i].constraints.size());
    }

    const std::uint32_t totalSlots = funcOffsets[F];
    if (totalSlots == 0) return {};

    // Constraints (translate local slot IDs to global)
    std::vector<std::uint32_t> conA, conB;
    conA.reserve(conOffsets[F]);
    conB.reserve(conOffsets[F]);

    for (std::uint32_t fi = 0; fi < F; ++fi) {
        std::uint32_t base = funcOffsets[fi];
        for (const auto& c : functions[fi].constraints) {
            conA.push_back(base + c.slotA);
            conB.push_back(base + c.slotB);
        }
    }

    // Operand hints (translate local → global)
    std::vector<std::uint32_t> opSlot;
    std::vector<std::uint8_t>  opWidth, opSign, opPtr;

    for (std::uint32_t fi = 0; fi < F; ++fi) {
        std::uint32_t base = funcOffsets[fi];
        for (const auto& h : functions[fi].operandHints) {
            opSlot.push_back(base + h.slot);
            opWidth.push_back(h.widthBytes);
            opSign.push_back(static_cast<std::uint8_t>(h.sign));
            opPtr.push_back(h.isPointer ? 1u : 0u);
        }
    }

    if (_impl->useGPU) {
        auto result = _impl->runGPU(
            functions, funcOffsets, totalSlots,
            conOffsets, conA, conB,
            opSlot, opWidth, opSign, opPtr);
        if (!result.empty()) return result;
        // Fall through to CPU on GPU failure.
    }
    return _impl->runCPU(
        functions, funcOffsets, totalSlots,
        conOffsets, conA, conB,
        opSlot, opWidth, opSign, opPtr);
}

} // namespace opencl
} // namespace retdec
