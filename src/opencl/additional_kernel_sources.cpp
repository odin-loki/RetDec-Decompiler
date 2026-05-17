/**
 * @file src/opencl/additional_kernel_sources.cpp
 * @brief Embedded OpenCL C sources (runtime fallback, keep in sync with kernels/*.cl).
 */

#include "retdec/opencl/kernel_sources.h"

namespace retdec {
namespace opencl {

const char *typePropagationClSource()
{
    // Full implementation from kernels/type_propagation.cl (embedded verbatim).
    return R"CL(
static uchar merge_width(uchar a, uchar b)
{
    if (a == 0) return b;
    if (b == 0) return a;
    return (a > b) ? a : b;
}

static uchar merge_signedness(uchar a, uchar b)
{
    if (a == 0) return b;
    if (b == 0) return a;
    if (a == b) return a;
    return 1u;
}

static uint dsu_find(__global uint *parent, uint x)
{
    uint root = x;
    for (uint d = 0; d < 64u; ++d) {
        uint p = parent[root];
        if (p == root) break;
        root = p;
    }
    uint cur = x;
    for (uint d = 0; d < 64u; ++d) {
        uint p = parent[cur];
        if (p == cur) break;
        parent[cur] = root;
        cur = p;
    }
    return root;
}

static bool dsu_union(__global uint  *parent,
                      __global uint  *rnk,
                      __global uchar *width,
                      __global uchar *signedness,
                      __global uchar *is_pointer,
                      uint ra, uint rb)
{
    if (ra == rb) return false;
    uchar mw = merge_width(width[ra], width[rb]);
    uchar ms = merge_signedness(signedness[ra], signedness[rb]);
    uchar mp = is_pointer[ra] | is_pointer[rb];
    uint winner, loser;
    if (rnk[ra] > rnk[rb]) { winner = ra; loser = rb; }
    else if (rnk[rb] > rnk[ra]) { winner = rb; loser = ra; }
    else { winner = ra; loser = rb; rnk[ra] += 1u; }
    parent[loser]      = winner;
    width[winner]      = mw;
    signedness[winner] = ms;
    is_pointer[winner] = mp;
    return true;
}

__kernel void retdec_type_propagation(
    __global uint  *parent,
    __global uint  *rnk,
    __global uchar *width,
    __global uchar *signedness,
    __global uchar *is_pointer,
    __global const uint *con_offsets,
    __global const uint *con_a,
    __global const uint *con_b,
    __global uint  *dirty_flags,
    __global uint  *global_done,
    uint num_funcs)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_funcs) return;
    uint c_start = con_offsets[gid];
    uint c_end   = con_offsets[gid + 1u];
    uint local_dirty = 0u;
    for (uint ci = c_start; ci < c_end; ++ci) {
        uint ra = dsu_find(parent, con_a[ci]);
        uint rb = dsu_find(parent, con_b[ci]);
        if (dsu_union(parent, rnk, width, signedness, is_pointer, ra, rb))
            local_dirty = 1u;
    }
    if (local_dirty) {
        dirty_flags[gid] = 1u;
        *global_done = 0u;
    }
}

__kernel void retdec_type_seed(
    __global       uchar *width,
    __global       uchar *signedness,
    __global       uchar *is_pointer,
    __global const uint  *operand_slot,
    __global const uchar *operand_width,
    __global const uchar *operand_sign,
    __global const uchar *operand_ptr,
    uint num_operands)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_operands) return;
    uint slot = operand_slot[gid];
    width[slot]      = merge_width(width[slot], operand_width[gid]);
    signedness[slot] = merge_signedness(signedness[slot], operand_sign[gid]);
    is_pointer[slot] |= operand_ptr[gid];
}
)CL";
}

const char *steensgaardClSource()
{
    return R"CL(
#define CON_COPY    0u
#define CON_ADDR_OF 1u
#define CON_STORE   2u
#define CON_LOAD    3u
#define NO_TARGET   0xFFFFFFFFu

static uint ecr_find(__global uint *parent, uint x)
{
    uint root = x;
    for (uint d = 0; d < 64u; ++d) { uint p = parent[root]; if (p == root) break; root = p; }
    uint cur = x;
    for (uint d = 0; d < 64u; ++d) { uint p = parent[cur]; if (p == cur) break; parent[cur] = root; cur = p; }
    return root;
}

static bool ecr_union(__global uint *parent, __global uint *rnk, uint ra, uint rb)
{
    if (ra == rb) return false;
    uint winner, loser;
    if (rnk[ra] > rnk[rb]) { winner = ra; loser = rb; }
    else if (rnk[rb] > rnk[ra]) { winner = rb; loser = ra; }
    else { winner = ra; loser = rb; rnk[ra] += 1u; }
    parent[loser] = winner;
    return true;
}

static bool ecr_join(__global uint *parent, __global uint *rnk, __global uint *pts_to,
                     __global uint *pending_a, __global uint *pending_b,
                     __global uint *pending_count, uint a, uint b, uint max_pending)
{
    uint ra = ecr_find(parent, a), rb = ecr_find(parent, b);
    if (ra == rb) return false;
    uint pa = pts_to[ra], pb = pts_to[rb];
    bool ch = ecr_union(parent, rnk, ra, rb);
    if (pa != NO_TARGET && pb != NO_TARGET) {
        uint idx = atomic_inc((__global atomic_uint*)pending_count);
        if (idx < max_pending) { pending_a[idx] = pa; pending_b[idx] = pb; }
    } else {
        uint winner = ecr_find(parent, a);
        if (pts_to[winner] == NO_TARGET) { if (pa != NO_TARGET) pts_to[winner] = pa; else if (pb != NO_TARGET) pts_to[winner] = pb; }
    }
    return ch;
}

__kernel void retdec_steensgaard_copy(__global uint *parent, __global uint *rnk, __global uint *pts_to,
    __global const uchar *con_type, __global const uint *con_a, __global const uint *con_b,
    uint num_constraints, __global uint *pending_a, __global uint *pending_b,
    __global uint *pending_count, uint max_pending, __global uint *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_constraints || con_type[gid] != CON_COPY) return;
    if (ecr_join(parent, rnk, pts_to, pending_a, pending_b, pending_count, con_a[gid], con_b[gid], max_pending))
        *changed_flag = 1u;
}

__kernel void retdec_steensgaard_addr(__global uint *parent, __global uint *rnk, __global uint *pts_to,
    __global const uchar *con_type, __global const uint *con_a, __global const uint *con_b,
    uint num_constraints, __global uint *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_constraints || con_type[gid] != CON_ADDR_OF) return;
    uint ra = ecr_find(parent, con_a[gid]), rb = ecr_find(parent, con_b[gid]);
    uint pra = pts_to[ra];
    if (pra == NO_TARGET) { pts_to[ra] = rb; *changed_flag = 1u; }
    else { if (ecr_union(parent, rnk, ecr_find(parent, pra), rb)) *changed_flag = 1u; }
}

