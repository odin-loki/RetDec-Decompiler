/**
 * @file src/cuda_accel/cuda_egraph_simplifier.cu
 * @brief CUDA e-graph equality saturation — port of egraph_simplify.cl.
 */
#include "retdec/cuda_accel/cuda_egraph_simplifier.h"
#include "retdec/cuda_accel/cuda_context.h"
#include "retdec/cuda_accel/cuda_profiler.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

// ═══════════════════════════════════════════════════════════════════════════
// EGraph host implementation
// ═══════════════════════════════════════════════════════════════════════════

std::uint32_t EGraph::addNode(EOpcode op, std::uint32_t lhs,
                               std::uint32_t rhs, std::uint32_t aux)
{
    std::uint32_t id = (std::uint32_t)nodes_.size();
    ENode n; n.opcode=op; n.lhs=lhs; n.rhs=rhs; n.aux=aux; n.litVal=0;
    n.eclass = id < classCount_ ? id : (std::uint32_t)classCount_++;
    nodes_.push_back(n);
    op_.push_back((std::uint32_t)op);
    eclass_.push_back(n.eclass);
    lhs_.push_back(lhs);
    rhs_.push_back(rhs);
    aux_.push_back(aux);
    lit_.push_back(0u);
    ufParent_.push_back(n.eclass);
    ufRank_.push_back(0u);
    return id;
}

std::uint32_t EGraph::addLiteral(std::uint64_t val) {
    auto id = addNode(EOpcode::LIT);
    lit_[id] = val;
    nodes_[id].litVal = val;
    return id;
}

std::uint32_t EGraph::addVar(std::uint32_t varId) {
    auto id = addNode(EOpcode::VAR);
    aux_[id] = varId;
    nodes_[id].aux = varId;
    return id;
}

void EGraph::initUnionFind() {
    std::iota(ufParent_.begin(), ufParent_.end(), 0u);
    std::fill(ufRank_.begin(), ufRank_.end(), 0u);
}

std::uint32_t EGraph::find(std::uint32_t x) {
    while (ufParent_[x] != x) {
        ufParent_[x] = ufParent_[ufParent_[x]];
        x = ufParent_[x];
    }
    return x;
}

// ═══════════════════════════════════════════════════════════════════════════
// CUDA kernels
// ═══════════════════════════════════════════════════════════════════════════
#ifdef RETDEC_HAS_CUDA

#define CUDA_NO_CLASS 0xFFFFFFFFu
#define CUDA_MAX_SCORE 99u

__device__ static unsigned int cdist_d(unsigned int op) {
    switch (op) {
        case 1: return 0u; // LIT
        case 2: return 0u; // VAR
        case 15: return 1u; // ARRAY
        case 16: return 1u; // FIELD
        case 17: return 2u; // BITFIELD
        case 3:  return 3u; // ADD
        case 4:  return 3u; // SUB
        case 14: return 3u; // DEREF
        case 5:  return 4u; // MUL
        case 6:  return 4u; // DIV
        case 13: return 5u; // CAST
        case 7: case 8: case 9: case 10: case 11: case 12: return 6u;
        default: return CUDA_MAX_SCORE;
    }
}

__device__ static unsigned int uf_find_d(unsigned int* parent, unsigned int x) {
    while (parent[x] != x) {
        unsigned int gp = parent[parent[x]];
        parent[x] = gp;
        x = gp;
    }
    return x;
}

__device__ static bool uf_union_d(unsigned int* parent, unsigned int* rank,
                                   unsigned int ra, unsigned int rb) {
    ra = uf_find_d(parent, ra);
    rb = uf_find_d(parent, rb);
    if (ra == rb) return false;
    if (rank[ra] < rank[rb]) { unsigned int t=ra;ra=rb;rb=t; }
    parent[rb] = ra;
    if (rank[ra] == rank[rb]) rank[ra]++;
    return true;
}

