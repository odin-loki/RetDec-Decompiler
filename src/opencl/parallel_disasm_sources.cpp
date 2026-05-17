/**
 * @file src/opencl/parallel_disasm_sources.cpp
 * @brief Embedded OpenCL C source for parallel_disasm.cl (runtime fallback).
 *        Keep in sync with src/opencl/kernels/parallel_disasm.cl.
 */

#include "retdec/opencl/kernel_sources.h"

namespace retdec {
namespace opencl {

const char *parallelDisasmClSource()
{
    // Embedded verbatim copy of kernels/parallel_disasm.cl.
    // The EmbedOpenCLKernels CMake module generates a parallel auto-updated
    // version; this file is the manual fallback.
    return R"CL_SOURCE(
#define RETDEC_BB_ADDR_NONE   0xFFFFFFFFFFFFFFFFUL
#define RETDEC_BB_FLAG_HAS_CALL   (1u)
#define RETDEC_BB_FLAG_ENDS_RET   (2u)
#define RETDEC_BB_FLAG_ENDS_JMP   (4u)
#define RETDEC_BB_FLAG_ENDS_JCC   (8u)
#define RETDEC_BB_FLAG_INVALID    (16u)

typedef struct {
    ulong start_addr;
    ulong end_addr;
    ulong successor0;
    ulong successor1;
    uint  insn_count;
    uint  flags;
} RetDecBasicBlock;

static uint x86_insn_length(__global const uchar *bytes, ulong off, ulong max_off)
{
    ulong start = off;
    uchar b;
    bool has66 = false, has67 = false, rexW = false;

    for (int pfx = 0; pfx < 5 && off < max_off; ++pfx) {
        b = bytes[off];
        if (b == 0x66)                                                  { has66 = true; ++off; }
        else if (b == 0x67)                                             { has67 = true; ++off; }
        else if (b == 0xF0 || b == 0xF2 || b == 0xF3)                 { ++off; }
        else if (b == 0x26 || b == 0x2E || b == 0x36 ||
                 b == 0x3E || b == 0x64 || b == 0x65)                  { ++off; }
        else break;
    }
    if (off >= max_off) return 0;
    b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) { rexW = (b & 0x08) != 0; ++off; }
    if (off >= max_off) return 0;

    uchar opc = bytes[off]; ++off;
    if (off > max_off) return 0;

    bool twoB = false;
    uchar opc2 = 0;
    if (opc == 0x0F && off < max_off) {
        opc2 = bytes[off]; ++off;
        twoB = true;
        if ((opc2 == 0x38 || opc2 == 0x3A) && off < max_off) ++off;
    }

    bool hasModRM = false;
    int  immBytes = 0;

    if (!twoB) {
        if      (opc >= 0x50 && opc <= 0x5F)   {}
        else if (opc >= 0x90 && opc <= 0x97)   {}
        else if (opc == 0x98 || opc == 0x99 || opc == 0x9B ||
                 opc == 0x9C || opc == 0x9D || opc == 0x9E || opc == 0x9F) {}
        else if (opc == 0xC3 || opc == 0xCB || opc == 0xC9 ||
                 opc == 0xCC || opc == 0xCE || opc == 0xCF) {}
        else if (opc == 0xC2 || opc == 0xCA) { immBytes = 2; }
        else if (opc == 0xC8)                { immBytes = 3; }
        else if (opc == 0xCD)                { immBytes = 1; }
        else if (opc >= 0xF8 && opc <= 0xFD) {}
        else if (opc == 0xF4 || opc == 0xF5 || opc == 0xD7) {}
        else if (opc == 0xEC || opc == 0xED || opc == 0xEE || opc == 0xEF) {}
        else if (opc == 0xE4 || opc == 0xE5 || opc == 0xE6 || opc == 0xE7) { immBytes = 1; }
        else if ((opc >= 0x70 && opc <= 0x7F) || opc == 0xEB || opc == 0xE3) { immBytes = 1; }
        else if (opc == 0xE0 || opc == 0xE1 || opc == 0xE2) { immBytes = 1; }
        else if (opc == 0xE9 || opc == 0xE8) { immBytes = 4; }
        else if (opc >= 0xB0 && opc <= 0xB7) { immBytes = 1; }
        else if (opc >= 0xB8 && opc <= 0xBF) { immBytes = rexW ? 8 : (has66 ? 2 : 4); }
        else if (opc >= 0xA0 && opc <= 0xA3) { immBytes = has67 ? 4 : 8; }
        else if (opc == 0xA8) { immBytes = 1; }
        else if (opc == 0xA9) { immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc >= 0xA4 && opc <= 0xAF) {}
        else if (opc == 0x05 || opc == 0x0D || opc == 0x15 || opc == 0x1D ||
                 opc == 0x25 || opc == 0x2D || opc == 0x35 || opc == 0x3D) { immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc == 0x04 || opc == 0x0C || opc == 0x14 || opc == 0x1C ||
                 opc == 0x24 || opc == 0x2C || opc == 0x34 || opc == 0x3C) { immBytes = 1; }
        else if (opc == 0x68) { immBytes = has66 ? 2 : 4; }
        else if (opc == 0x6A) { immBytes = 1; }
        else if (opc == 0x83) { hasModRM = true; immBytes = 1; }
        else if (opc == 0x81) { hasModRM = true; immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc == 0xC0 || opc == 0xC1) { hasModRM = true; immBytes = 1; }
        else if (opc == 0xD0 || opc == 0xD1 || opc == 0xD2 || opc == 0xD3) { hasModRM = true; }
        else if (opc == 0xC6) { hasModRM = true; immBytes = 1; }
        else if (opc == 0xC7) { hasModRM = true; immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc == 0x6B) { hasModRM = true; immBytes = 1; }
        else if (opc == 0x69) { hasModRM = true; immBytes = has66 ? 2 : 4; }
        else { hasModRM = true; }
    } else {
        if      (opc2 >= 0x80 && opc2 <= 0x8F) { immBytes = 4; }
        else if (opc2 == 0xA4 || opc2 == 0xAC) { hasModRM = true; immBytes = 1; }
        else if (opc2 == 0x1F || opc2 == 0x18) { hasModRM = true; }
        else if (opc2 == 0xA2 || opc2 == 0x05 || opc2 == 0x07 ||
                 opc2 == 0x31 || opc2 == 0x34 || opc2 == 0x35) {}
        else { hasModRM = true; }
    }

    if (hasModRM) {
        if (off >= max_off) return 0;
        uchar modrm = bytes[off]; ++off;
        uint mod = (uint)(modrm >> 6);
        uint rm  = (uint)(modrm & 7u);
        uint disp = (mod == 1u) ? 1u : (mod == 2u) ? 4u : (mod == 0u && rm == 5u) ? 4u : 0u;
        if (!has67 && mod != 3u && rm == 4u) {
            if (off >= max_off) return 0;
            uchar sib = bytes[off]; ++off;
            if ((sib & 7u) == 5u && mod == 0u) disp = 4u;
        }
        off += disp;
    }

    off += (ulong)(uint)immBytes;
    if (off > max_off || off > start + 15u) return 0;
    return (uint)(off - start);
}

__kernel
__attribute__((reqd_work_group_size(64, 1, 1)))
void retdec_parallel_disasm(
    __global const uchar       *bytes,
    ulong                       byte_count,
    __global const ulong       *entry_offsets,
    uint                        num_entries,
    ulong                       base_vma,
    __global RetDecBasicBlock  *bb_out,
    __global volatile uint     *visited_words,
    __global uint              *error_flags)
{
    size_t gid = get_global_id(0);
    if (gid >= (size_t)num_entries) return;

    ulong seed = entry_offsets[gid];
    if (seed >= byte_count) { error_flags[gid] = 1u; return; }

    RetDecBasicBlock bb;
    bb.start_addr = base_vma + seed;
    bb.end_addr   = base_vma + seed;
    bb.successor0 = RETDEC_BB_ADDR_NONE;
    bb.successor1 = RETDEC_BB_ADDR_NONE;
    bb.insn_count = 0u;
    bb.flags      = 0u;

    ulong off = seed;

    for (uint steps = 0; steps < 4096u && off < byte_count; ++steps) {
        ulong word_idx = off >> 5;
        uint  bit_idx  = (uint)(off & 31u);
        uint  mask     = 1u << bit_idx;
        __global volatile uint *wp = visited_words + word_idx;
        uint old = atomic_or((__global atomic_uint*)wp, mask);
        if (old & mask) { bb.successor0 = base_vma + off; break; }

        uint len = x86_insn_length(bytes, off, byte_count);
        if (len == 0u) { bb.flags |= RETDEC_BB_FLAG_INVALID; break; }

        ulong next_off = off + (ulong)len;
        ++bb.insn_count;
        bb.end_addr = base_vma + next_off;

        /* prefix skip + branch detect */
        ulong po = off;
        uchar bx;
        for (int pi = 0; pi < 5 && po < byte_count; ++pi) {
            bx = bytes[po];
            if (bx == 0x66 || bx == 0x67 || bx == 0xF0 || bx == 0xF2 || bx == 0xF3 ||
                bx == 0x26 || bx == 0x2E || bx == 0x36 || bx == 0x3E || bx == 0x64 || bx == 0x65) { ++po; }
            else break;
        }
        if (po < byte_count && bytes[po] >= 0x40 && bytes[po] <= 0x4F) ++po;
        if (po >= byte_count) { off = next_off; continue; }
        bx = bytes[po];

        if (bx == 0xC3 || bx == 0xCB || bx == 0xC2 || bx == 0xCA || bx == 0xCF) {
            bb.flags |= RETDEC_BB_FLAG_ENDS_RET; break;
        }
        if (bx == 0xEB) {
            int rel = (int)(schar)(bytes[po + 1]);
            bb.flags |= RETDEC_BB_FLAG_ENDS_JMP;
            bb.successor0 = base_vma + next_off + (long)rel; break;
        }
        if (bx == 0xE9) {
            int rel = (int)(
                (uint)(bytes[po+1]) | ((uint)(bytes[po+2])<<8) |
                ((uint)(bytes[po+3])<<16) | ((uint)(bytes[po+4])<<24));
            bb.flags |= RETDEC_BB_FLAG_ENDS_JMP;
            bb.successor0 = base_vma + next_off + (long)rel; break;
        }
        if ((bx >= 0x70 && bx <= 0x7F) || bx == 0xE3) {
            int rel = (int)(schar)(bytes[po + 1]);
            bb.flags |= RETDEC_BB_FLAG_ENDS_JCC;
            bb.successor0 = base_vma + next_off;
            bb.successor1 = base_vma + next_off + (long)rel; break;
        }
        if (bx == 0x0F && po + 1 < byte_count) {
            uchar b2 = bytes[po + 1];
            if (b2 >= 0x80 && b2 <= 0x8F) {
                int rel = (int)(
                    (uint)(bytes[po+2]) | ((uint)(bytes[po+3])<<8) |
                    ((uint)(bytes[po+4])<<16) | ((uint)(bytes[po+5])<<24));
                bb.flags |= RETDEC_BB_FLAG_ENDS_JCC;
                bb.successor0 = base_vma + next_off;
                bb.successor1 = base_vma + next_off + (long)rel; break;
            }
        }
        if (bx == 0xFF && po + 1 < byte_count) {
            uint reg = (uint)((bytes[po+1] >> 3) & 7u);
            if (reg == 4u || reg == 5u) { bb.flags |= RETDEC_BB_FLAG_ENDS_JMP; break; }
            if (reg == 2u || reg == 3u)   bb.flags |= RETDEC_BB_FLAG_HAS_CALL;
        }
        if (bx == 0xE8) bb.flags |= RETDEC_BB_FLAG_HAS_CALL;
        off = next_off;
    }

    bb_out[gid] = bb;
    error_flags[gid] = 0u;
}
)CL_SOURCE";
}

} // namespace opencl
} // namespace retdec
