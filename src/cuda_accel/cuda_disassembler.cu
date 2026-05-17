/**
 * @file src/cuda_accel/cuda_disassembler.cu
 * @brief Parallel x86-64 CFG disassembler — CUDA port of parallel_disasm.cl.
 *
 * One thread per seed entry-point.
 */
#include "retdec/cuda_accel/cuda_disassembler.h"
#include "retdec/cuda_accel/cuda_context.h"
#include "retdec/cuda_accel/cuda_profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

// ═══════════════════════════════════════════════════════════════════════════
// Device-side basic block layout (must match the host BasicBlock struct layout)
// ═══════════════════════════════════════════════════════════════════════════

struct alignas(8) CUDABasicBlock {
    unsigned long long startAddr;
    unsigned long long endAddr;
    unsigned long long successor0;
    unsigned long long successor1;
    unsigned int       insnCount;
    unsigned int       flags;
};

#define RETDEC_BB_ADDR_NONE_D   0xFFFFFFFFFFFFFFFFULL
#define RETDEC_BB_FLAG_HAS_CALL 1u
#define RETDEC_BB_FLAG_ENDS_RET 2u
#define RETDEC_BB_FLAG_ENDS_JMP 4u
#define RETDEC_BB_FLAG_ENDS_JCC 8u
#define RETDEC_BB_FLAG_INVALID  16u

// ═══════════════════════════════════════════════════════════════════════════
// GPU: x86-64 instruction length decoder + branch info decoder
// (direct port from parallel_disasm.cl)
// ═══════════════════════════════════════════════════════════════════════════

#ifdef RETDEC_HAS_CUDA

struct BranchInfo {
    int                type;   // 0=none, 1=JMP, 2=JCC, 3=CALL, 4=RET
    unsigned long long target;
};