__device__ static bool class_literal_d(
    const unsigned int* op, const unsigned int* eclass, const unsigned long long* lit,
    unsigned int* parent, unsigned int n_nodes, unsigned int cls,
    unsigned long long* val)
{
    unsigned int root = uf_find_d(parent, cls);
    for (unsigned int i = 0; i < n_nodes; i++) {
        if (op[i] == 1 && uf_find_d(parent, eclass[i]) == root) {
            *val = lit[i]; return true;
        }
    }
    return false;
}

__global__ void retdec_egraph_saturate(
    unsigned int* op,
    unsigned int* eclass,
    unsigned int* lhs,
    unsigned int* rhs,
    unsigned int* aux,
    unsigned long long* lit,
    unsigned int* uf_parent,
    unsigned int* uf_rank,
    unsigned int* dirty,
    unsigned int  n_nodes,
    unsigned int  n_classes)
{
    unsigned int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= n_nodes) return;

    unsigned int myop  = op[id];
    unsigned int mycls = uf_find_d(uf_parent, eclass[id]);
    unsigned int l = (lhs[id]!=CUDA_NO_CLASS)?uf_find_d(uf_parent,lhs[id]):CUDA_NO_CLASS;
    unsigned int r = (rhs[id]!=CUDA_NO_CLASS)?uf_find_d(uf_parent,rhs[id]):CUDA_NO_CLASS;
    unsigned int myaux = aux[id];
    bool fired = false;

    // Rule 1: x+0→x, x-0→x, x|0→x, x^0→x
    if ((myop==3||myop==4||myop==8||myop==9) && l!=CUDA_NO_CLASS && r!=CUDA_NO_CLASS) {
        unsigned long long rv=0;
        if (class_literal_d(op,eclass,lit,uf_parent,n_nodes,r,&rv) && rv==0UL)
            fired |= uf_union_d(uf_parent, uf_rank, mycls, l);
    }
    // Rule: x&~0→x
    if (myop==7 && l!=CUDA_NO_CLASS && r!=CUDA_NO_CLASS) {
        unsigned long long rv=0;
        if (class_literal_d(op,eclass,lit,uf_parent,n_nodes,r,&rv) && rv==0xFFFFFFFFFFFFFFFFULL)
            fired |= uf_union_d(uf_parent, uf_rank, mycls, l);
    }
    // Rule 2: x*1→x, 1*x→x
    if (myop==5 && l!=CUDA_NO_CLASS && r!=CUDA_NO_CLASS) {
        unsigned long long rv=0, lv=0;
        if (class_literal_d(op,eclass,lit,uf_parent,n_nodes,r,&rv) && rv==1UL)
            fired |= uf_union_d(uf_parent, uf_rank, mycls, l);
        if (class_literal_d(op,eclass,lit,uf_parent,n_nodes,l,&lv) && lv==1UL)
            fired |= uf_union_d(uf_parent, uf_rank, mycls, r);
    }
    // Rule 4: x>>0→x, x<<0→x
    if ((myop==10||myop==11||myop==12) && l!=CUDA_NO_CLASS && r!=CUDA_NO_CLASS) {
        unsigned long long rv=0;
        if (class_literal_d(op,eclass,lit,uf_parent,n_nodes,r,&rv) && rv==0UL)
            fired |= uf_union_d(uf_parent, uf_rank, mycls, l);
    }
    // Rule 7: DEREF(ADD(base, idx)) → ARRAY or FIELD
    if (myop==14 && l!=CUDA_NO_CLASS) {
        for (unsigned int ai=0; ai<n_nodes; ai++) {
            if (op[ai]==3 && uf_find_d(uf_parent,eclass[ai])==l &&
                lhs[ai]!=CUDA_NO_CLASS && rhs[ai]!=CUDA_NO_CLASS)
            {
                unsigned long long fld=0;
                bool is_lit = class_literal_d(op,eclass,lit,uf_parent,n_nodes,
                                               uf_find_d(uf_parent,rhs[ai]),&fld);
                if (is_lit) {
                    op[id]=16; lhs[id]=uf_find_d(uf_parent,lhs[ai]); rhs[id]=CUDA_NO_CLASS; aux[id]=(unsigned int)fld;
                } else {
                    op[id]=15; lhs[id]=uf_find_d(uf_parent,lhs[ai]); rhs[id]=uf_find_d(uf_parent,rhs[ai]);
                }
                fired=true; break;
            }
        }
    }
    // Rule 6: CAST(CAST(x,w1),w2) → CAST(x,w2) when w2<=w1
    if (myop==13 && l!=CUDA_NO_CLASS) {
        for (unsigned int ci=0; ci<n_nodes; ci++) {
            if (op[ci]==13 && uf_find_d(uf_parent,eclass[ci])==l && lhs[ci]!=CUDA_NO_CLASS) {
                if (myaux <= aux[ci]) { lhs[id]=lhs[ci]; fired=true; }
                break;
            }
        }
    }

    if (fired) atomicOr(dirty, 1u);
}