__kernel void retdec_steensgaard_deref(__global uint *parent, __global uint *rnk, __global uint *pts_to,
    __global const uchar *con_type, __global const uint *con_a, __global const uint *con_b,
    uint num_constraints, __global uint *pending_a, __global uint *pending_b,
    __global uint *pending_count, uint max_pending, __global uint *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_constraints) return;
    uchar ct = con_type[gid];
    if (ct != CON_STORE && ct != CON_LOAD) return;
    uint ra = ecr_find(parent, con_a[gid]), rb = ecr_find(parent, con_b[gid]);
    if (ct == CON_STORE) {
        uint pra = pts_to[ra];
        if (pra != NO_TARGET && ecr_join(parent, rnk, pts_to, pending_a, pending_b, pending_count, ecr_find(parent, pra), rb, max_pending))
            *changed_flag = 1u;
    } else {
        uint prb = pts_to[rb];
        if (prb != NO_TARGET && ecr_join(parent, rnk, pts_to, pending_a, pending_b, pending_count, ra, ecr_find(parent, prb), max_pending))
            *changed_flag = 1u;
    }
}

__kernel void retdec_steensgaard_flush_pending(__global uint *parent, __global uint *rnk, __global uint *pts_to,
    __global uint *pending_a, __global uint *pending_b, uint num_pending,
    __global uint *next_pending_a, __global uint *next_pending_b, __global uint *next_pending_count,
    uint max_next_pending, __global uint *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_pending) return;
    if (ecr_join(parent, rnk, pts_to, next_pending_a, next_pending_b, next_pending_count,
                 ecr_find(parent, pending_a[gid]), ecr_find(parent, pending_b[gid]), max_next_pending))
        *changed_flag = 1u;
}
)CL";
}

const char *semanticHashClSource()
{
    // Full mini x86-64 emulator kernel — embedded verbatim from kernels/semantic_hash.cl.
    return R"SEMHASH(
#define SCRATCH_SIZE     4096u
#define MAX_STEPS        16384u
#define MAX_CALL_DEPTH   4u
#define NO_FAULT         0u
#define FAULT_RET        1u
#define FAULT_GENERAL    2u
#define FAULT_STEPLIMIT  3u
#define FAULT_UNSUPPORTED 4u

#define FLAG_CF  (1u << 0)
#define FLAG_PF  (1u << 2)
#define FLAG_AF  (1u << 4)
#define FLAG_ZF  (1u << 6)
#define FLAG_SF  (1u << 7)
#define FLAG_OF  (1u << 11)

#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7

typedef struct {
    ulong regs[16];
    uint  flags;
    uint  rip;
    uint  halted;
    uint  call_depth;
    ulong scratch_base;
} X86State;

static ulong fnv1a64_step(ulong h, ulong val)
{
    for (int b = 0; b < 8; ++b) {
        h ^= (val & 0xFFu);
        h *= 1099511628211UL;
        val >>= 8;
    }
    return h;
}

static ulong mem_read64(__global const uchar *scratch, ulong base, ulong addr, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 8u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return 0; }
    __global const uchar *p = scratch + rel;
    return (ulong)p[0] | ((ulong)p[1]<<8) | ((ulong)p[2]<<16) | ((ulong)p[3]<<24) |
           ((ulong)p[4]<<32)| ((ulong)p[5]<<40)| ((ulong)p[6]<<48)| ((ulong)p[7]<<56);
}

static uint mem_read32(__global const uchar *scratch, ulong base, ulong addr, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 4u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return 0; }
    __global const uchar *p = scratch + rel;
    return (uint)p[0] | ((uint)p[1]<<8) | ((uint)p[2]<<16) | ((uint)p[3]<<24);
}

static void mem_write64(__global uchar *scratch, ulong base, ulong addr, ulong val, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 8u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return; }
    __global uchar *p = scratch + rel;
    for (int b = 0; b < 8; ++b) { p[b] = (uchar)(val >> (b * 8)); }
}

static void mem_write32(__global uchar *scratch, ulong base, ulong addr, uint val, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 4u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return; }
    __global uchar *p = scratch + rel;
    for (int b = 0; b < 4; ++b) { p[b] = (uchar)(val >> (b * 8)); }
}

static uint compute_flags_add64(ulong a, ulong b, ulong result)
{
    uint f = 0;
    if (result == 0)           f |= FLAG_ZF;
    if (result >> 63)          f |= FLAG_SF;
    if (result < a)            f |= FLAG_CF;
    ulong sa = a >> 63, sb = b >> 63, sr = result >> 63;
    if (sa == sb && sr != sa)  f |= FLAG_OF;
    return f;
}

static uint compute_flags_sub64(ulong a, ulong b, ulong result)
{
    uint f = 0;
    if (result == 0)           f |= FLAG_ZF;
    if (result >> 63)          f |= FLAG_SF;
    if (a < b)                 f |= FLAG_CF;
    ulong sa = a >> 63, sb = b >> 63, sr = result >> 63;
    if (sa != sb && sr != sa)  f |= FLAG_OF;
    return f;
}

static uint compute_flags_logical64(ulong result)
{
    uint f = 0;
    if (result == 0) f |= FLAG_ZF;
    if (result >> 63) f |= FLAG_SF;
    return f;
}

static bool eval_cond(uint flags, uint cond)
{
    bool cf = (flags & FLAG_CF) != 0;
    bool zf = (flags & FLAG_ZF) != 0;
    bool sf = (flags & FLAG_SF) != 0;
    bool of = (flags & FLAG_OF) != 0;
    switch (cond & 0xFu) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xA: return of != sf;
    case 0xB: return of == sf;
    case 0xC: return zf || (of != sf);
    case 0xD: return !zf && (of == sf);
    default:  return false;
    }
}