__device__ static unsigned int x86InsnLength(const unsigned char* bytes,
                                              unsigned long long   off,
                                              unsigned long long   maxOff)
{
    unsigned long long start = off;
    unsigned char b;

    bool has_66 = false, has_67 = false, has_rex = false, rex_w = false;

    for (int pfx = 0; pfx < 5 && off < maxOff; ++pfx) {
        b = bytes[off];
        if      (b == 0x66) { has_66 = true; ++off; }
        else if (b == 0x67) { has_67 = true; ++off; }
        else if (b == 0xF0 || b == 0xF2 || b == 0xF3) { ++off; }
        else if (b == 0x26 || b == 0x2E || b == 0x36 ||
                 b == 0x3E || b == 0x64 || b == 0x65) { ++off; }
        else break;
    }
    if (off >= maxOff) return 0;

    b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) {
        has_rex = true; rex_w = (b & 0x08) != 0;
        ++off;
        if (off >= maxOff) return 0;
        b = bytes[off];
    }

    if (b == 0xC4 || b == 0xC5) {
        unsigned int vlen = (b == 0xC4) ? 3u : 2u;
        off += vlen;
        if (off >= maxOff) return 0;
        ++off; // opcode
        if (off >= maxOff) return 0;
        unsigned char modrm = bytes[off]; ++off;
        unsigned int mod = modrm >> 6, rm = modrm & 7u;
        if (mod != 3u) {
            if (!has_67 && rm == 4u) ++off;
            if (mod == 1u) ++off; else if (mod == 2u) off += 4u; else if (rm == 5u) off += 4u;
        }
        if (off > maxOff) return 0;
        return (unsigned int)(off - start);
    }
    if (b == 0x62) {
        off += 4u; if (off >= maxOff) return 0;
        ++off;     if (off >= maxOff) return 0;
        unsigned char modrm = bytes[off]; ++off;
        unsigned int mod = modrm >> 6, rm = modrm & 7u;
        if (mod != 3u) {
            if (rm == 4u) ++off;
            if (mod == 1u) ++off; else if (mod == 2u) off += 4u; else if (rm == 5u) off += 4u;
        }
        if (off > maxOff) return 0;
        return (unsigned int)(off - start);
    }

    unsigned char opc = bytes[off]; ++off;
    if (off > maxOff) return 0;

    bool two_byte = false;
    unsigned char opc2 = 0;
    if (opc == 0x0F) {
        if (off >= maxOff) return 0;
        opc2 = bytes[off]; ++off;
        two_byte = true;
        if ((opc2 == 0x38 || opc2 == 0x3A) && off < maxOff) ++off;
    }

    bool has_modrm = false;
    int  imm_bytes = 0;

    if (!two_byte) {
        if      (opc >= 0x50 && opc <= 0x5F) {}
        else if (opc == 0x9C || opc == 0x9D) {}
        else if (opc >= 0x90 && opc <= 0x97) {}
        else if (opc == 0x98 || opc == 0x99) {}
        else if (opc == 0x9B) {}
        else if (opc == 0x9E || opc == 0x9F) {}
        else if (opc == 0xC3 || opc == 0xCB) {}
        else if (opc == 0xC2 || opc == 0xCA) { imm_bytes = 2; }
        else if (opc == 0xC8) { imm_bytes = 3; }
        else if (opc == 0xC9) {}
        else if (opc == 0xCC || opc == 0xCE) {}
        else if (opc == 0xCD) { imm_bytes = 1; }
        else if (opc == 0xCF) {}
        else if (opc == 0x37 || opc == 0x3F || opc == 0x27 || opc == 0x2F) {}
        else if (opc >= 0xF8 && opc <= 0xFD) {}
        else if (opc == 0xF4 || opc == 0xF5) {}
        else if (opc == 0xD7) {}
        else if (opc == 0xEC || opc == 0xED || opc == 0xEE || opc == 0xEF) {}
        else if (opc == 0xE4 || opc == 0xE5) { imm_bytes = 1; }
        else if (opc == 0xE6 || opc == 0xE7) { imm_bytes = 1; }
        else if ((opc >= 0x70 && opc <= 0x7F) || opc == 0xEB || opc == 0xE3) { imm_bytes = 1; }
        else if (opc == 0xE9) { imm_bytes = 4; }
        else if (opc == 0xE8) { imm_bytes = 4; }
        else if (opc >= 0xE0 && opc <= 0xE2) { imm_bytes = 1; }
        else if (opc >= 0xB0 && opc <= 0xB7) { imm_bytes = 1; }
        else if (opc >= 0xB8 && opc <= 0xBF) { imm_bytes = rex_w ? 8 : (has_66 ? 2 : 4); }
        else if (opc >= 0xA0 && opc <= 0xA3) { imm_bytes = has_67 ? 4 : 8; }
        else if (opc == 0xA8) { imm_bytes = 1; }
        else if (opc == 0xA9) { imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4); }
        else if (opc >= 0xA4 && opc <= 0xAF) {}
        else if (opc == 0x05 || opc == 0x0D || opc == 0x15 || opc == 0x1D ||
                 opc == 0x25 || opc == 0x2D || opc == 0x35 || opc == 0x3D) {
            imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4);
        }
        else if (opc == 0x04 || opc == 0x0C || opc == 0x14 || opc == 0x1C ||
                 opc == 0x24 || opc == 0x2C || opc == 0x34 || opc == 0x3C) { imm_bytes = 1; }
        else if (opc == 0x68) { imm_bytes = has_66 ? 2 : 4; }
        else if (opc == 0x6A) { imm_bytes = 1; }
        else if (opc == 0x83) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0x81) { has_modrm = true; imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4); }
        else if (opc == 0xC0 || opc == 0xC1) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0xD0 || opc == 0xD1 || opc == 0xD2 || opc == 0xD3) { has_modrm = true; }
        else if (opc == 0xC6) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0xC7) { has_modrm = true; imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4); }
        else if (opc == 0x6B) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0x69) { has_modrm = true; imm_bytes = has_66 ? 2 : 4; }
        else { has_modrm = true; }
    } else {
        if      (opc2 >= 0x80 && opc2 <= 0x8F) { imm_bytes = 4; }
        else if (opc2 == 0xA4 || opc2 == 0xAC) { has_modrm = true; imm_bytes = 1; }
        else if (opc2 == 0xAF) { has_modrm = true; }
        else if (opc2 == 0xB6 || opc2 == 0xB7 || opc2 == 0xBE || opc2 == 0xBF) { has_modrm = true; }
        else if (opc2 == 0xA2 || opc2 == 0x05 || opc2 == 0x07 || opc2 == 0x31 ||
                 opc2 == 0x34 || opc2 == 0x35) {}
        else if (opc2 == 0x1F || opc2 == 0x18) { has_modrm = true; }
        else { has_modrm = true; }
    }

    if (has_modrm) {
        if (off >= maxOff) return 0;
        unsigned char modrm = bytes[off]; ++off;
        unsigned int mod = modrm >> 6, rm = modrm & 7u;
        unsigned int disp = 0;
        if      (mod == 1u) disp = 1u;
        else if (mod == 2u) disp = 4u;
        else if (mod == 0u && rm == 5u) disp = 4u;
        bool need_sib = (!has_67 && mod != 3u && rm == 4u);
        if (need_sib) {
            if (off >= maxOff) return 0;
            unsigned char sib = bytes[off]; ++off;
            if ((sib & 7u) == 5u && mod == 0u) disp = 4u;
        }
        off += disp;
    }
    off += (unsigned long long)(unsigned int)imm_bytes;
    if (off > maxOff || off > start + 15u) return 0;
    return (unsigned int)(off - start);
}