__global__ void retdec_egraph_extract(
    const unsigned int* op,
    const unsigned int* eclass,
    unsigned int*       uf_parent,
    unsigned int*       best_node,
    unsigned int*       best_score,
    unsigned int        n_nodes,
    unsigned int        n_classes)
{
    unsigned int cls = blockIdx.x * blockDim.x + threadIdx.x;
    if (cls >= n_classes) return;

    unsigned int root = uf_find_d(uf_parent, cls);
    unsigned int bnode = CUDA_NO_CLASS, bsc = CUDA_MAX_SCORE + 1u;

    for (unsigned int i = 0; i < n_nodes; i++) {
        if (uf_find_d(uf_parent, eclass[i]) == root) {
            unsigned int sc = cdist_d(op[i]);
            if (sc < bsc) { bsc=sc; bnode=i; }
        }
    }
    best_node[cls]  = bnode;
    best_score[cls] = bsc;
}

#endif // RETDEC_HAS_CUDA

// ═══════════════════════════════════════════════════════════════════════════
// Host class
// ═══════════════════════════════════════════════════════════════════════════

CUDAEGraphSimplifier::CUDAEGraphSimplifier(CUDAContext* ctx) : ctx_(ctx) {
#ifdef RETDEC_HAS_CUDA
    if (ctx_ && ctx_->isReady()) gpuReady_ = true;
#endif
}

CUDAEGraphSimplifier::~CUDAEGraphSimplifier() = default;