static uint x86_insn_len(__global const uchar *bytes, uint off, uint max_off)
{
    uint start = off;
    bool has66 = false, has67 = false, rexW = false;
    uchar b;
    for (int p = 0; p < 5 && off < max_off; ++p) {
        b = bytes[off];
        if (b==0x66) { has66=true; ++off; }
        else if (b==0x67) { has67=true; ++off; }
        else if (b==0xF0||b==0xF2||b==0xF3) { ++off; }
        else if (b==0x26||b==0x2E||b==0x36||b==0x3E||b==0x64||b==0x65) { ++off; }
        else break;
    }
    if (off >= max_off) return 0;
    b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) { rexW = (b & 0x08) != 0; ++off; }
    if (off >= max_off) return 0;
    uchar opc = bytes[off]; ++off;
    if (off > max_off) return 0;
    bool twoB = false; uchar opc2 = 0;
    if (opc == 0x0F && off < max_off) { opc2 = bytes[off]; ++off; twoB = true;
        if ((opc2==0x38||opc2==0x3A) && off < max_off) ++off; }
    bool hasM = false; int immB = 0;
    if (!twoB) {
        if (opc>=0x50&&opc<=0x5F) {}
        else if (opc>=0x90&&opc<=0x97) {}
        else if (opc==0x98||opc==0x99||opc==0x9B||opc==0x9C||opc==0x9D||opc==0x9E||opc==0x9F) {}
        else if (opc==0xC3||opc==0xCB||opc==0xC9||opc==0xCC||opc==0xCE||opc==0xCF) {}
        else if (opc==0xC2||opc==0xCA) { immB=2; }
        else if (opc==0xC8) { immB=3; }
        else if (opc==0xCD) { immB=1; }
        else if (opc>=0xF8&&opc<=0xFD) {}
        else if (opc==0xF4||opc==0xF5||opc==0xD7) {}
        else if (opc==0xEC||opc==0xED||opc==0xEE||opc==0xEF) {}
        else if (opc==0xE4||opc==0xE5||opc==0xE6||opc==0xE7) { immB=1; }
        else if ((opc>=0x70&&opc<=0x7F)||opc==0xEB||opc==0xE3) { immB=1; }
        else if (opc==0xE0||opc==0xE1||opc==0xE2) { immB=1; }
        else if (opc==0xE9||opc==0xE8) { immB=4; }
        else if (opc>=0xB0&&opc<=0xB7) { immB=1; }
        else if (opc>=0xB8&&opc<=0xBF) { immB=rexW?8:(has66?2:4); }
        else if (opc>=0xA0&&opc<=0xA3) { immB=has67?4:8; }
        else if (opc==0xA8) { immB=1; }
        else if (opc==0xA9) { immB=rexW?4:(has66?2:4); }
        else if (opc>=0xA4&&opc<=0xAF) {}
        else if (opc==0x05||opc==0x0D||opc==0x15||opc==0x1D||
                 opc==0x25||opc==0x2D||opc==0x35||opc==0x3D) { immB=rexW?4:(has66?2:4); }
        else if (opc==0x04||opc==0x0C||opc==0x14||opc==0x1C||
                 opc==0x24||opc==0x2C||opc==0x34||opc==0x3C) { immB=1; }
        else if (opc==0x68) { immB=has66?2:4; }
        else if (opc==0x6A) { immB=1; }
        else if (opc==0x83) { hasM=true; immB=1; }
        else if (opc==0x81) { hasM=true; immB=rexW?4:(has66?2:4); }
        else if (opc==0xC0||opc==0xC1) { hasM=true; immB=1; }
        else if (opc==0xD0||opc==0xD1||opc==0xD2||opc==0xD3) { hasM=true; }
        else if (opc==0xC6) { hasM=true; immB=1; }
        else if (opc==0xC7) { hasM=true; immB=rexW?4:(has66?2:4); }
        else if (opc==0x6B) { hasM=true; immB=1; }
        else if (opc==0x69) { hasM=true; immB=has66?2:4; }
        else { hasM=true; }
    } else {
        if (opc2>=0x80&&opc2<=0x8F) { immB=4; }
        else if (opc2==0xA4||opc2==0xAC) { hasM=true; immB=1; }
        else if (opc2==0x1F||opc2==0x18) { hasM=true; }
        else if (opc2==0xA2||opc2==0x05||opc2==0x07||opc2==0x31||opc2==0x34||opc2==0x35) {}
        else { hasM=true; }
    }
    if (hasM) {
        if (off >= max_off) return 0;
        uchar modrm = bytes[off]; ++off;
        uint mod = (uint)(modrm>>6), rm = (uint)(modrm&7u);
        uint disp = (mod==1u)?1u:(mod==2u)?4u:(mod==0u&&rm==5u)?4u:0u;
        if (!has67 && mod!=3u && rm==4u) {
            if (off >= max_off) return 0;
            uchar sib = bytes[off]; ++off;
            if ((sib&7u)==5u && mod==0u) disp=4u;
        }
        off += disp;
    }
    off += (uint)immB;
    if (off > max_off || off > start+15u) return 0;
    return off - start;
}