__device__ static BranchInfo decodeBranch(const unsigned char* bytes,
                                           unsigned long long   insnOff,
                                           unsigned int         insnLen,
                                           unsigned long long   baseVMA)
{
    BranchInfo bi{0, RETDEC_BB_ADDR_NONE_D};
    if (insnLen == 0) return bi;

    unsigned long long off = insnOff;
    unsigned char b;
    for (int i = 0; i < 5; ++i) {
        b = bytes[off];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65)
            ++off;
        else break;
    }
    b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) { ++off; b = bytes[off]; }

    unsigned long long next = insnOff + (unsigned long long)insnLen;

    if (b == 0x0F) {
        unsigned char b2 = bytes[off+1];
        if (b2 >= 0x80 && b2 <= 0x8F) {
            int rel = (int)((unsigned int)bytes[off+2]
                          | ((unsigned int)bytes[off+3] << 8)
                          | ((unsigned int)bytes[off+4] << 16)
                          | ((unsigned int)bytes[off+5] << 24));
            bi.type   = 2;
            bi.target = baseVMA + next + (long long)rel;
            return bi;
        }
    }
    if (b >= 0x70 && b <= 0x7F) {
        int rel = (int)(signed char)bytes[off+1];
        bi.type = 2; bi.target = baseVMA + next + (long long)rel; return bi;
    }
    if (b == 0xE3) {
        int rel = (int)(signed char)bytes[off+1];
        bi.type = 2; bi.target = baseVMA + next + (long long)rel; return bi;
    }
    if (b == 0xEB) {
        int rel = (int)(signed char)bytes[off+1];
        bi.type = 1; bi.target = baseVMA + next + (long long)rel; return bi;
    }
    if (b == 0xE9) {
        int rel = (int)((unsigned int)bytes[off+1]
                       | ((unsigned int)bytes[off+2] << 8)
                       | ((unsigned int)bytes[off+3] << 16)
                       | ((unsigned int)bytes[off+4] << 24));
        bi.type = 1; bi.target = baseVMA + next + (long long)rel; return bi;
    }
    if (b == 0xFF) {
        unsigned char modrm = bytes[off+1];
        unsigned int  reg   = (modrm >> 3) & 7u;
        if (reg == 4u || reg == 5u) { bi.type = 1; return bi; }
        if (reg == 2u || reg == 3u) { bi.type = 3; return bi; }
    }
    if (b == 0xE8) {
        int rel = (int)((unsigned int)bytes[off+1]
                       | ((unsigned int)bytes[off+2] << 8)
                       | ((unsigned int)bytes[off+3] << 16)
                       | ((unsigned int)bytes[off+4] << 24));
        bi.type = 3; bi.target = baseVMA + next + (long long)rel; return bi;
    }
    if (b == 0xC3 || b == 0xCB || b == 0xC2 || b == 0xCA || b == 0xCF) {
        bi.type = 4; return bi;
    }
    return bi;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main CUDA kernel
