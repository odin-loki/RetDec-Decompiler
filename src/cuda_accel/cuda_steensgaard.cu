/**
 * @file src/cuda_accel/cuda_steensgaard.cu
 * @brief CUDA Steensgaard points-to analysis — port of steensgaard.cl.
 */
#include "retdec/cuda_accel/cuda_steensgaard.h"
#include "retdec/cuda_accel/cuda_context.h"
#include "retdec/cuda_accel/cuda_profiler.h"

#include <chrono>
#include <numeric>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

static constexpr unsigned int NO_TARGET = 0xFFFFFFFFu;

// ═══════════════════════════════════════════════════════════════════════════
// CUDA device helpers
// ═══════════════════════════════════════════════════════════════════════════
#ifdef RETDEC_HAS_CUDA

__device__ static unsigned int ecr_find(unsigned int* parent, unsigned int x) {
    unsigned int root = x;
    for (unsigned d = 0; d < 64u; ++d) {
        unsigned int p = parent[root];
        if (p == root) break;
        root = p;
    }
    unsigned int cur = x;
    for (unsigned d = 0; d < 64u; ++d) {
        unsigned int p = parent[cur];
        if (p == cur) break;
        parent[cur] = root;
        cur = p;
    }
    return root;
}

__device__ static bool ecr_union(unsigned int* parent, unsigned int* rnk,
                                  unsigned int ra, unsigned int rb) {
    if (ra == rb) return false;
    unsigned int winner, loser;
    if (rnk[ra] > rnk[rb]) { winner=ra; loser=rb; }
    else if (rnk[rb] > rnk[ra]) { winner=rb; loser=ra; }
    else { winner=ra; loser=rb; rnk[ra]+=1u; }
    parent[loser] = winner;
    return true;
}

// Wave 1: COPY
__global__ void retdec_steensgaard_copy(
    unsigned int*       parent,
    unsigned int*       rnk,
    unsigned int*       pts_to,
    const unsigned char* con_type,
    const unsigned int* con_a,
    const unsigned int* con_b,
    unsigned int        num_con,
    unsigned int*       pending_a,
    unsigned int*       pending_b,
    unsigned int*       pending_count,
    unsigned int        max_pending,
    unsigned int*       changed_flag)
{
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= num_con || con_type[gid] != 0) return;

    unsigned int a = con_a[gid], b = con_b[gid];
    unsigned int ra = ecr_find(parent, a), rb = ecr_find(parent, b);
    if (ra == rb) return;

    unsigned int pa = pts_to[ra], pb = pts_to[rb];
    bool changed = ecr_union(parent, rnk, ra, rb);

    if (pa != NO_TARGET && pb != NO_TARGET) {
        unsigned int idx = atomicAdd(pending_count, 1u);
        if (idx < max_pending) { pending_a[idx]=pa; pending_b[idx]=pb; }
    } else {
        unsigned int winner = ecr_find(parent, a);
        if (pts_to[winner] == NO_TARGET) {
            if (pa != NO_TARGET) pts_to[winner] = pa;
            else if (pb != NO_TARGET) pts_to[winner] = pb;
        }
    }
    if (changed) atomicOr(changed_flag, 1u);
}

// Wave 2: ADDR_OF
__global__ void retdec_steensgaard_addr(
    unsigned int*       parent,
    unsigned int*       rnk,
    unsigned int*       pts_to,
    const unsigned char* con_type,
    const unsigned int* con_a,
    const unsigned int* con_b,
    unsigned int        num_con,
    unsigned int*       changed_flag)
{
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= num_con || con_type[gid] != 1) return;

    unsigned int a=con_a[gid], b=con_b[gid];
    unsigned int ra=ecr_find(parent,a), rb=ecr_find(parent,b);
    unsigned int pra=pts_to[ra];

    if (pra == NO_TARGET) {
        pts_to[ra] = rb;
        atomicOr(changed_flag, 1u);
    } else {
        unsigned int rpra = ecr_find(parent, pra);
        if (ecr_union(parent, rnk, rpra, rb)) atomicOr(changed_flag, 1u);
    }
}