static uint exec_one(__private X86State *st,
                     __global const uchar *func,
                     uint func_size,
                     __global uchar *scratch)
{
    uint ip = st->rip;
    if (ip >= func_size) return FAULT_GENERAL;

    bool has_rep = false, has_repz = false, has_66 = false, has_67 = false;
    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;

    while (ip < func_size) {
        uchar b = func[ip];
        if (b == 0xF3) { has_repz = true; ++ip; }
        else if (b == 0xF2) { has_rep = true; ++ip; }
        else if (b == 0xF0) { ++ip; }
        else if (b == 0x66) { has_66 = true; ++ip; }
        else if (b == 0x67) { has_67 = true; ++ip; }
        else if (b == 0x26||b == 0x2E||b == 0x36||b == 0x3E||b == 0x64||b == 0x65) { ++ip; }
        else break;
    }
    if (ip >= func_size) return FAULT_GENERAL;

    uchar b = func[ip];
    if (b >= 0x40 && b <= 0x4F) {
        rex_w = (b & 0x08) != 0; rex_r = (b & 0x04) != 0;
        rex_x = (b & 0x02) != 0; rex_b = (b & 0x01) != 0;
        ++ip;
    }
    if (ip >= func_size) return FAULT_GENERAL;

    uint start_ip = st->rip;
    uchar opc = func[ip]; ++ip;

    #define REG(r)   (st->regs[(r) + (rex_r ? 8u : 0u)])
    #define RMREG(r) (st->regs[(r) + (rex_b ? 8u : 0u)])
    #define READ_IMM8  ((ip < func_size) ? (long)(schar)(func[ip++]) : 0L)
    #define READ_IMM32 ((ip+3u < func_size) ? \
        (long)(int)((uint)func[ip]|((uint)func[ip+1]<<8)|((uint)func[ip+2]<<16)|((uint)func[ip+3]<<24)) \
        : (ip+=4u, 0L))
    #define READ_UIMM32 ((ip+3u < func_size) ? \
        ((uint)func[ip]|((uint)func[ip+1]<<8)|((uint)func[ip+2]<<16)|((uint)func[ip+3]<<24)) \
        : (ip+=4u, 0u))

    /* Two-byte opcodes */
    if (opc == 0x0F && ip < func_size) {
        uchar opc2 = func[ip++];
        /* Near Jcc 0F 80-8F */
        if (opc2 >= 0x80 && opc2 <= 0x8F && ip + 3u < func_size) {
            long rel = READ_IMM32;
            if (eval_cond(st->flags, opc2 & 0xFu))
                st->rip = (uint)((int)ip + (int)rel);
            else
                st->rip = ip;
            return NO_FAULT;
        }
        /* RDTSC: 0F 31 */
        if (opc2 == 0x31) {
            static uint rdtsc_ctr = 0u;
            rdtsc_ctr += 1000u;
            st->regs[REG_RAX] = (ulong)(rdtsc_ctr & 0xFFFFFFFFu);
            st->regs[REG_RDX] = (ulong)(rdtsc_ctr >> 20);
            st->rip = ip; return NO_FAULT;
        }
        /* CPUID: 0F A2 */
        if (opc2 == 0xA2) {
            uint leaf = (uint)st->regs[REG_RAX];
            if (leaf == 0u) { st->regs[REG_RAX]=1u; st->regs[REG_RBX]=0x756e6547u; st->regs[REG_RDX]=0x49656e69u; st->regs[REG_RCX]=0x6c65746eu; }
            else if (leaf == 1u) { st->regs[REG_RAX]=0x000906E9u; st->regs[REG_RBX]=0u; st->regs[REG_RCX]=0u; st->regs[REG_RDX]=0u; }
            else { st->regs[REG_RAX]=0u; st->regs[REG_RBX]=0u; st->regs[REG_RCX]=0u; st->regs[REG_RDX]=0u; }
            st->rip = ip; return NO_FAULT;
        }
        /* MOVZX r32/64, r/m8: 0F B6 ; MOVZX r32/64, r/m16: 0F B7 */
        if ((opc2 == 0xB6 || opc2 == 0xB7) && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint src = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                if (opc2 == 0xB6) st->regs[dst] = st->regs[src] & 0xFFu;
                else              st->regs[dst] = st->regs[src] & 0xFFFFu;
            }
            st->rip = ip; return NO_FAULT;
        }
        /* MOVSX r64, r/m32: 0F BE/BF */
        if ((opc2 == 0xBE || opc2 == 0xBF) && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint src = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                if (opc2 == 0xBE) st->regs[dst] = (ulong)(long)(schar)(uchar)st->regs[src];
                else              st->regs[dst] = (ulong)(long)(short)(ushort)st->regs[src];
            }
            st->rip = ip; return NO_FAULT;
        }
        /* IMUL r, r/m: 0F AF */
        if (opc2 == 0xAF && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint src = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                st->regs[dst] = (ulong)((long)st->regs[dst] * (long)st->regs[src]);
            }
            st->rip = ip; return NO_FAULT;
        }
        /* BSF/BSR: 0F BC/BD */
        if ((opc2 == 0xBC || opc2 == 0xBD) && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint src = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                ulong v = st->regs[src];
                if (v == 0) { st->flags |= FLAG_ZF; }
                else {
                    st->flags &= ~FLAG_ZF;
                    uint pos = 0;
                    if (opc2 == 0xBC) { while (!(v & 1u)) { v >>= 1; ++pos; } }
                    else              { while (!(v >> 63)) { v <<= 1; ++pos; } pos = 63u - pos; }
                    st->regs[dst] = pos;
                }
            }
            st->rip = ip; return NO_FAULT;
        }
        /* POPCNT: F3 0F B8 */
        if (opc2 == 0xB8 && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint src = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                ulong v = st->regs[src];
                uint cnt = 0;
                while (v) { cnt += (uint)(v&1u); v>>=1; }
                st->regs[dst] = cnt;
            }
            st->rip = ip; return NO_FAULT;
        }
        /* CMOVcc: 0F 40-4F */
        if (opc2 >= 0x40 && opc2 <= 0x4F && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint src = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u && eval_cond(st->flags, opc2 & 0xFu)) {
                st->regs[dst] = st->regs[src];
            }
            st->rip = ip; return NO_FAULT;
        }
        /* SETcc: 0F 90-9F */
        if (opc2 >= 0x90 && opc2 <= 0x9F && ip < func_size) {
            uchar modrm = func[ip++];
            uint dst = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                st->regs[dst] = (st->regs[dst] & ~0xFFuL) | (eval_cond(st->flags, opc2&0xFu) ? 1u : 0u);
            }
            st->rip = ip; return NO_FAULT;
        }
        /* XCHG: 87 /r (two-byte form handled elsewhere) */
        /* BT/BTC/BTR/BTS: 0F A3/AB/B3/BB */
        if ((opc2==0xA3||opc2==0xAB||opc2==0xB3||opc2==0xBB) && ip < func_size) {
            uchar modrm = func[ip++];
            uint bit_reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint dst = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                uint bit = (uint)st->regs[bit_reg] & 63u;
                ulong mask = 1uL << bit;
                bool old = (st->regs[dst] & mask) != 0;
                if (old) st->flags |= FLAG_CF; else st->flags &= ~FLAG_CF;
                if (opc2==0xAB) st->regs[dst] |= mask;
                else if (opc2==0xB3) st->regs[dst] &= ~mask;
                else if (opc2==0xBB) st->regs[dst] ^= mask;
            }
            st->rip = ip; return NO_FAULT;
        }
        /* Fall through: skip with length decoder */
        st->rip = start_ip;
        uint len2 = x86_insn_len(func, start_ip, func_size);
        st->rip = (len2 > 0) ? start_ip + len2 : start_ip + 1u;
        return NO_FAULT;
    }

    /* NOP: 90 */
    if (opc == 0x90) { st->rip = ip; return NO_FAULT; }

    /* RET near/far: C3/CB */
    if (opc == 0xC3 || opc == 0xCB) {
        if (st->call_depth > 0u) {
            --st->call_depth;
            uint fault = NO_FAULT;
            ulong ra = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
            st->regs[REG_RSP] += 8u;
            st->rip = (uint)ra;
            return fault;
        }
        st->halted = FAULT_RET; return FAULT_RET;
    }

    /* JMP rel32: E9 ; JMP rel8: EB */
    if (opc == 0xE9) { long rel = READ_IMM32; st->rip = (uint)((int)ip + (int)rel); return NO_FAULT; }
    if (opc == 0xEB) { long rel = READ_IMM8;  st->rip = (uint)((int)ip + (int)rel); return NO_FAULT; }

    /* Short Jcc: 70-7F */
    if (opc >= 0x70 && opc <= 0x7F) {
        long rel = READ_IMM8;
        st->rip = eval_cond(st->flags, opc & 0xFu) ? (uint)((int)ip + (int)rel) : ip;
        return NO_FAULT;
    }

    /* CALL rel32: E8 */
    if (opc == 0xE8) {
        long rel = READ_IMM32;
        uint target = (uint)((int)ip + (int)rel);
        uint fault = NO_FAULT;
        st->regs[REG_RSP] -= 8u;
        mem_write64(scratch, st->scratch_base, st->regs[REG_RSP], (ulong)ip, &fault);
        if (target < func_size && st->call_depth < MAX_CALL_DEPTH) {
            ++st->call_depth; st->rip = target; return fault;
        }
        ulong ra = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
        st->regs[REG_RSP] += 8u; st->regs[REG_RAX] = 0; st->rip = (uint)ra;
        return fault;
    }

    /* LOOP/LOOPE/LOOPNE: E2/E1/E0 ; JECXZ: E3 */
    if (opc == 0xE2 || opc == 0xE1 || opc == 0xE0 || opc == 0xE3) {
        long rel = READ_IMM8;
        bool taken = false;
        if (opc == 0xE3) { taken = (st->regs[REG_RCX] == 0u); }
        else {
            --st->regs[REG_RCX];
            bool nz = st->regs[REG_RCX] != 0u;
            if (opc == 0xE2) taken = nz;
            else if (opc == 0xE1) taken = nz && (st->flags & FLAG_ZF);
            else taken = nz && !(st->flags & FLAG_ZF);
        }
        st->rip = taken ? (uint)((int)ip + (int)rel) : ip;
        return NO_FAULT;
    }

    /* PUSH r64: 50-57 ; POP r64: 58-5F */
    if (opc >= 0x50 && opc <= 0x57) {
        uint reg = (opc&7u) + (rex_b?8u:0u); uint fault = NO_FAULT;
        st->regs[REG_RSP] -= 8u;
        mem_write64(scratch, st->scratch_base, st->regs[REG_RSP], st->regs[reg], &fault);
        st->rip = ip; return fault;
    }
    if (opc >= 0x58 && opc <= 0x5F) {
        uint reg = (opc&7u) + (rex_b?8u:0u); uint fault = NO_FAULT;
        st->regs[reg] = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
        st->regs[REG_RSP] += 8u;
        st->rip = ip; return fault;
    }

    /* INC r64: 40-47 (non-REX) ; DEC r64: 48-4F (non-REX) — these are now REX prefixes in 64-bit mode, skip */

    /* MOV r64, imm64: B8-BF */
    if (opc >= 0xB8 && opc <= 0xBF) {
        uint reg = (opc&7u) + (rex_b?8u:0u);
        if (rex_w) {
            ulong v = 0;
            for (int bb = 0; bb < 8 && ip+(uint)bb < func_size; ++bb) v |= (ulong)func[ip+bb] << (bb*8);
            ip += 8u; st->regs[reg] = v;
        } else {
            uint v = READ_UIMM32; st->regs[reg] = (ulong)v;
        }
        st->rip = ip; return NO_FAULT;
    }

    /* MOV r8, imm8: B0-B7 */
    if (opc >= 0xB0 && opc <= 0xB7) {
        uint reg = opc & 7u;
        if (ip < func_size) { st->regs[reg] = (st->regs[reg] & ~0xFFuL) | func[ip++]; }
        st->rip = ip; return NO_FAULT;
    }

    /* XCHG rAX, r: 91-97 */
    if (opc >= 0x91 && opc <= 0x97) {
        uint reg = (opc&7u) + (rex_b?8u:0u);
        ulong tmp = st->regs[REG_RAX]; st->regs[REG_RAX] = st->regs[reg]; st->regs[reg] = tmp;
        st->rip = ip; return NO_FAULT;
    }

    /* LEAVE: C9 */
    if (opc == 0xC9) {
        st->regs[REG_RSP] = st->regs[REG_RBP];
        uint fault = NO_FAULT;
        st->regs[REG_RBP] = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
        st->regs[REG_RSP] += 8u;
        st->rip = ip; return fault;
    }

    /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP rAX, imm */
    if ((opc & 7u) == 5u && opc <= 0x3D) {
        uint grp = (opc >> 3) & 7u;
        long imm = READ_IMM32;
        ulong a = st->regs[REG_RAX], bv = (ulong)imm, r = 0;
        switch (grp) {
        case 0: r=a+bv; st->regs[REG_RAX]=r; st->flags=compute_flags_add64(a,bv,r); break;
        case 1: r=a|bv; st->regs[REG_RAX]=r; st->flags=compute_flags_logical64(r); break;
        case 2: r=a+bv+((st->flags&FLAG_CF)?1u:0u); st->regs[REG_RAX]=r; break;
        case 3: r=a-bv-((st->flags&FLAG_CF)?1u:0u); st->regs[REG_RAX]=r; break;
        case 4: r=a&bv; st->regs[REG_RAX]=r; st->flags=compute_flags_logical64(r); break;
        case 5: r=a-bv; st->regs[REG_RAX]=r; st->flags=compute_flags_sub64(a,bv,r); break;
        case 6: r=a^bv; st->regs[REG_RAX]=r; st->flags=compute_flags_logical64(r); break;
        case 7: r=a-bv; st->flags=compute_flags_sub64(a,bv,r); break;
        }
        st->rip = ip; return NO_FAULT;
    }

    /* TEST AL/AX/EAX/RAX, imm */
    if (opc == 0xA8) {
        if (ip < func_size) { ulong r = st->regs[REG_RAX] & func[ip++]; st->flags=compute_flags_logical64(r); }
        st->rip = ip; return NO_FAULT;
    }
    if (opc == 0xA9) {
        long imm = READ_IMM32; ulong r = st->regs[REG_RAX] & (ulong)imm;
        st->flags = compute_flags_logical64(r); st->rip = ip; return NO_FAULT;
    }

    /* CBW/CWDE/CDQE: 98 ; CWD/CDQ/CQO: 99 */
    if (opc == 0x98) {
        if (rex_w) st->regs[REG_RAX] = (ulong)(long)(int)(uint)(st->regs[REG_RAX] & 0xFFFFFFFFu);
        else       st->regs[REG_RAX] = (ulong)(long)(short)(ushort)(st->regs[REG_RAX] & 0xFFFFu);
        st->rip = ip; return NO_FAULT;
    }
    if (opc == 0x99) {
        if (rex_w) st->regs[REG_RDX] = (st->regs[REG_RAX] >> 63) ? 0xFFFFFFFFFFFFFFFFuL : 0uL;
        else       st->regs[REG_RDX] = (st->regs[REG_RAX] >> 31) ? 0xFFFFFFFFu : 0u;
        st->rip = ip; return NO_FAULT;
    }

    /* INC r64: FF /0 ; DEC r64: FF /1 ; CALL r/m: FF /2 ; JMP r/m: FF /4 ; PUSH r/m: FF /6 */
    if (opc == 0xFF && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint mod = (uint)(modrm>>6);
        if (mod == 3u) {
            switch (reg_op) {
            case 0: { ulong a=st->regs[rm],r=a+1u; st->regs[rm]=r;
                      st->flags=(st->flags&FLAG_CF)|compute_flags_add64(a,1u,r); break; }
            case 1: { ulong a=st->regs[rm],r=a-1u; st->regs[rm]=r;
                      st->flags=(st->flags&FLAG_CF)|compute_flags_sub64(a,1u,r); break; }
            case 2: { ulong target=st->regs[rm]; uint fault=NO_FAULT;
                      st->regs[REG_RSP]-=8u;
                      mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],(ulong)ip,&fault);
                      if (target < func_size && st->call_depth < MAX_CALL_DEPTH) {
                          ++st->call_depth; st->rip=(uint)target; return fault;
                      }
                      ulong ra=mem_read64(scratch,st->scratch_base,st->regs[REG_RSP],&fault);
                      st->regs[REG_RSP]+=8u; st->regs[REG_RAX]=0; st->rip=(uint)ra; return fault; }
            case 4: st->rip=(uint)st->regs[rm]; return NO_FAULT;
            case 6: { uint fault=NO_FAULT; st->regs[REG_RSP]-=8u;
                      mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],st->regs[rm],&fault);
                      st->rip=ip; return fault; }
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* Group 80/81/83 */
    if ((opc == 0x80 || opc == 0x81 || opc == 0x83) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        long imm;
        if (opc == 0x80 || opc == 0x83) imm = READ_IMM8;
        else imm = READ_IMM32;
        if ((modrm>>6)==3u) {
            ulong a = st->regs[rm], bv = (ulong)imm, r = 0;
            switch (reg_op) {
            case 0: r=a+bv; st->regs[rm]=r; st->flags=compute_flags_add64(a,bv,r); break;
            case 1: r=a|bv; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 4: r=a&bv; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 5: r=a-bv; st->regs[rm]=r; st->flags=compute_flags_sub64(a,bv,r); break;
            case 6: r=a^bv; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 7: r=a-bv; st->flags=compute_flags_sub64(a,bv,r); break;
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* MOV r/m, r and r, r/m: 88-8B */
    if (opc >= 0x88 && opc <= 0x8B && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint src = (uint)(modrm&7u)      + (rex_b?8u:0u);
        if ((modrm>>6)==3u) {
            bool toMem = (opc==0x88||opc==0x89);
            if (toMem) st->regs[src] = st->regs[dst];
            else       st->regs[dst] = st->regs[src];
        }
        st->rip = ip; return NO_FAULT;
    }

    /* MOV r/m64, imm32: C7 /0 */
    if (opc == 0xC7 && ip < func_size) {
        uchar modrm = func[ip++];
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        long imm = READ_IMM32;
        if ((modrm>>6)==3u) st->regs[rm] = (ulong)imm;
        st->rip = ip; return NO_FAULT;
    }

    /* LEA r, m: 8D */
    if (opc == 0x8D && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint base = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint mod = modrm>>6;
        long disp = 0;
        if (mod==1u) disp = READ_IMM8;
        else if (mod==2u) disp = READ_IMM32;
        st->regs[dst] = st->regs[base] + (ulong)disp;
        st->rip = ip; return NO_FAULT;
    }

    /* IMUL r, r/m, imm: 6B/69 */
    if ((opc == 0x6B || opc == 0x69) && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint src = (uint)(modrm&7u)      + (rex_b?8u:0u);
        long imm = (opc==0x6B) ? READ_IMM8 : READ_IMM32;
        if ((modrm>>6)==3u) st->regs[dst] = (ulong)((long)st->regs[src] * imm);
        st->rip = ip; return NO_FAULT;
    }

    /* ADD/SUB/AND/OR/XOR/CMP/MOV/XCHG r/m,r: 00-3F, 86/87 */
    if (opc <= 0x3F && (opc&7u) <= 3u && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint src = (uint)(modrm&7u)      + (rex_b?8u:0u);
        uint grp = (opc>>3)&7u;
        if ((modrm>>6)==3u) {
            ulong a = st->regs[src], b2 = st->regs[dst], r = 0;
            bool toReg = (opc&3u)>=2u;
            if (toReg) { a=st->regs[dst]; b2=st->regs[src]; }
            switch(grp){
            case 0: r=a+b2; st->regs[toReg?dst:src]=r; st->flags=compute_flags_add64(a,b2,r); break;
            case 1: r=a|b2; st->regs[toReg?dst:src]=r; st->flags=compute_flags_logical64(r); break;
            case 4: r=a&b2; st->regs[toReg?dst:src]=r; st->flags=compute_flags_logical64(r); break;
            case 5: r=a-b2; st->regs[toReg?dst:src]=r; st->flags=compute_flags_sub64(a,b2,r); break;
            case 6: r=a^b2; st->regs[toReg?dst:src]=r; st->flags=compute_flags_logical64(r); break;
            case 7: r=a-b2; st->flags=compute_flags_sub64(a,b2,r); break;
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* XCHG r/m, r: 86/87 */
    if ((opc==0x86||opc==0x87) && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint src = (uint)(modrm&7u)      + (rex_b?8u:0u);
        if ((modrm>>6)==3u) { ulong tmp=st->regs[dst]; st->regs[dst]=st->regs[src]; st->regs[src]=tmp; }
        st->rip = ip; return NO_FAULT;
    }

    /* F6/F7 group: TEST, NOT, NEG, MUL, IMUL, DIV, IDIV */
    if ((opc == 0xF6 || opc == 0xF7) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        if ((modrm>>6)==3u) {
            ulong a = st->regs[rm];
            switch(reg_op) {
            case 0: case 1: { long imm=(opc==0xF6)?READ_IMM8:READ_IMM32; ulong r=a&(ulong)imm; st->flags=compute_flags_logical64(r); break; }
            case 2: { st->regs[rm] = ~a; break; }
            case 3: { ulong r=(ulong)(-(long)a); st->regs[rm]=r; st->flags=compute_flags_sub64(0,a,r); break; }
            case 4: { ulong r=st->regs[REG_RAX]*a; st->regs[REG_RAX]=r&0xFFFFFFFFFFFFFFFFuL; st->regs[REG_RDX]=0; break; }
            case 5: { ulong r=(ulong)((long)st->regs[REG_RAX]*(long)a); st->regs[REG_RAX]=r; st->regs[REG_RDX]=(ulong)((long)r>>63); break; }
            case 6: if (a!=0) { st->regs[REG_RDX]=st->regs[REG_RAX]%a; st->regs[REG_RAX]=st->regs[REG_RAX]/a; } break;
            case 7: if (a!=0) { long q=(long)st->regs[REG_RAX]/(long)a; st->regs[REG_RAX]=(ulong)q; st->regs[REG_RDX]=(ulong)((long)st->regs[REG_RAX]%(long)a); } break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* D0/D1/D2/D3/C0/C1: shifts/rotates */
    if ((opc==0xD0||opc==0xD1||opc==0xD2||opc==0xD3||opc==0xC0||opc==0xC1) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint cnt = 1u;
        if (opc==0xC0||opc==0xC1) cnt = (uint)(uchar)READ_IMM8;
        else if (opc==0xD2||opc==0xD3) cnt = (uint)st->regs[REG_RCX] & 63u;
        if ((modrm>>6)==3u) {
            ulong a = st->regs[rm], r = 0;
            switch(reg_op){
            case 4: r=a<<cnt; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 5: r=a>>cnt; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 7: r=(ulong)((long)a>>(int)cnt); st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 0: r=(a<<cnt)|(a>>(64u-cnt)); st->regs[rm]=r; break;
            case 1: r=(a>>cnt)|(a<<(64u-cnt)); st->regs[rm]=r; break;
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* INT/HLT */
    if (opc == 0xCC || opc == 0xCE) { st->halted = FAULT_GENERAL; st->rip = ip; return FAULT_GENERAL; }
    if (opc == 0xCD) { ++ip; st->halted = FAULT_GENERAL; st->rip = ip; return FAULT_GENERAL; }
    if (opc == 0xF4) { st->halted = FAULT_GENERAL; st->rip = ip; return FAULT_GENERAL; }

    /* PUSH imm32/imm8: 68/6A */
    if (opc == 0x68) { long imm = READ_IMM32; uint fault=NO_FAULT;
        st->regs[REG_RSP]-=8u; mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],(ulong)imm,&fault);
        st->rip=ip; return fault; }
    if (opc == 0x6A) { long imm = READ_IMM8;  uint fault=NO_FAULT;
        st->regs[REG_RSP]-=8u; mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],(ulong)imm,&fault);
        st->rip=ip; return fault; }

    /* String ops: A4-AF (step over) */
    if (opc >= 0xA4 && opc <= 0xAF) { st->rip = ip; return NO_FAULT; }

    /* Flag ops: F8-FD */
    if (opc >= 0xF8 && opc <= 0xFD) {
        if (opc==0xF8) st->flags &= ~FLAG_CF;
        else if (opc==0xF9) st->flags |= FLAG_CF;
        st->rip = ip; return NO_FAULT;
    }

    /* TEST r/m, r: 84/85 */
    if ((opc==0x84||opc==0x85) && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint src = (uint)(modrm&7u)      + (rex_b?8u:0u);
        if ((modrm>>6)==3u) { ulong r=st->regs[dst]&st->regs[src]; st->flags=compute_flags_logical64(r); }
        st->rip = ip; return NO_FAULT;
    }

    /* CMP r/m, r and r, r/m: 38-3F */
    if (opc >= 0x38 && opc <= 0x3F && ip < func_size) {
        uchar modrm = func[ip++];
        uint dst = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint src = (uint)(modrm&7u)      + (rex_b?8u:0u);
        if ((modrm>>6)==3u) {
            ulong a = (opc&2u) ? st->regs[dst] : st->regs[src];
            ulong b2 = (opc&2u) ? st->regs[src] : st->regs[dst];
            ulong r = a-b2; st->flags=compute_flags_sub64(a,b2,r);
        }
        st->rip = ip; return NO_FAULT;
    }

    /* Default: use length decoder to skip */
    uint len = x86_insn_len(func, start_ip, func_size);
    if (len == 0) { st->rip = start_ip + 1u; return FAULT_UNSUPPORTED; }
    st->rip = start_ip + len;
    return NO_FAULT;

    #undef REG
    #undef RMREG
    #undef READ_IMM8
    #undef READ_IMM32
    #undef READ_UIMM32
}

__kernel void retdec_semantic_hash(
    __global const uchar  *func_bytes,
    __global const uint   *func_byte_offset,
    __global const uint   *func_byte_size,
    __global const ulong  *test_inputs,
    __global       uchar  *scratch,
    __global       ulong  *result_hashes,
    __global       uint   *exec_status)
{
    uint gid = (uint)get_global_id(0);

    uint func_off = func_byte_offset[gid];
    uint func_sz  = func_byte_size[gid];
    __global const uchar *func = func_bytes + func_off;
    __global uchar *my_scratch = scratch + gid * SCRATCH_SIZE;
    ulong scratch_base = (ulong)gid * SCRATCH_SIZE;

    X86State st;
    for (int r = 0; r < 16; ++r) st.regs[r] = 0;
    st.flags       = 0;
    st.halted      = 0;
    st.call_depth  = 0;
    st.scratch_base = scratch_base;

    st.regs[REG_RSP] = scratch_base + (ulong)(SCRATCH_SIZE - 128u);

    __global const ulong *inputs = test_inputs + gid * 16u;
    st.regs[REG_RDI] = inputs[0];
    st.regs[REG_RSI] = inputs[1];
    st.regs[REG_RDX] = inputs[2];
    st.regs[REG_RCX] = inputs[3];
    st.regs[8]       = inputs[4];
    st.regs[9]       = inputs[5];
    st.regs[REG_RAX] = inputs[6];

    st.rip = 0;

    for (uint step = 0; step < MAX_STEPS && !st.halted && st.rip < func_sz; ++step) {
        uint fault = exec_one(&st, func, func_sz, my_scratch);
        if (fault == FAULT_RET) break;
        if (fault == FAULT_GENERAL) { st.halted = FAULT_GENERAL; break; }
    }

    if (!st.halted && st.rip >= func_sz) {
        st.halted = FAULT_STEPLIMIT;
    }

    ulong hash = 14695981039346656037uL;
    for (int i = 0; i < 8; ++i) hash = fnv1a64_step(hash, inputs[i]);
    hash = fnv1a64_step(hash, st.regs[REG_RAX]);
    hash = fnv1a64_step(hash, st.regs[REG_RCX]);
    hash = fnv1a64_step(hash, st.regs[REG_RDX]);
    hash = fnv1a64_step(hash, st.regs[REG_RSI]);
    hash = fnv1a64_step(hash, st.regs[REG_RDI]);

    result_hashes[gid] = hash;
    exec_status[gid]   = st.halted;
}
)SEMHASH";
}

const char *egraphSimplifyClSource()
{
    // Full equality-saturation kernel — embedded verbatim from kernels/egraph_simplify.cl.
    return R"EGRAPH(
#define ENOP      0u
#define ELIT      1u
#define EVAR      2u
#define EADD      3u
#define ESUB      4u
#define EMUL      5u
#define EDIV      6u
#define EAND      7u
#define EOR       8u
#define EXOR      9u
#define ESHL      10u
#define ESHR      11u
#define EASHR     12u
#define ECAST     13u
#define EDEREF    14u
#define EARRAY    15u
#define EFIELD    16u
#define EBITFIELD 17u

#define NO_CLASS  0xFFFFFFFFu
#define MAX_SCORE 99u

static uint cdist(uint opcode)
{
    switch (opcode) {
        case ELIT:      return 0u;
        case EVAR:      return 0u;
        case EARRAY:    return 1u;
        case EFIELD:    return 1u;
        case EBITFIELD: return 2u;
        case EADD:      return 3u;
        case ESUB:      return 3u;
        case EDEREF:    return 3u;
        case EMUL:      return 4u;
        case EDIV:      return 4u;
        case ECAST:     return 5u;
        case EAND:      return 6u;
        case EOR:       return 6u;
        case EXOR:      return 6u;
        case ESHL:      return 6u;
        case ESHR:      return 6u;
        case EASHR:     return 6u;
        default:        return MAX_SCORE;
    }
}

static uint uf_find(__global uint *parent, uint x)
{
    while (parent[x] != x) {
        uint gp = parent[parent[x]];
        parent[x] = gp;
        x = gp;
    }
    return x;
}

static bool uf_union(__global uint *parent, __global uint *rank, uint ra, uint rb)
{
    ra = uf_find(parent, ra);
    rb = uf_find(parent, rb);
    if (ra == rb) return false;
    if (rank[ra] < rank[rb]) { uint t = ra; ra = rb; rb = t; }
    parent[rb] = ra;
    if (rank[ra] == rank[rb]) rank[ra]++;
    return true;
}

static bool class_literal(__global const uint  *op,
                          __global const uint  *eclass,
                          __global const ulong *lit,
                          __global       uint  *parent,
                          uint n_nodes, uint cls, __private ulong *val)
{
    uint root = uf_find(parent, cls);
    for (uint i = 0u; i < n_nodes; i++) {
        if (op[i] == ELIT && uf_find(parent, eclass[i]) == root) {
            *val = lit[i];
            return true;
        }
    }
    return false;
}

__kernel void retdec_egraph_saturate(
    __global       uint  *op,
    __global       uint  *eclass,
    __global       uint  *lhs,
    __global       uint  *rhs,
    __global       uint  *aux,
    __global       ulong *lit,
    __global       uint  *uf_parent,
    __global       uint  *uf_rank,
    __global       uint  *dirty,
                   uint   n_nodes,
                   uint   n_classes)
{
    uint id = (uint)get_global_id(0);
    if (id >= n_nodes) return;

    uint myop  = op[id];
    uint mycls = uf_find(uf_parent, eclass[id]);
    uint l     = (lhs[id] != NO_CLASS) ? uf_find(uf_parent, lhs[id]) : NO_CLASS;
    uint r     = (rhs[id] != NO_CLASS) ? uf_find(uf_parent, rhs[id]) : NO_CLASS;
    uint myaux = aux[id];
    bool fired = false;

    /* Rule 1: x + 0 -> x, x - 0 -> x, x | 0 -> x, x ^ 0 -> x */
    if ((myop == EADD || myop == ESUB || myop == EOR || myop == EXOR)
        && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0UL)
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
    }
    /* x & ~0 -> x */
    if (myop == EAND && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0xFFFFFFFFFFFFFFFFUL)
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
    }
    /* Rule 2: x * 1 -> x */
    if (myop == EMUL && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0, lv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 1UL)
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, l, &lv) && lv == 1UL)
            fired |= uf_union(uf_parent, uf_rank, mycls, r);
    }
    /* Rule 3: x * 0 -> 0 */
    if (myop == EMUL && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0, lv = 0;
        bool rz = class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0UL;
        bool lz = class_literal(op, eclass, lit, uf_parent, n_nodes, l, &lv) && lv == 0UL;
        if (rz || lz) {
            for (uint i = 0u; i < n_nodes; i++) {
                if (op[i] == ELIT && lit[i] == 0UL) {
                    fired |= uf_union(uf_parent, uf_rank, mycls, uf_find(uf_parent, eclass[i]));
                    break;
                }
            }
        }
    }
    /* Rule 4: x >> 0 -> x, x << 0 -> x */
    if ((myop == ESHL || myop == ESHR || myop == EASHR)
        && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0UL)
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
    }
    /* Rule 5: (x >> k) & mask -> EBITFIELD(x, k, popcount(mask)) */
    if (myop == EAND && l != NO_CLASS && r != NO_CLASS) {
        for (uint si = 0u; si < n_nodes; si++) {
            if ((op[si] == ESHR || op[si] == EASHR)
                && uf_find(uf_parent, eclass[si]) == l
                && lhs[si] != NO_CLASS && rhs[si] != NO_CLASS) {
                ulong k_val = 0, mask_val = 0;
                bool have_k    = class_literal(op, eclass, lit, uf_parent, n_nodes, uf_find(uf_parent, rhs[si]), &k_val);
                bool have_mask = class_literal(op, eclass, lit, uf_parent, n_nodes, r, &mask_val);
                if (have_k && have_mask && mask_val != 0UL) {
                    ulong m = mask_val >> (uint)k_val;
                    bool contiguous = (m != 0UL) && ((m & (m + 1UL)) == 0UL);
                    if (contiguous) {
                        uint src_cls = uf_find(uf_parent, lhs[si]);
                        op[id]  = EBITFIELD;
                        lhs[id] = src_cls;
                        rhs[id] = NO_CLASS;
                        aux[id] = (uint)k_val;
                        fired = true;
                    }
                }
                break;
            }
        }
    }
    /* Rule 6: CAST(CAST(x, w1), w2) -> CAST(x, w2) when w2 <= w1 */
    if (myop == ECAST && l != NO_CLASS) {
        for (uint ci = 0u; ci < n_nodes; ci++) {
            if (op[ci] == ECAST && uf_find(uf_parent, eclass[ci]) == l && lhs[ci] != NO_CLASS) {
                if (myaux <= aux[ci]) { lhs[id] = lhs[ci]; fired = true; }
                break;
            }
        }
    }
    /* Rule 7: DEREF(ADD(base, idx)) -> ARRAY(base, idx) or FIELD(base, offset) */
    if (myop == EDEREF && l != NO_CLASS) {
        for (uint ai = 0u; ai < n_nodes; ai++) {
            if (op[ai] == EADD && uf_find(uf_parent, eclass[ai]) == l
                && lhs[ai] != NO_CLASS && rhs[ai] != NO_CLASS) {
                ulong fld = 0;
                bool is_lit = class_literal(op, eclass, lit, uf_parent, n_nodes, uf_find(uf_parent, rhs[ai]), &fld);
                if (is_lit) {
                    op[id]  = EFIELD;
                    lhs[id] = uf_find(uf_parent, lhs[ai]);
                    rhs[id] = NO_CLASS;
                    aux[id] = (uint)fld;
                } else {
                    op[id]  = EARRAY;
                    lhs[id] = uf_find(uf_parent, lhs[ai]);
                    rhs[id] = uf_find(uf_parent, rhs[ai]);
                }
                fired = true;
                break;
            }
        }
    }

    if (fired) atomic_or(dirty, 1u);
}

__kernel void retdec_egraph_extract(
    __global const uint *op,
    __global const uint *eclass,
    __global       uint *uf_parent,
    __global       uint *best_node,
    __global       uint *best_score,
                   uint  n_nodes,
                   uint  n_classes)
{
    uint cls = (uint)get_global_id(0);
    if (cls >= n_classes) return;

    uint root = uf_find(uf_parent, cls);
    uint bnode = NO_CLASS;
    uint bsc   = MAX_SCORE + 1u;

    for (uint i = 0u; i < n_nodes; i++) {
        if (uf_find(uf_parent, eclass[i]) == root) {
            uint sc = cdist(op[i]);
            if (sc < bsc) { bsc = sc; bnode = i; }
        }
    }

    best_node[cls]  = bnode;
    best_score[cls] = bsc;
}
)EGRAPH";
}

} // namespace opencl
} // namespace retdec