// ═══════════════════════════════════════════════════════════════════════════

__global__ void retdec_parallel_disasm_kernel(
    const unsigned char*       bytes,
    unsigned long long          byteCount,
    const unsigned long long*  entryOffsets,
    unsigned int                numEntries,
    unsigned long long          baseVMA,
    CUDABasicBlock*            bbOut,
    unsigned int*              visitedWords,  // atomic bitset, 1 bit per byte
    unsigned int*              errorFlags)
{
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= numEntries) return;

    unsigned long long seed = entryOffsets[gid];
    if (seed >= byteCount) {
        errorFlags[gid] = 1u;
        return;
    }

    CUDABasicBlock bb{};
    bb.startAddr  = baseVMA + seed;
    bb.endAddr    = baseVMA + seed;
    bb.successor0 = RETDEC_BB_ADDR_NONE_D;
    bb.successor1 = RETDEC_BB_ADDR_NONE_D;
    bb.insnCount  = 0u;
    bb.flags      = 0u;

    unsigned long long off = seed;

    for (unsigned int steps = 0; steps < 4096u && off < byteCount; ++steps) {
        unsigned int wordIdx = (unsigned int)(off >> 5);
        unsigned int bitIdx  = (unsigned int)(off & 31u);
        unsigned int mask    = 1u << bitIdx;
        unsigned int oldVal  = atomicOr(&visitedWords[wordIdx], mask);
        if (oldVal & mask) {
            bb.successor0 = baseVMA + off;
            break;
        }

        unsigned int len = x86InsnLength(bytes, off, byteCount);
        if (len == 0u) {
            bb.flags |= RETDEC_BB_FLAG_INVALID;
            break;
        }

        unsigned long long nextOff = off + (unsigned long long)len;
        BranchInfo bi = decodeBranch(bytes, off, len, baseVMA);

        ++bb.insnCount;
        bb.endAddr = baseVMA + nextOff;

        if (bi.type == 3) {
            bb.flags |= RETDEC_BB_FLAG_HAS_CALL;
            off = nextOff;
        } else if (bi.type == 1) {
            bb.flags    |= RETDEC_BB_FLAG_ENDS_JMP;
            bb.successor0 = bi.target;
            break;
        } else if (bi.type == 2) {
            bb.flags    |= RETDEC_BB_FLAG_ENDS_JCC;
            bb.successor0 = baseVMA + nextOff;
            bb.successor1 = bi.target;
            break;
        } else if (bi.type == 4) {
            bb.flags |= RETDEC_BB_FLAG_ENDS_RET;
            break;
        } else {
            off = nextOff;
        }
    }

    bbOut[gid]      = bb;
    errorFlags[gid] = 0u;
}

#endif // RETDEC_HAS_CUDA