// Wave 3: DEREF (STORE + LOAD)
__global__ void retdec_steensgaard_deref(
    unsigned int*       parent,
    unsigned int*       rnk,
    unsigned int*       pts_to,
    const unsigned char* con_type,
    const unsigned int* con_a,
    const unsigned int* con_b,
    unsigned int        num_con,
    unsigned int*       pending_a,
    unsigned int*       pending_b,
    unsigned int*       pending_count,
    unsigned int        max_pending,
    unsigned int*       changed_flag)
{
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= num_con) return;
    unsigned char ct = con_type[gid];
    if (ct != 2 && ct != 3) return;

    unsigned int a=con_a[gid], b=con_b[gid];
    unsigned int ra=ecr_find(parent,a), rb=ecr_find(parent,b);

    if (ct == 2) { // STORE: *a := b
        unsigned int pra = pts_to[ra];
        if (pra != NO_TARGET) {
            unsigned int rpra = ecr_find(parent, pra);
            unsigned int rra2 = ecr_find(parent, rpra), rrb = ecr_find(parent, rb);
            if (rra2 != rrb) {
                unsigned int pa=pts_to[rra2], pb=pts_to[rrb];
                bool ch = ecr_union(parent, rnk, rra2, rrb);
                if (pa != NO_TARGET && pb != NO_TARGET) {
                    unsigned int idx = atomicAdd(pending_count, 1u);
                    if (idx < max_pending){pending_a[idx]=pa;pending_b[idx]=pb;}
                } else {
                    unsigned int w = ecr_find(parent, rra2);
                    if (pts_to[w] == NO_TARGET){if(pa!=NO_TARGET)pts_to[w]=pa;else if(pb!=NO_TARGET)pts_to[w]=pb;}
                }
                if (ch) atomicOr(changed_flag, 1u);
            }
        }
    } else { // LOAD: a := *b
        unsigned int prb = pts_to[rb];
        if (prb != NO_TARGET) {
            unsigned int rprb = ecr_find(parent, prb);
            unsigned int rra = ecr_find(parent, ra), rrb2 = ecr_find(parent, rprb);
            if (rra != rrb2) {
                unsigned int pa=pts_to[rra], pb=pts_to[rrb2];
                bool ch = ecr_union(parent, rnk, rra, rrb2);
                if (pa != NO_TARGET && pb != NO_TARGET) {
                    unsigned int idx = atomicAdd(pending_count, 1u);
                    if (idx < max_pending){pending_a[idx]=pa;pending_b[idx]=pb;}
                } else {
                    unsigned int w = ecr_find(parent, rra);
                    if (pts_to[w] == NO_TARGET){if(pa!=NO_TARGET)pts_to[w]=pa;else if(pb!=NO_TARGET)pts_to[w]=pb;}
                }
                if (ch) atomicOr(changed_flag, 1u);
            }
        }
    }
}

// Flush pending
__global__ void retdec_steensgaard_flush(
    unsigned int* parent,
    unsigned int* rnk,
    unsigned int* pts_to,
    unsigned int* pending_a,
    unsigned int* pending_b,
    unsigned int  num_pending,
    unsigned int* next_a,
    unsigned int* next_b,
    unsigned int* next_count,
    unsigned int  max_next,
    unsigned int* changed_flag)
{
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= num_pending) return;

    unsigned int a=pending_a[gid], b=pending_b[gid];
    unsigned int ra=ecr_find(parent,a), rb=ecr_find(parent,b);
    if (ra==rb) return;

    unsigned int pa=pts_to[ra], pb=pts_to[rb];
    bool ch = ecr_union(parent, rnk, ra, rb);
    if (pa != NO_TARGET && pb != NO_TARGET) {
        unsigned int idx = atomicAdd(next_count, 1u);
        if (idx < max_next){next_a[idx]=pa;next_b[idx]=pb;}
    } else {
        unsigned int w = ecr_find(parent, ra);
        if (pts_to[w]==NO_TARGET){if(pa!=NO_TARGET)pts_to[w]=pa;else if(pb!=NO_TARGET)pts_to[w]=pb;}
    }
    if (ch) atomicOr(changed_flag, 1u);
}

#endif // RETDEC_HAS_CUDA