std::vector<EClassResult> CUDAEGraphSimplifier::simplify(EGraph& graph,
                                                           std::uint32_t maxIter)
{
    if (graph.nodeCount() == 0) return {};
    auto t0 = std::chrono::steady_clock::now();

#ifdef RETDEC_HAS_CUDA
    if (gpuReady_ && ctx_) {
        graph.initUnionFind();
        unsigned int N = (unsigned int)graph.nodeCount();
        unsigned int C = (unsigned int)graph.classCount();

        cudaStream_t stream = ctx_->stream();

        void *dOp=nullptr,*dEc=nullptr,*dLhs=nullptr,*dRhs=nullptr,*dAux=nullptr,*dLit=nullptr;
        void *dPar=nullptr,*dRnk=nullptr,*dDirty=nullptr;
        void *dBestNode=nullptr,*dBestScore=nullptr;

        auto cleanup=[&](){
            for(void* p:{dOp,dEc,dLhs,dRhs,dAux,dLit,dPar,dRnk,dDirty,dBestNode,dBestScore})
                if(p)cudaFree(p);
        };

        bool ok=(
            cudaMalloc(&dOp,    N*4)==cudaSuccess &&
            cudaMalloc(&dEc,    N*4)==cudaSuccess &&
            cudaMalloc(&dLhs,   N*4)==cudaSuccess &&
            cudaMalloc(&dRhs,   N*4)==cudaSuccess &&
            cudaMalloc(&dAux,   N*4)==cudaSuccess &&
            cudaMalloc(&dLit,   N*8)==cudaSuccess &&
            cudaMalloc(&dPar,   N*4)==cudaSuccess &&
            cudaMalloc(&dRnk,   N*4)==cudaSuccess &&
            cudaMalloc(&dDirty, 4)  ==cudaSuccess &&
            cudaMalloc(&dBestNode,  C*4)==cudaSuccess &&
            cudaMalloc(&dBestScore, C*4)==cudaSuccess);

        if (!ok){cleanup();gpuReady_=false;goto cpu_fallback;}

        cudaMemcpyAsync(dOp,  graph.op().data(),       N*4,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dEc,  graph.eclass().data(),    N*4,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dLhs, graph.lhs().data(),       N*4,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dRhs, graph.rhs().data(),       N*4,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dAux, graph.aux().data(),       N*4,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dLit, graph.lit().data(),       N*8,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dPar, graph.ufParent().data(),  N*4,cudaMemcpyHostToDevice,stream);
        cudaMemcpyAsync(dRnk, graph.ufRank().data(),    N*4,cudaMemcpyHostToDevice,stream);

        {
            unsigned int block=256;
            unsigned int gridN=(N+block-1)/block;
            unsigned int gridC=(C+block-1)/block;

            for (std::uint32_t iter=0; iter<maxIter; ++iter) {
                unsigned int zero=0;
                cudaMemcpyAsync(dDirty,&zero,4,cudaMemcpyHostToDevice,stream);
                retdec_egraph_saturate<<<gridN,block,0,stream>>>(
                    (unsigned int*)dOp,(unsigned int*)dEc,(unsigned int*)dLhs,
                    (unsigned int*)dRhs,(unsigned int*)dAux,(unsigned long long*)dLit,
                    (unsigned int*)dPar,(unsigned int*)dRnk,(unsigned int*)dDirty,N,C);
                unsigned int dirty=0;
                cudaMemcpyAsync(&dirty,dDirty,4,cudaMemcpyDeviceToHost,stream);
                cudaStreamSynchronize(stream);
                if(!dirty) break;
            }

            retdec_egraph_extract<<<gridC,block,0,stream>>>(
                (const unsigned int*)dOp,(const unsigned int*)dEc,
                (unsigned int*)dPar,(unsigned int*)dBestNode,(unsigned int*)dBestScore,N,C);
        }

        std::vector<unsigned int> bestNode(C), bestScore(C);
        std::vector<unsigned int> dOpHost(N);
        cudaMemcpyAsync(bestNode.data(),  dBestNode,  C*4,cudaMemcpyDeviceToHost,stream);
        cudaMemcpyAsync(bestScore.data(), dBestScore, C*4,cudaMemcpyDeviceToHost,stream);
        cudaMemcpyAsync(dOpHost.data(),   dOp,        N*4,cudaMemcpyDeviceToHost,stream);
        cudaStreamSynchronize(stream);
        cleanup();

        std::vector<EClassResult> result(C);
        for (unsigned int c=0;c<C;++c){
            result[c].classId  = c;
            result[c].bestNode = bestNode[c];
            result[c].score    = bestScore[c];
            result[c].opcode   = (bestNode[c]<N)
                                 ? static_cast<EOpcode>(dOpHost[bestNode[c]])
                                 : EOpcode::NOP;
        }

        auto t1=std::chrono::steady_clock::now();
        CUDAProfiler::instance().record("egraph_saturate",
            (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());
        return result;
    }
cpu_fallback:
#endif

    return simplifyCPU(graph, maxIter);
}

std::vector<EClassResult> CUDAEGraphSimplifier::simplifyCPU(EGraph& graph,
                                                              std::uint32_t maxIter)
{
    graph.initUnionFind();
    std::size_t N = graph.nodeCount();
    std::size_t C = graph.classCount();

    auto& op_     = graph.op();
    auto& eclass_ = graph.eclass();
    auto& lhs_    = graph.lhs();
    auto& rhs_    = graph.rhs();
    auto& aux_    = graph.aux();
    auto& lit_    = graph.lit();
    auto& par     = graph.ufParent();
    auto& rnk     = graph.ufRank();

    std::function<unsigned int(unsigned int)> ufFind = [&](unsigned int x) -> unsigned int {
        while (par[x]!=x){par[x]=par[par[x]];x=par[x];}return x;
    };
    auto ufUnion = [&](unsigned int ra, unsigned int rb) -> bool {
        ra=ufFind(ra);rb=ufFind(rb);
        if(ra==rb)return false;
        if(rnk[ra]<rnk[rb])std::swap(ra,rb);
        par[rb]=ra;
        if(rnk[ra]==rnk[rb])rnk[ra]++;
        return true;
    };
    auto classLit = [&](unsigned int cls, std::uint64_t& val) -> bool {
        unsigned int root=ufFind(cls);
        for(std::size_t i=0;i<N;i++)
            if(op_[i]==1&&ufFind(eclass_[i])==root){val=lit_[i];return true;}
        return false;
    };

    auto cdist=[](unsigned int op)->unsigned int{
        switch(op){case 1:case 2:return 0;case 15:case 16:return 1;case 17:return 2;
        case 3:case 4:case 14:return 3;case 5:case 6:return 4;case 13:return 5;
        case 7:case 8:case 9:case 10:case 11:case 12:return 6;default:return 99;}
    };

    for (std::uint32_t iter=0; iter<maxIter; ++iter) {
        bool dirty=false;
        for (std::size_t id=0;id<N;++id) {
            unsigned int myop=op_[id];
            unsigned int mycls=ufFind(eclass_[id]);
            unsigned int l=(lhs_[id]!=kNoClass)?ufFind(lhs_[id]):kNoClass;
            unsigned int r=(rhs_[id]!=kNoClass)?ufFind(rhs_[id]):kNoClass;

            std::uint64_t rv=0,lv=0;
            if((myop==3||myop==4||myop==8||myop==9)&&l!=kNoClass&&r!=kNoClass)
                if(classLit(r,rv)&&rv==0)dirty|=ufUnion(mycls,l);
            if(myop==7&&l!=kNoClass&&r!=kNoClass)
                if(classLit(r,rv)&&rv==0xFFFFFFFFFFFFFFFFULL)dirty|=ufUnion(mycls,l);
            if(myop==5&&l!=kNoClass&&r!=kNoClass){
                if(classLit(r,rv)&&rv==1)dirty|=ufUnion(mycls,l);
                if(classLit(l,lv)&&lv==1)dirty|=ufUnion(mycls,r);
            }
            if((myop==10||myop==11||myop==12)&&l!=kNoClass&&r!=kNoClass)
                if(classLit(r,rv)&&rv==0)dirty|=ufUnion(mycls,l);
            if(myop==14&&l!=kNoClass){
                for(std::size_t ai=0;ai<N;ai++){
                    if(op_[ai]==3&&ufFind(eclass_[ai])==l&&lhs_[ai]!=kNoClass&&rhs_[ai]!=kNoClass){
                        std::uint64_t fld=0;
                        bool is_lit=classLit(ufFind(rhs_[ai]),fld);
                        if(is_lit){op_[id]=16;lhs_[id]=ufFind(lhs_[ai]);rhs_[id]=kNoClass;aux_[id]=(unsigned int)fld;}
                        else{op_[id]=15;lhs_[id]=ufFind(lhs_[ai]);rhs_[id]=ufFind(rhs_[ai]);}
                        dirty=true;break;
                    }
                }
            }
            if(myop==13&&l!=kNoClass){
                for(std::size_t ci=0;ci<N;ci++){
                    if(op_[ci]==13&&ufFind(eclass_[ci])==l&&lhs_[ci]!=kNoClass){
                        if(aux_[id]<=aux_[ci]){lhs_[id]=lhs_[ci];dirty=true;}
                        break;
                    }
                }
            }
        }
        if(!dirty)break;
    }

    std::vector<EClassResult> result(C);
    for(unsigned int c=0;c<(unsigned int)C;++c){
        result[c].classId=c;
        unsigned int root=ufFind(c);
        unsigned int bnode=kNoClass, bsc=100u;
        for(std::size_t i=0;i<N;i++){
            if(ufFind(eclass_[i])==root){
                unsigned int sc=cdist(op_[i]);
                if(sc<bsc){bsc=sc;bnode=(unsigned int)i;}
            }
        }
        result[c].bestNode=bnode;
        result[c].score=bsc;
        result[c].opcode=(bnode<N)?static_cast<EOpcode>(op_[bnode]):EOpcode::NOP;
    }
    return result;
}

} // namespace retdec::cuda_accel