// ═══════════════════════════════════════════════════════════════════════════
// CPU fallback — uses std::async for parallelism
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Minimal CPU x86-64 length decoder (same logic, plain C++)
static unsigned int cpuX86InsnLen(const std::uint8_t* bytes,
                                   std::size_t off, std::size_t maxOff) {
    auto start = off;
    bool has_66=false, has_67=false, rex_w=false;
    for (int i=0;i<5&&off<maxOff;++i) {
        auto b=bytes[off];
        if(b==0x66){has_66=true;++off;}
        else if(b==0x67){has_67=true;++off;}
        else if(b==0xF0||b==0xF2||b==0xF3){++off;}
        else if(b==0x26||b==0x2E||b==0x36||b==0x3E||b==0x64||b==0x65){++off;}
        else break;
    }
    if(off>=maxOff)return 0;
    auto b=bytes[off];
    if(b>=0x40&&b<=0x4F){rex_w=(b&8)!=0;++off;if(off>=maxOff)return 0;b=bytes[off];}
    // VEX/EVEX (simplified)
    if(b==0xC4||b==0xC5){off+=(b==0xC4?3u:2u);if(off>=maxOff)return 0;
        ++off;if(off>=maxOff)return 0;
        unsigned char mr=bytes[off];++off;unsigned mod=(unsigned)mr>>6,rm=(unsigned)mr&7u;
        if(mod!=3u){if(!has_67&&rm==4u)++off;if(mod==1u)++off;else if(mod==2u)off+=4u;else if(rm==5u)off+=4u;}
        return off>maxOff?0:(unsigned int)(off-start);}
    if(b==0x62){off+=4u;if(off>=maxOff)return 0;++off;if(off>=maxOff)return 0;
        unsigned char mr=bytes[off];++off;unsigned mod=(unsigned)mr>>6,rm=(unsigned)mr&7u;
        if(mod!=3u){if(rm==4u)++off;if(mod==1u)++off;else if(mod==2u)off+=4u;else if(rm==5u)off+=4u;}
        return off>maxOff?0:(unsigned int)(off-start);}
    auto opc=bytes[off];++off;
    bool two=false;unsigned char opc2=0;
    if(opc==0x0F){if(off>=maxOff)return 0;opc2=bytes[off];++off;two=true;
        if((opc2==0x38||opc2==0x3A)&&off<maxOff)++off;}
    bool has_mr=false;int imm=0;
    if(!two){
        if(opc>=0x50&&opc<=0x5F){}
        else if(opc==0xC3||opc==0xCB){}
        else if(opc==0xC2||opc==0xCA){imm=2;}
        else if(opc==0xC8){imm=3;}
        else if(opc==0xC9||opc==0x9C||opc==0x9D||(opc>=0x90&&opc<=0x97)||opc==0x98||opc==0x99||opc==0x9B||opc==0x9E||opc==0x9F){}
        else if(opc==0xCC||opc==0xCE){}
        else if(opc==0xCD){imm=1;}
        else if(opc==0xCF){}
        else if(opc>=0xF8&&opc<=0xFD){}
        else if(opc==0xF4||opc==0xF5||opc==0xD7){}
        else if(opc==0xEC||opc==0xED||opc==0xEE||opc==0xEF){}
        else if(opc==0xE4||opc==0xE5||opc==0xE6||opc==0xE7){imm=1;}
        else if((opc>=0x70&&opc<=0x7F)||opc==0xEB||opc==0xE3){imm=1;}
        else if(opc==0xE9){imm=4;}
        else if(opc==0xE8){imm=4;}
        else if(opc>=0xE0&&opc<=0xE2){imm=1;}
        else if(opc>=0xB0&&opc<=0xB7){imm=1;}
        else if(opc>=0xB8&&opc<=0xBF){imm=rex_w?8:(has_66?2:4);}
        else if(opc>=0xA0&&opc<=0xA3){imm=has_67?4:8;}
        else if(opc==0xA8){imm=1;}
        else if(opc==0xA9){imm=rex_w?4:(has_66?2:4);}
        else if(opc>=0xA4&&opc<=0xAF){}
        else if(opc==0x05||opc==0x0D||opc==0x15||opc==0x1D||opc==0x25||opc==0x2D||opc==0x35||opc==0x3D){imm=rex_w?4:(has_66?2:4);}
        else if(opc==0x04||opc==0x0C||opc==0x14||opc==0x1C||opc==0x24||opc==0x2C||opc==0x34||opc==0x3C){imm=1;}
        else if(opc==0x68){imm=has_66?2:4;}
        else if(opc==0x6A){imm=1;}
        else if(opc==0x83||opc==0xC0||opc==0xC1){has_mr=true;imm=1;}
        else if(opc==0x81){has_mr=true;imm=rex_w?4:(has_66?2:4);}
        else if(opc==0xD0||opc==0xD1||opc==0xD2||opc==0xD3){has_mr=true;}
        else if(opc==0xC6){has_mr=true;imm=1;}
        else if(opc==0xC7){has_mr=true;imm=rex_w?4:(has_66?2:4);}
        else if(opc==0x6B){has_mr=true;imm=1;}
        else if(opc==0x69){has_mr=true;imm=has_66?2:4;}
        else{has_mr=true;}
    } else {
        if(opc2>=0x80&&opc2<=0x8F){imm=4;}
        else if(opc2==0xA4||opc2==0xAC){has_mr=true;imm=1;}
        else if(opc2==0xAF||opc2==0xB6||opc2==0xB7||opc2==0xBE||opc2==0xBF||opc2==0x1F||opc2==0x18){has_mr=true;}
        else if(opc2==0xA2||opc2==0x05||opc2==0x07||opc2==0x31||opc2==0x34||opc2==0x35){}
        else{has_mr=true;}
    }
    if(has_mr){
        if(off>=maxOff)return 0;
        auto mr=bytes[off];++off;
        unsigned int mod=mr>>6,rm=mr&7u,disp=0;
        if(mod==1u)disp=1u;else if(mod==2u)disp=4u;else if(mod==0u&&rm==5u)disp=4u;
        if(!has_67&&mod!=3u&&rm==4u){
            if(off>=maxOff)return 0;
            auto sib=bytes[off];++off;
            if((sib&7u)==5u&&mod==0u)disp=4u;
        }
        off+=disp;
    }
    off+=(std::size_t)(unsigned int)imm;
    if(off>maxOff||off>start+15u)return 0;
    return (unsigned int)(off-start);
}