// ═══════════════════════════════════════════════════════════════════════════
// CPU fallback
// ═══════════════════════════════════════════════════════════════════════════

CUDASteensgaard::CUDASteensgaard(CUDAContext* ctx) : ctx_(ctx) {
#ifdef RETDEC_HAS_CUDA
    if (ctx_ && ctx_->isReady()) gpuReady_ = true;
#endif
}

CUDASteensgaard::~CUDASteensgaard() = default;

AliasResult CUDASteensgaard::analyze(std::uint32_t numVars,
                                      const std::vector<PtsConstraint>& constraints)
{
    if (numVars == 0) return {};
    auto t0 = std::chrono::steady_clock::now();

#ifdef RETDEC_HAS_CUDA
    if (gpuReady_ && ctx_) {
        std::size_t C = constraints.size();
        std::size_t maxPending = std::max(C, std::size_t(64));

        // Build flat arrays
        std::vector<unsigned char> conType(C);
        std::vector<unsigned int>  conA(C), conB(C);
        for (std::size_t i=0;i<C;++i){
            conType[i]=(unsigned char)constraints[i].kind;
            conA[i]=constraints[i].varA;
            conB[i]=constraints[i].varB;
        }
        std::vector<unsigned int> parent(numVars), rnk(numVars,0), ptsTo(numVars, NO_TARGET);
        std::iota(parent.begin(), parent.end(), 0u);

        cudaStream_t stream = ctx_->stream();

        void *dParent=nullptr,*dRnk=nullptr,*dPtsTo=nullptr;
        void *dConType=nullptr,*dConA=nullptr,*dConB=nullptr;
        void *dPendA=nullptr,*dPendB=nullptr,*dPendCount=nullptr;
        void *dNextA=nullptr,*dNextB=nullptr,*dNextCount=nullptr;
        void *dChanged=nullptr;

        auto cleanup = [&](){
            for (void* p : {dParent,dRnk,dPtsTo,dConType,dConA,dConB,
                            dPendA,dPendB,dPendCount,dNextA,dNextB,dNextCount,dChanged})
                if(p) cudaFree(p);
        };

        bool ok = (cudaMalloc(&dParent,   numVars*4) == cudaSuccess
                && cudaMalloc(&dRnk,      numVars*4) == cudaSuccess
                && cudaMalloc(&dPtsTo,    numVars*4) == cudaSuccess
                && (C==0||cudaMalloc(&dConType, C)    == cudaSuccess)
                && (C==0||cudaMalloc(&dConA,    C*4)  == cudaSuccess)
                && (C==0||cudaMalloc(&dConB,    C*4)  == cudaSuccess)
                && cudaMalloc(&dPendA,    maxPending*4)==cudaSuccess
                && cudaMalloc(&dPendB,    maxPending*4)==cudaSuccess
                && cudaMalloc(&dPendCount,4)==cudaSuccess
                && cudaMalloc(&dNextA,    maxPending*4)==cudaSuccess
                && cudaMalloc(&dNextB,    maxPending*4)==cudaSuccess
                && cudaMalloc(&dNextCount,4)==cudaSuccess
                && cudaMalloc(&dChanged,  4)==cudaSuccess);

        if (!ok) { cleanup(); gpuReady_=false; lastError_="cudaMalloc failed"; goto cpu_fallback; }

        cudaMemcpyAsync(dParent, parent.data(),  numVars*4, cudaMemcpyHostToDevice, stream);
        cudaMemcpyAsync(dRnk,    rnk.data(),     numVars*4, cudaMemcpyHostToDevice, stream);
        cudaMemcpyAsync(dPtsTo,  ptsTo.data(),   numVars*4, cudaMemcpyHostToDevice, stream);
        if (C>0){
            cudaMemcpyAsync(dConType,conType.data(),C,  cudaMemcpyHostToDevice,stream);
            cudaMemcpyAsync(dConA,   conA.data(),   C*4,cudaMemcpyHostToDevice,stream);
            cudaMemcpyAsync(dConB,   conB.data(),   C*4,cudaMemcpyHostToDevice,stream);
        }

        unsigned int block = 256;
        unsigned int gridC = C ? (unsigned int)((C+block-1)/block) : 1;

        for (lastIter_ = 0; lastIter_ < kMaxIterations; ++lastIter_) {
            unsigned int zero = 0;
            cudaMemcpyAsync(dChanged,    &zero, 4, cudaMemcpyHostToDevice, stream);
            cudaMemcpyAsync(dPendCount,  &zero, 4, cudaMemcpyHostToDevice, stream);
            cudaMemsetAsync(dPendA, 0, maxPending*4, stream);

            if (C > 0) {
                retdec_steensgaard_copy<<<gridC, block, 0, stream>>>(
                    (unsigned int*)dParent,(unsigned int*)dRnk,(unsigned int*)dPtsTo,
                    (const unsigned char*)dConType,(const unsigned int*)dConA,(const unsigned int*)dConB,
                    (unsigned int)C,(unsigned int*)dPendA,(unsigned int*)dPendB,
                    (unsigned int*)dPendCount,(unsigned int)maxPending,(unsigned int*)dChanged);
                retdec_steensgaard_addr<<<gridC, block, 0, stream>>>(
                    (unsigned int*)dParent,(unsigned int*)dRnk,(unsigned int*)dPtsTo,
                    (const unsigned char*)dConType,(const unsigned int*)dConA,(const unsigned int*)dConB,
                    (unsigned int)C,(unsigned int*)dChanged);
                retdec_steensgaard_deref<<<gridC, block, 0, stream>>>(
                    (unsigned int*)dParent,(unsigned int*)dRnk,(unsigned int*)dPtsTo,
                    (const unsigned char*)dConType,(const unsigned int*)dConA,(const unsigned int*)dConB,
                    (unsigned int)C,(unsigned int*)dPendA,(unsigned int*)dPendB,
                    (unsigned int*)dPendCount,(unsigned int)maxPending,(unsigned int*)dChanged);
            }

            // Flush pending
            unsigned int pendCount = 0;
            cudaMemcpyAsync(&pendCount, dPendCount, 4, cudaMemcpyDeviceToHost, stream);
            cudaStreamSynchronize(stream);
            if (pendCount > 0 && pendCount <= maxPending) {
                zero = 0;
                cudaMemcpyAsync(dNextCount, &zero, 4, cudaMemcpyHostToDevice, stream);
                unsigned int gridP = (pendCount+block-1)/block;
                retdec_steensgaard_flush<<<gridP, block, 0, stream>>>(
                    (unsigned int*)dParent,(unsigned int*)dRnk,(unsigned int*)dPtsTo,
                    (unsigned int*)dPendA,(unsigned int*)dPendB,pendCount,
                    (unsigned int*)dNextA,(unsigned int*)dNextB,(unsigned int*)dNextCount,
                    (unsigned int)maxPending,(unsigned int*)dChanged);
                cudaStreamSynchronize(stream);
            }

            unsigned int changed = 0;
            cudaMemcpyAsync(&changed, dChanged, 4, cudaMemcpyDeviceToHost, stream);
            cudaStreamSynchronize(stream);
            if (changed == 0) break;
        }

        cudaMemcpyAsync(parent.data(), dParent, numVars*4, cudaMemcpyDeviceToHost, stream);
        cudaMemcpyAsync(ptsTo.data(),  dPtsTo,  numVars*4, cudaMemcpyDeviceToHost, stream);
        cudaStreamSynchronize(stream);
        cleanup();

        // Build result using path-compressed parent as alias class
        AliasResult result;
        result.aliasClass.resize(numVars);
        result.pointsTo.resize(numVars);
        // Simple CPU path compression to find roots
        std::function<unsigned int(unsigned int)> find = [&](unsigned int x) -> unsigned int {
            if (parent[x] == x) return x;
            parent[x] = find(parent[x]);
            return parent[x];
        };
        for (unsigned int i=0;i<numVars;++i) {
            result.aliasClass[i] = find(i);
            result.pointsTo[i]   = (ptsTo[i] != NO_TARGET) ? find(ptsTo[i]) : AliasResult::kNoTarget;
        }

        auto t1 = std::chrono::steady_clock::now();
        CUDAProfiler::instance().record("steensgaard",
            (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());
        return result;
    }
cpu_fallback:
#endif

    return analyzeCPU(numVars, constraints);
}

AliasResult CUDASteensgaard::analyzeCPU(std::uint32_t numVars,
                                          const std::vector<PtsConstraint>& cons)
{
    std::vector<unsigned int> parent(numVars), rnk(numVars, 0), ptsTo(numVars, NO_TARGET);
    std::iota(parent.begin(), parent.end(), 0u);

    std::function<unsigned int(unsigned int)> find = [&](unsigned int x) -> unsigned int {
        if (parent[x] == x) return x;
        parent[x] = find(parent[x]);
        return parent[x];
    };
    auto unite = [&](unsigned int a, unsigned int b) -> bool {
        a = find(a); b = find(b);
        if (a == b) return false;
        if (rnk[a] < rnk[b]) std::swap(a, b);
        parent[b] = a;
        if (rnk[a] == rnk[b]) ++rnk[a];
        return true;
    };

    for (lastIter_ = 0; lastIter_ < kMaxIterations; ++lastIter_) {
        bool changed = false;
        std::vector<std::pair<unsigned int,unsigned int>> pending;

        for (auto& c : cons) {
            unsigned int a=c.varA, b=c.varB;
            if (a>=numVars||b>=numVars) continue;
            switch (c.kind) {
                case ConstraintKind::Copy: {
                    unsigned int ra=find(a), rb=find(b);
                    if (ra==rb) break;
                    unsigned int pa=ptsTo[ra], pb=ptsTo[rb];
                    changed |= unite(a, b);
                    if (pa!=NO_TARGET && pb!=NO_TARGET) pending.emplace_back(pa, pb);
                    else { unsigned int w=find(a); if(ptsTo[w]==NO_TARGET){if(pa!=NO_TARGET)ptsTo[w]=pa;else if(pb!=NO_TARGET)ptsTo[w]=pb;} }
                    break;
                }
                case ConstraintKind::AddrOf: {
                    unsigned int ra=find(a);
                    if (ptsTo[ra]==NO_TARGET){ptsTo[ra]=find(b);changed=true;}
                    else changed |= unite(ptsTo[ra], b);
                    break;
                }
                case ConstraintKind::Store: {
                    unsigned int ra=find(a); if(ptsTo[ra]==NO_TARGET) break;
                    unsigned int rpra=find(ptsTo[ra]);
                    unsigned int prpra=ptsTo[rpra], prb2=ptsTo[find(b)];
                    if (unite(rpra, b)) {
                        changed=true;
                        if(prpra!=NO_TARGET&&prb2!=NO_TARGET) pending.emplace_back(prpra,prb2);
                        else{unsigned int w=find(rpra);if(ptsTo[w]==NO_TARGET){if(prpra!=NO_TARGET)ptsTo[w]=prpra;else if(prb2!=NO_TARGET)ptsTo[w]=prb2;}}
                    }
                    break;
                }
                case ConstraintKind::Load: {
                    unsigned int rb=find(b); if(ptsTo[rb]==NO_TARGET) break;
                    unsigned int rprb=find(ptsTo[rb]);
                    unsigned int pra2=ptsTo[find(a)], prprb=ptsTo[rprb];
                    if (unite(a, rprb)) {
                        changed=true;
                        if(pra2!=NO_TARGET&&prprb!=NO_TARGET) pending.emplace_back(pra2,prprb);
                        else{unsigned int w=find(a);if(ptsTo[w]==NO_TARGET){if(pra2!=NO_TARGET)ptsTo[w]=pra2;else if(prprb!=NO_TARGET)ptsTo[w]=prprb;}}
                    }
                    break;
                }
            }
        }
        for (auto& [pa, pb] : pending) if (unite(pa, pb)) changed = true;
        if (!changed) break;
    }

    AliasResult result;
    result.aliasClass.resize(numVars);
    result.pointsTo.resize(numVars);
    for (unsigned int i=0;i<numVars;++i) {
        result.aliasClass[i] = find(i);
        result.pointsTo[i]   = (ptsTo[i]!=NO_TARGET) ? find(ptsTo[i]) : AliasResult::kNoTarget;
    }
    return result;
}

} // namespace retdec::cuda_accel