static BasicBlock cpuDisassembleOne(const std::uint8_t* bytes,
                                     std::size_t byteCount,
                                     std::uint64_t baseVMA,
                                     std::uint64_t seedVMA,
                                     std::vector<std::atomic_uint>& visited) {
    BasicBlock bb{};
    bb.startAddr  = seedVMA;
    bb.endAddr    = seedVMA;
    bb.successor0 = kBBAddrNone;
    bb.successor1 = kBBAddrNone;

    if (seedVMA < baseVMA) { bb.flags |= BB_INVALID; return bb; }
    std::size_t off = (std::size_t)(seedVMA - baseVMA);
    if (off >= byteCount) { bb.flags |= BB_INVALID; return bb; }

    for (unsigned steps = 0; steps < 4096u && off < byteCount; ++steps) {
        // Mark visited
        std::size_t wordIdx = off >> 5;
        unsigned int bitIdx  = (unsigned int)(off & 31u);
        unsigned int mask    = 1u << bitIdx;
        unsigned int old     = visited[wordIdx].fetch_or(mask);
        if (old & mask) {
            bb.successor0 = baseVMA + off;
            break;
        }
        unsigned int len = cpuX86InsnLen(bytes, off, byteCount);
        if (len == 0) { bb.flags |= BB_INVALID; break; }

        std::size_t nextOff = off + len;
        ++bb.insnCount;
        bb.endAddr = baseVMA + nextOff;

        // Decode branch
        auto b = bytes[off];
        // Skip prefixes
        std::size_t bOff = off;
        for (int p=0;p<5;++p){auto pb=bytes[bOff];if(pb==0x66||pb==0x67||pb==0xF0||pb==0xF2||pb==0xF3||pb==0x26||pb==0x2E||pb==0x36||pb==0x3E||pb==0x64||pb==0x65)++bOff;else break;}
        b = bytes[bOff];
        if(b>=0x40&&b<=0x4F){++bOff;b=bytes[bOff];}
        std::uint64_t next = baseVMA + nextOff;

        if (b == 0x0F && bOff+1 < byteCount) {
            auto b2 = bytes[bOff+1];
            if (b2 >= 0x80 && b2 <= 0x8F && bOff+5 < byteCount) {
                int rel = (int)((unsigned int)bytes[bOff+2]|((unsigned int)bytes[bOff+3]<<8)|((unsigned int)bytes[bOff+4]<<16)|((unsigned int)bytes[bOff+5]<<24));
                bb.flags |= BB_ENDS_JCC; bb.successor0 = next; bb.successor1 = next + (long long)rel; goto done;
            }
        }
        if ((b >= 0x70 && b <= 0x7F) || b == 0xE3) {
            if (bOff+1 < byteCount) {
                int rel = (int)(signed char)bytes[bOff+1];
                bb.flags |= BB_ENDS_JCC; bb.successor0 = next; bb.successor1 = next + (long long)rel; goto done;
            }
        }
        if (b == 0xEB && bOff+1 < byteCount) {
            int rel = (int)(signed char)bytes[bOff+1];
            bb.flags |= BB_ENDS_JMP; bb.successor0 = next + (long long)rel; goto done;
        }
        if (b == 0xE9 && bOff+4 < byteCount) {
            int rel = (int)((unsigned int)bytes[bOff+1]|((unsigned int)bytes[bOff+2]<<8)|((unsigned int)bytes[bOff+3]<<16)|((unsigned int)bytes[bOff+4]<<24));
            bb.flags |= BB_ENDS_JMP; bb.successor0 = next + (long long)rel; goto done;
        }
        if (b == 0xFF && bOff+1 < byteCount) {
            unsigned int reg = (bytes[bOff+1] >> 3) & 7u;
            if (reg == 4u || reg == 5u) { bb.flags |= BB_ENDS_JMP; goto done; }
            if (reg == 2u || reg == 3u) { bb.flags |= BB_HAS_CALL; off = nextOff; continue; }
        }
        if (b == 0xE8 && bOff+4 < byteCount) {
            bb.flags |= BB_HAS_CALL; off = nextOff; continue;
        }
        if (b == 0xC3 || b == 0xCB || b == 0xC2 || b == 0xCA || b == 0xCF) {
            bb.flags |= BB_ENDS_RET; goto done;
        }
        off = nextOff;
        continue;
    done:
        break;
    }
    return bb;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Host class implementation
// ═══════════════════════════════════════════════════════════════════════════

CUDADisassembler::CUDADisassembler(CUDAContext* ctx) : ctx_(ctx) {
#ifdef RETDEC_HAS_CUDA
    if (ctx_ && ctx_->isReady()) gpuReady_ = true;
#endif
}

CUDADisassembler::~CUDADisassembler() = default;

std::vector<BasicBlock> CUDADisassembler::disassemble(
    const std::uint8_t* codeBytes,
    std::size_t codeSize,
    std::uint64_t baseVMA,
    const std::vector<std::uint64_t>& entryVMAs)
{
    if (entryVMAs.empty() || !codeBytes || codeSize == 0) return {};

    auto t0 = std::chrono::steady_clock::now();

#ifdef RETDEC_HAS_CUDA
    if (gpuReady_ && ctx_) {
        std::size_t n = entryVMAs.size();

        // Convert VMAs to offsets
        std::vector<std::uint64_t> offsets(n);
        for (std::size_t i = 0; i < n; ++i)
            offsets[i] = (entryVMAs[i] >= baseVMA) ? (entryVMAs[i] - baseVMA) : codeSize;

        std::size_t visitedWords = (codeSize + 31u) / 32u;

        void* dBytes    = nullptr;
        void* dEntries  = nullptr;
        void* dBbOut    = nullptr;
        void* dVisited  = nullptr;
        void* dErrors   = nullptr;

        cudaStream_t stream = ctx_->stream();

        auto fail = [&]() {
            if (dBytes)   cudaFree(dBytes);
            if (dEntries) cudaFree(dEntries);
            if (dBbOut)   cudaFree(dBbOut);
            if (dVisited) cudaFree(dVisited);
            if (dErrors)  cudaFree(dErrors);
        };

        if (cudaMalloc(&dBytes,   codeSize)                   != cudaSuccess ||
            cudaMalloc(&dEntries, n * sizeof(std::uint64_t))  != cudaSuccess ||
            cudaMalloc(&dBbOut,   n * sizeof(CUDABasicBlock)) != cudaSuccess ||
            cudaMalloc(&dVisited, visitedWords * sizeof(unsigned int)) != cudaSuccess ||
            cudaMalloc(&dErrors,  n * sizeof(unsigned int))   != cudaSuccess) {
            fail(); gpuReady_ = false;
            lastError_ = "cudaMalloc failed";
            return disassembleCPU(codeBytes, codeSize, baseVMA, entryVMAs);
        }

        cudaMemsetAsync(dVisited, 0, visitedWords * sizeof(unsigned int), stream);
        cudaMemsetAsync(dErrors,  0, n * sizeof(unsigned int), stream);
        cudaMemcpyAsync(dBytes,   codeBytes,   codeSize, cudaMemcpyHostToDevice, stream);
        cudaMemcpyAsync(dEntries, offsets.data(), n * sizeof(std::uint64_t),
                        cudaMemcpyHostToDevice, stream);

        unsigned int block = 64;
        unsigned int grid  = (unsigned int)((n + block - 1) / block);
        retdec_parallel_disasm_kernel<<<grid, block, 0, stream>>>(
            (const unsigned char*)dBytes,
            (unsigned long long)codeSize,
            (const unsigned long long*)dEntries,
            (unsigned int)n,
            (unsigned long long)baseVMA,
            (CUDABasicBlock*)dBbOut,
            (unsigned int*)dVisited,
            (unsigned int*)dErrors);

        std::vector<CUDABasicBlock> gpuBBs(n);
        cudaMemcpyAsync(gpuBBs.data(), dBbOut, n * sizeof(CUDABasicBlock),
                        cudaMemcpyDeviceToHost, stream);
        cudaStreamSynchronize(stream);

        fail();

        if (cudaGetLastError() != cudaSuccess) {
            gpuReady_ = false;
            return disassembleCPU(codeBytes, codeSize, baseVMA, entryVMAs);
        }

        std::vector<BasicBlock> result(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto& g = gpuBBs[i]; auto& r = result[i];
            r.startAddr  = g.startAddr;
            r.endAddr    = g.endAddr;
            r.successor0 = g.successor0;
            r.successor1 = g.successor1;
            r.insnCount  = g.insnCount;
            r.flags      = g.flags;
        }

        auto t1 = std::chrono::steady_clock::now();
        CUDAProfiler::instance().record("parallel_disasm",
            (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());
        return result;
    }
#endif

    auto result = disassembleCPU(codeBytes, codeSize, baseVMA, entryVMAs);
    auto t1 = std::chrono::steady_clock::now();
    CUDAProfiler::instance().record("parallel_disasm_cpu",
        (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());
    return result;
}

std::vector<BasicBlock> CUDADisassembler::disassembleCPU(
    const std::uint8_t* bytes, std::size_t size, std::uint64_t base,
    const std::vector<std::uint64_t>& seeds)
{
    std::size_t n           = seeds.size();
    std::size_t visitedWords = (size + 31u) / 32u;
    std::vector<std::atomic_uint> visited(visitedWords);
    for (auto& v : visited) v.store(0u);

    unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    std::size_t chunkSize = std::max(std::size_t(1), n / hw);

    std::vector<std::future<std::vector<BasicBlock>>> futures;
    for (std::size_t start = 0; start < n; start += chunkSize) {
        std::size_t end = std::min(start + chunkSize, n);
        futures.push_back(std::async(std::launch::async,
            [&, start, end]() {
                std::vector<BasicBlock> chunk;
                chunk.reserve(end - start);
                for (std::size_t i = start; i < end; ++i)
                    chunk.push_back(cpuDisassembleOne(bytes, size, base, seeds[i], visited));
                return chunk;
            }));
    }

    std::vector<BasicBlock> result;
    result.reserve(n);
    for (auto& f : futures) {
        auto chunk = f.get();
        for (auto& bb : chunk) result.push_back(std::move(bb));
    }
    return result;
}

} // namespace retdec::cuda_accel
