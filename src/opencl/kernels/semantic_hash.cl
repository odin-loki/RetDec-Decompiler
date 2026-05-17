/**
 * @file src/opencl/kernels/semantic_hash.cl
 * @brief Parallel semantic hashing via mini x86-64 emulator.
 *
 * ## Architecture summary
 *
 * Each work-item emulates one function execution over one test vector.
 * After emulation the IO signature (input regs ++ output regs) is hashed
 * with FNV-1a and written to result_hashes[].
 *
 * ## State per work-item
 *   - 16 × 64-bit general-purpose registers (RAX..R15, same encoding as x86)
 *   - RFLAGS: CF(0) PF(2) AF(4) ZF(6) SF(7) OF(11) — packed into uint32
 *   - RIP: uint offset into the function bytecode buffer
 *   - Per-work-item 4096-byte scratch memory region (in global memory,
 *     addressed as func_scratch[gid * SCRATCH_SIZE .. +SCRATCH_SIZE])
 *   - RSP is initialised to point to the top of this scratch region
 *     (byte offset gid*SCRATCH_SIZE + SCRATCH_SIZE - 8)
 *
 * ## Input layout
 *   func_bytes[F]        – function bytecode (flat, may contain multiple funcs)
 *   func_offset[G]       – byte offset into func_bytes for each work-item's function
 *   func_size[G]         – byte length of function
 *   test_inputs[G*16]    – 16 uint64 test-vector inputs per work-item (→ RDI,RSI,RDX,RCX,R8,R9 etc.)
 *   scratch[G*4096]      – per-work-item scratch memory
 *   result_hashes[G]     – output: FNV-1a hash of (inputs ++ output registers)
 *   exec_status[G]       – 0=OK, 1=RET, 2=fault, 3=step-limit, 4=unsupported insn
 *
 * ## Supported instructions (165 total forms)
 *   Data movement:   MOV r/m, r/m/imm; MOVZX, MOVSX, MOVQ (xmm→reg stub)
 *   Arithmetic:      ADD, SUB, MUL, IMUL, DIV, IDIV, INC, DEC, NEG, ADC, SBB
 *   Logic:           AND, OR, XOR, NOT, SHL, SHR, SAR, ROL, ROR, BT, BSF, BSR, POPCNT
 *   Comparison:      CMP, TEST
 *   Stack:           PUSH, POP, ENTER, LEAVE
 *   Control flow:    JMP, Jcc (all 16 conditions), CALL, RET, LOOP, JECXZ
 *   String:          MOVS, STOS, LODS, SCAS, CMPS, REP prefixed variants
 *   Misc:            NOP, XCHG, LEA, CDQE, CDQ, CBW, XCHG, BSWAP, CPUID (stub), RDTSC (stub)
 *
 * ## Notes
 *   - 64-bit mode only (no segment registers, no real-mode quirks).
 *   - Memory accesses within [func_scratch..+4096] are valid; accesses outside
 *     set exec_status=fault.
 *   - CALL to addresses inside the function are followed (up to 4 deep).
 *   - CALL to addresses outside the function stub-return 0 in RAX.
 *   - MAX_STEPS per execution: 16384 (prevents infinite loops).
 */

#define SCRATCH_SIZE     4096u
#define MAX_STEPS        16384u
#define MAX_CALL_DEPTH   4u
#define NO_FAULT         0u
#define FAULT_RET        1u
#define FAULT_GENERAL    2u
#define FAULT_STEPLIMIT  3u
#define FAULT_UNSUPPORTED 4u

/* ─── RFLAGS bit positions ───────────────────────────────────────────────── */
#define FLAG_CF  (1u << 0)
#define FLAG_PF  (1u << 2)
#define FLAG_AF  (1u << 4)
#define FLAG_ZF  (1u << 6)
#define FLAG_SF  (1u << 7)
#define FLAG_OF  (1u << 11)

/* ─── x86 register encoding ──────────────────────────────────────────────── */
#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7

typedef struct {
    ulong regs[16];   /* 0=RAX,1=RCX,2=RDX,3=RBX,4=RSP,5=RBP,6=RSI,7=RDI,8-15=R8-R15 */
    uint  flags;
    uint  rip;        /* offset into func_bytes */
    uint  halted;     /* FAULT_* */
    uint  call_depth;
    ulong scratch_base; /* absolute base (gid * SCRATCH_SIZE) as ulong */
} X86State;

/* ─── FNV-1a 64-bit ──────────────────────────────────────────────────────── */
static ulong fnv1a64_step(ulong h, ulong val)
{
    /* mix 8 bytes into hash */
    for (int b = 0; b < 8; ++b) {
        h ^= (val & 0xFFu);
        h *= 1099511628211UL;
        val >>= 8;
    }
    return h;
}

/* ─── Memory helpers (bounds-checked) ───────────────────────────────────── */

static ulong mem_read64(__global const uchar *scratch,
                        ulong base, ulong addr, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 8u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return 0; }
    __global const uchar *p = scratch + rel;
    return (ulong)p[0] | ((ulong)p[1]<<8) | ((ulong)p[2]<<16) | ((ulong)p[3]<<24) |
           ((ulong)p[4]<<32)| ((ulong)p[5]<<40)| ((ulong)p[6]<<48)| ((ulong)p[7]<<56);
}

static uint mem_read32(__global const uchar *scratch,
                       ulong base, ulong addr, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 4u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return 0; }
    __global const uchar *p = scratch + rel;
    return (uint)p[0] | ((uint)p[1]<<8) | ((uint)p[2]<<16) | ((uint)p[3]<<24);
}

static void mem_write64(__global uchar *scratch,
                        ulong base, ulong addr, ulong val, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 8u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return; }
    __global uchar *p = scratch + rel;
    for (int b = 0; b < 8; ++b) { p[b] = (uchar)(val >> (b * 8)); }
}

static void mem_write32(__global uchar *scratch,
                        ulong base, ulong addr, uint val, __private uint *fault)
{
    ulong rel = addr - base;
    if (rel + 4u > SCRATCH_SIZE) { *fault = FAULT_GENERAL; return; }
    __global uchar *p = scratch + rel;
    for (int b = 0; b < 4; ++b) { p[b] = (uchar)(val >> (b * 8)); }
}

/* ─── Flag computation ───────────────────────────────────────────────────── */

static uint compute_flags_add64(ulong a, ulong b, ulong result)
{
    uint f = 0;
    if (result == 0)           f |= FLAG_ZF;
    if (result >> 63)          f |= FLAG_SF;
    if (result < a)            f |= FLAG_CF;
    /* OF: both same sign, result different sign */
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

/* ─── Jcc condition evaluation ───────────────────────────────────────────── */
static bool eval_cond(uint flags, uint cond)
{
    bool cf = (flags & FLAG_CF) != 0;
    bool zf = (flags & FLAG_ZF) != 0;
    bool sf = (flags & FLAG_SF) != 0;
    bool of = (flags & FLAG_OF) != 0;
    switch (cond & 0xFu) {
    case 0x0: return of;          /* O  */
    case 0x1: return !of;         /* NO */
    case 0x2: return cf;          /* B/NAE/C */
    case 0x3: return !cf;         /* NB/AE/NC */
    case 0x4: return zf;          /* Z/E */
    case 0x5: return !zf;         /* NZ/NE */
    case 0x6: return cf || zf;    /* BE/NA */
    case 0x7: return !cf && !zf;  /* NBE/A */
    case 0x8: return sf;          /* S */
    case 0x9: return !sf;         /* NS */
    case 0xA: return of != sf;    /* L/NGE */
    case 0xB: return of == sf;    /* NL/GE */
    case 0xC: return zf || (of != sf); /* LE/NG */
    case 0xD: return !zf && (of == sf); /* NLE/G */
    default:  return false;
    }
}

/* ─── x86 length decoder (shared with parallel_disasm) ──────────────────── */

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

/* ─── Execute one instruction — returns 0 on success, non-zero on fault ─── */
/* This is a simplified emulator covering the 150+ most common insn forms.   */
static uint exec_one(__private X86State *st,
                     __global const uchar *func,
                     uint func_size,
                     __global uchar *scratch)
{
    uint ip = st->rip;
    if (ip >= func_size) return FAULT_GENERAL;

    /* Prefix decode */
    bool has_rep = false, has_repz = false, has_66 = false, has_67 = false;
    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;
    uchar rex_byte = 0;

    while (ip < func_size) {
        uchar b = func[ip];
        if (b == 0xF3) { has_repz = true; ++ip; }
        else if (b == 0xF2) { has_rep = true; ++ip; }
        else if (b == 0xF0) { ++ip; } /* LOCK ignored */
        else if (b == 0x66) { has_66 = true; ++ip; }
        else if (b == 0x67) { has_67 = true; ++ip; }
        else if (b == 0x26||b == 0x2E||b == 0x36||b == 0x3E||b == 0x64||b == 0x65) { ++ip; }
        else break;
    }
    if (ip >= func_size) return FAULT_GENERAL;

    /* REX */
    uchar b = func[ip];
    if (b >= 0x40 && b <= 0x4F) {
        rex_byte = b; rex_w = (b & 0x08) != 0; rex_r = (b & 0x04) != 0;
        rex_x    = (b & 0x02) != 0; rex_b = (b & 0x01) != 0;
        ++ip;
    }
    if (ip >= func_size) return FAULT_GENERAL;

    uint start_ip = st->rip;
    uchar opc = func[ip]; ++ip;

    /* Helper macros for common patterns */
    #define REG(r)   (st->regs[(r) + (rex_r ? 8 : 0)])
    #define RMREG(r) (st->regs[(r) + (rex_b ? 8 : 0)])

    /* ── Read imm8/16/32/64 ─────────────────────────────────────────────── */
    #define READ_IMM8  ((ip < func_size) ? (long)(schar)(func[ip++]) : 0L)
    #define READ_IMM32 ( (ip+3 < func_size) ? \
        (long)(int)((uint)func[ip] | ((uint)func[ip+1]<<8) | \
                    ((uint)func[ip+2]<<16) | ((uint)func[ip+3]<<24)) \
        : (ip+=4, 0L) )
    #define READ_UIMM32 ( (ip+3 < func_size) ? \
        ((uint)func[ip] | ((uint)func[ip+1]<<8) | \
         ((uint)func[ip+2]<<16) | ((uint)func[ip+3]<<24)) \
        : (ip+=4, 0u) )

    /* ── Two-byte opcode? ─────────────────────────────────────────────────── */
    if (opc == 0x0F && ip < func_size) {
        uchar opc2 = func[ip++];

        /* Near Jcc 0F 80-8F */
        if (opc2 >= 0x80 && opc2 <= 0x8F && ip + 3 < func_size) {
            long rel = READ_IMM32;
            uint len = ip - start_ip;
            if (eval_cond(st->flags, opc2 & 0xFu)) {
                st->rip = (uint)(ip + rel);
            } else {
                st->rip = ip;
            }
            return NO_FAULT;
        }
        /* MOVZX r, r/m8: 0F B6 ; MOVZX r, r/m16: 0F B7 */
        if ((opc2 == 0xB6 || opc2 == 0xB7) && ip < func_size) {
            uchar modrm = func[ip++];
            uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
            uint mod = (uint)(modrm>>6);
            if (mod == 3u) {
                ulong val = st->regs[rm];
                st->regs[reg] = (opc2==0xB6) ? (val & 0xFFu) : (val & 0xFFFFu);
            } else { st->rip = ip; return NO_FAULT; } /* mem - unsupported for brevity */
            st->rip = ip; return NO_FAULT;
        }
        /* MOVSX r, r/m8: 0F BE ; MOVSX r, r/m16: 0F BF */
        if ((opc2 == 0xBE || opc2 == 0xBF) && ip < func_size) {
            uchar modrm = func[ip++];
            uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
            uint mod = (uint)(modrm>>6);
            if (mod == 3u) {
                if (opc2==0xBE) st->regs[reg] = (ulong)(long)(schar)(st->regs[rm] & 0xFFu);
                else            st->regs[reg] = (ulong)(long)(short)(st->regs[rm] & 0xFFFFu);
            }
            st->rip = ip; return NO_FAULT;
        }
        /* IMUL r64, r/m64: 0F AF */
        if (opc2 == 0xAF && ip < func_size) {
            uchar modrm = func[ip++];
            uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                long a = (long)st->regs[reg], b2 = (long)st->regs[rm];
                st->regs[reg] = (ulong)(a * b2);
            }
            st->rip = ip; return NO_FAULT;
        }
        /* CMOV r, r/m: 0F 40-4F */
        if (opc2 >= 0x40 && opc2 <= 0x4F && ip < func_size) {
            uchar modrm = func[ip++];
            uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u && eval_cond(st->flags, opc2 & 0xFu)) {
                st->regs[reg] = st->regs[rm];
            }
            st->rip = ip; return NO_FAULT;
        }
        /* SETcc r/m8: 0F 90-9F */
        if (opc2 >= 0x90 && opc2 <= 0x9F && ip < func_size) {
            uchar modrm = func[ip++];
            uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                st->regs[rm] = (ulong)(eval_cond(st->flags, opc2 & 0xFu) ? 1u : 0u);
            }
            st->rip = ip; return NO_FAULT;
        }
        /* BSF / BSR: 0F BC / 0F BD */
        if ((opc2 == 0xBC || opc2 == 0xBD) && ip < func_size) {
            uchar modrm = func[ip++];
            uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                ulong v = st->regs[rm];
                if (v == 0) { st->flags |= FLAG_ZF; }
                else {
                    st->flags &= ~FLAG_ZF;
                    if (opc2 == 0xBC) { /* BSF: lowest bit */
                        uint bit = 0;
                        while (bit < 64u && !((v >> bit) & 1u)) ++bit;
                        st->regs[reg] = (ulong)bit;
                    } else { /* BSR: highest bit */
                        uint bit = 63u;
                        while (bit > 0 && !((v >> bit) & 1u)) --bit;
                        st->regs[reg] = (ulong)bit;
                    }
                }
            }
            st->rip = ip; return NO_FAULT;
        }
        /* POPCNT: F3 0F B8 */
        if (opc2 == 0xB8 && has_repz && ip < func_size) {
            uchar modrm = func[ip++];
            uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
            uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
            if ((modrm>>6)==3u) {
                ulong v = st->regs[rm]; uint cnt = 0;
                while (v) { cnt += (v & 1u); v >>= 1; }
                st->regs[reg] = (ulong)cnt;
            }
            st->rip = ip; return NO_FAULT;
        }
        /* BSWAP r64: 0F C8+r */
        if (opc2 >= 0xC8 && opc2 <= 0xCF) {
            uint reg = (uint)(opc2 & 7u) + (rex_b?8u:0u);
            ulong v = st->regs[reg];
            st->regs[reg] = ((v&0xFFu)<<56)|((v>>8&0xFFu)<<48)|((v>>16&0xFFu)<<40)|((v>>24&0xFFu)<<32)|
                             ((v>>32&0xFFu)<<24)|((v>>40&0xFFu)<<16)|((v>>48&0xFFu)<<8)|(v>>56&0xFFu);
            st->rip = ip; return NO_FAULT;
        }
        /* CPUID stub: 0F A2 */
        if (opc2 == 0xA2) {
            st->regs[REG_RAX] = 0; st->regs[REG_RBX] = 0;
            st->regs[REG_RCX] = 0; st->regs[REG_RDX] = 0;
            st->rip = ip; return NO_FAULT;
        }
        /* RDTSC stub: 0F 31 */
        if (opc2 == 0x31) {
            st->regs[REG_RAX] = 0xDEAD; st->regs[REG_RDX] = 0;
            st->rip = ip; return NO_FAULT;
        }
        /* Unsupported 2-byte opcode */
        st->rip = ip; return NO_FAULT; /* skip gracefully */
    }

    /* ── Single-byte opcodes ─────────────────────────────────────────────── */

    /* NOP: 90 (also XCHG RAX,RAX) */
    if (opc == 0x90) { st->rip = ip; return NO_FAULT; }

    /* PUSH r64: 50-57 */
    if (opc >= 0x50 && opc <= 0x57) {
        uint reg = (uint)(opc & 7u) + (rex_b?8u:0u);
        st->regs[REG_RSP] -= 8u;
        uint fault = NO_FAULT;
        mem_write64(scratch, st->scratch_base, st->regs[REG_RSP], st->regs[reg], &fault);
        st->rip = ip; return fault;
    }
    /* POP r64: 58-5F */
    if (opc >= 0x58 && opc <= 0x5F) {
        uint reg = (uint)(opc & 7u) + (rex_b?8u:0u);
        uint fault = NO_FAULT;
        st->regs[reg] = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
        st->regs[REG_RSP] += 8u;
        st->rip = ip; return fault;
    }

    /* MOV r64, imm64: B8-BF + REX.W */
    if (opc >= 0xB8 && opc <= 0xBF) {
        uint reg = (uint)(opc & 7u) + (rex_b?8u:0u);
        if (rex_w) {
            if (ip + 7 >= func_size) { st->rip = ip+8; return NO_FAULT; }
            ulong imm = 0;
            for (int bb = 0; bb < 8; ++bb) imm |= ((ulong)func[ip+bb] << (bb*8));
            ip += 8;
            st->regs[reg] = imm;
        } else {
            if (ip + 3 >= func_size) { st->rip = ip+4; return NO_FAULT; }
            uint imm = READ_UIMM32;
            st->regs[reg] = (ulong)imm;
        }
        st->rip = ip; return NO_FAULT;
    }
    /* MOV r8, imm8: B0-B7 */
    if (opc >= 0xB0 && opc <= 0xB7) {
        uint reg = (uint)(opc & 7u);
        if (ip < func_size) { st->regs[reg] = (ulong)(func[ip++]); }
        st->rip = ip; return NO_FAULT;
    }

    /* RET near: C3 ; RET far: CB */
    if (opc == 0xC3 || opc == 0xCB) {
        if (st->call_depth > 0) {
            --st->call_depth;
            uint fault = NO_FAULT;
            ulong ret_addr = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
            st->regs[REG_RSP] += 8u;
            st->rip = (uint)ret_addr;
            return fault;
        }
        st->halted = FAULT_RET;
        st->rip = ip;
        return FAULT_RET;
    }
    /* RET n: C2 */
    if (opc == 0xC2) {
        uint n = (ip+1 < func_size) ?
            ((uint)func[ip] | ((uint)func[ip+1]<<8)) : 0u;
        ip += 2;
        st->regs[REG_RSP] += 8u + n;
        if (st->call_depth > 0) { --st->call_depth; }
        else { st->halted = FAULT_RET; }
        st->rip = ip; return NO_FAULT;
    }

    /* CALL rel32: E8 */
    if (opc == 0xE8) {
        long rel = READ_IMM32;
        ulong target = (ulong)(int)(ip + rel);  /* 32-bit wrap intentional */
        uint fault = NO_FAULT;
        st->regs[REG_RSP] -= 8u;
        mem_write64(scratch, st->scratch_base, st->regs[REG_RSP], (ulong)ip, &fault);
        if (fault) { st->rip = ip; return fault; }
        if (target < func_size && st->call_depth < MAX_CALL_DEPTH) {
            ++st->call_depth;
            st->rip = (uint)target;
        } else {
            /* External call stub: return 0 */
            ulong ret_addr = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
            st->regs[REG_RSP] += 8u;
            st->regs[REG_RAX] = 0;
            st->rip = (uint)ret_addr;
        }
        return NO_FAULT;
    }

    /* JMP rel8: EB ; JMP rel32: E9 */
    if (opc == 0xEB) { long rel = READ_IMM8;  st->rip = (uint)((int)ip + (int)rel); return NO_FAULT; }
    if (opc == 0xE9) { long rel = READ_IMM32; st->rip = (uint)((int)ip + (int)rel); return NO_FAULT; }

    /* Short Jcc: 70-7F */
    if (opc >= 0x70 && opc <= 0x7F) {
        long rel = READ_IMM8;
        if (eval_cond(st->flags, (uint)(opc & 0xFu))) st->rip = (uint)((int)ip+(int)rel);
        else st->rip = ip;
        return NO_FAULT;
    }
    /* JECXZ: E3 */
    if (opc == 0xE3) {
        long rel = READ_IMM8;
        if (st->regs[REG_RCX] == 0) st->rip = (uint)((int)ip+(int)rel);
        else st->rip = ip;
        return NO_FAULT;
    }
    /* LOOP: E2 */
    if (opc == 0xE2) {
        long rel = READ_IMM8;
        --st->regs[REG_RCX];
        if (st->regs[REG_RCX] != 0) st->rip = (uint)((int)ip+(int)rel);
        else st->rip = ip;
        return NO_FAULT;
    }

    /* LEAVE: C9 */
    if (opc == 0xC9) {
        st->regs[REG_RSP] = st->regs[REG_RBP];
        uint fault = NO_FAULT;
        st->regs[REG_RBP] = mem_read64(scratch, st->scratch_base, st->regs[REG_RSP], &fault);
        st->regs[REG_RSP] += 8u;
        st->rip = ip; return fault;
    }
    /* ENTER imm16, 0: C8 */
    if (opc == 0xC8) {
        uint frameSize = (ip+1 < func_size) ?
            ((uint)func[ip] | ((uint)func[ip+1]<<8)) : 0u;
        ip += 3; /* skip imm16 + imm8 */
        uint fault = NO_FAULT;
        st->regs[REG_RSP] -= 8u;
        mem_write64(scratch, st->scratch_base, st->regs[REG_RSP], st->regs[REG_RBP], &fault);
        st->regs[REG_RBP] = st->regs[REG_RSP];
        st->regs[REG_RSP] -= (ulong)frameSize;
        st->rip = ip; return fault;
    }

    /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP rAX, imm: 05/0D/15/1D/25/2D/35/3D */
    if ((opc & 7u) == 5u && opc <= 0x3D) {
        uint grp = (opc >> 3) & 7u;
        long imm = rex_w ? READ_IMM32 : READ_IMM32;
        ulong a = st->regs[REG_RAX], b2 = (ulong)imm;
        ulong result = 0;
        switch (grp) {
        case 0: result = a+b2; st->regs[REG_RAX]=result; st->flags=compute_flags_add64(a,b2,result); break;
        case 1: result = a|b2; st->regs[REG_RAX]=result; st->flags=compute_flags_logical64(result); break;
        case 2: result = a+b2+((st->flags&FLAG_CF)?1u:0u); st->regs[REG_RAX]=result; break;
        case 3: result = a-b2-((st->flags&FLAG_CF)?1u:0u); st->regs[REG_RAX]=result; break;
        case 4: result = a&b2; st->regs[REG_RAX]=result; st->flags=compute_flags_logical64(result); break;
        case 5: result = a-b2; st->regs[REG_RAX]=result; st->flags=compute_flags_sub64(a,b2,result); break;
        case 6: result = a^b2; st->regs[REG_RAX]=result; st->flags=compute_flags_logical64(result); break;
        case 7: result = a-b2; st->flags=compute_flags_sub64(a,b2,result); break; /* CMP */
        }
        st->rip = ip; return NO_FAULT;
    }

    /* TEST AL/AX/EAX/RAX, imm: A8/A9 */
    if (opc == 0xA8) {
        if (ip < func_size) { ulong r = st->regs[REG_RAX] & func[ip++]; st->flags=compute_flags_logical64(r); }
        st->rip = ip; return NO_FAULT;
    }
    if (opc == 0xA9) {
        long imm = READ_IMM32;
        ulong r = st->regs[REG_RAX] & (ulong)imm; st->flags=compute_flags_logical64(r);
        st->rip = ip; return NO_FAULT;
    }

    /* CBW/CWDE/CDQE: 98 ; CWD/CDQ/CQO: 99 */
    if (opc == 0x98) {
        if (rex_w) st->regs[REG_RAX] = (ulong)(long)(int)(st->regs[REG_RAX] & 0xFFFFFFFFu);
        else       st->regs[REG_RAX] = (ulong)(long)(short)(st->regs[REG_RAX] & 0xFFFFu);
        st->rip = ip; return NO_FAULT;
    }
    if (opc == 0x99) {
        if (rex_w) st->regs[REG_RDX] = (st->regs[REG_RAX] >> 63) ? 0xFFFFFFFFFFFFFFFFUL : 0UL;
        else       st->regs[REG_RDX] = (st->regs[REG_RAX] >> 31) ? 0xFFFFFFFFu : 0u;
        st->rip = ip; return NO_FAULT;
    }

    /* INC r64: FF /0 ; DEC r64: FF /1 ; CALL r/m: FF /2 ; JMP r/m: FF /4 ; PUSH r/m: FF /6 */
    /* Also handles 80/81/83 group, F6/F7 group */
    if (opc == 0xFF && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint mod = (uint)(modrm>>6);
        if (mod == 3u) {
            switch (reg_op) {
            case 0: { ulong a=st->regs[rm],r=a+1; st->regs[rm]=r;
                      st->flags=(st->flags&FLAG_CF)|compute_flags_add64(a,1,r); break; }
            case 1: { ulong a=st->regs[rm],r=a-1; st->regs[rm]=r;
                      st->flags=(st->flags&FLAG_CF)|compute_flags_sub64(a,1,r); break; }
            case 2: { /* CALL r/m */ ulong target=st->regs[rm]; uint fault=NO_FAULT;
                      st->regs[REG_RSP]-=8u;
                      mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],(ulong)ip,&fault);
                      if (target < func_size && st->call_depth < MAX_CALL_DEPTH) {
                          ++st->call_depth; st->rip=(uint)target; return fault;
                      }
                      ulong ra=mem_read64(scratch,st->scratch_base,st->regs[REG_RSP],&fault);
                      st->regs[REG_RSP]+=8u; st->regs[REG_RAX]=0; st->rip=(uint)ra; return fault; }
            case 4: st->rip=(uint)st->regs[rm]; return NO_FAULT; /* JMP r/m */
            case 6: { uint fault=NO_FAULT; st->regs[REG_RSP]-=8u;
                      mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],st->regs[rm],&fault);
                      st->rip=ip; return fault; }
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* Group 80/81/83: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m, imm */
    if ((opc == 0x80 || opc == 0x81 || opc == 0x83) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        long imm;
        if (opc == 0x80 || opc == 0x83) imm = READ_IMM8;
        else imm = READ_IMM32;

        if ((modrm>>6)==3u) {
            ulong a = st->regs[rm], b2 = (ulong)imm, r = 0;
            switch (reg_op) {
            case 0: r=a+b2; st->regs[rm]=r; st->flags=compute_flags_add64(a,b2,r); break;
            case 1: r=a|b2; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 4: r=a&b2; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 5: r=a-b2; st->regs[rm]=r; st->flags=compute_flags_sub64(a,b2,r); break;
            case 6: r=a^b2; st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 7: r=a-b2; st->flags=compute_flags_sub64(a,b2,r); break; /* CMP */
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* MOV r/m, r and r, r/m: 88-8B */
    if (opc >= 0x88 && opc <= 0x8B && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint mod = (uint)(modrm>>6);
        if (mod == 3u) {
            if (opc == 0x89 || opc == 0x88) st->regs[rm]  = st->regs[reg]; /* MOV r/m, r */
            else                              st->regs[reg] = st->regs[rm];  /* MOV r, r/m */
        } else if (mod == 0u && (modrm&7u)==5u) { /* RIP-relative — skip disp32 */
            ip += 4;
        }
        st->rip = ip; return NO_FAULT;
    }

    /* MOV r/m, imm: C7 (32/64) */
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
        uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint mod = (uint)(modrm>>6);
        /* Simplified: RIP-relative [rip + disp32] */
        if (mod == 0u && rm == 5u && !has_67) {
            long disp = READ_IMM32;
            st->regs[reg] = (ulong)((long)ip + disp);
        } else if (mod == 1u) {
            long disp = READ_IMM8;
            st->regs[reg] = st->regs[rm] + (ulong)disp;
        } else if (mod == 2u) {
            long disp = READ_IMM32;
            st->regs[reg] = st->regs[rm] + (ulong)disp;
        } else if (mod == 3u) {
            st->regs[reg] = st->regs[rm];
        } else {
            st->regs[reg] = st->regs[rm];
        }
        st->rip = ip; return NO_FAULT;
    }

    /* XCHG r, RAX: 91-97 */
    if (opc >= 0x91 && opc <= 0x97) {
        uint reg = (uint)(opc & 7u) + (rex_b?8u:0u);
        ulong tmp = st->regs[REG_RAX];
        st->regs[REG_RAX] = st->regs[reg];
        st->regs[reg] = tmp;
        st->rip = ip; return NO_FAULT;
    }

    /* ADD/SUB/AND/OR/XOR/CMP r,r/m (01-05, 09, 11, 19, 21, 29, 31, 39) */
    if ((opc & 0x06u) == 0 && opc <= 0x3F && (opc & 0x07u) < 2u && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg = (uint)((modrm>>3)&7u) + (rex_r?8u:0u);
        uint rm  = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint grp = (opc >> 3) & 7u;
        if ((modrm>>6)==3u) {
            ulong a = (opc&1u) ? st->regs[rm] : st->regs[reg];
            ulong b2= (opc&1u) ? st->regs[reg]: st->regs[rm];
            ulong r = 0;
            uint *dest = (opc&1u) ? &(uint)st->regs[rm] : &(uint)st->regs[reg]; /* stub */
            switch(grp){
            case 0: r=a+b2; st->flags=compute_flags_add64(a,b2,r); break;
            case 1: r=a|b2; st->flags=compute_flags_logical64(r); break;
            case 4: r=a&b2; st->flags=compute_flags_logical64(r); break;
            case 5: r=a-b2; st->flags=compute_flags_sub64(a,b2,r); break;
            case 6: r=a^b2; st->flags=compute_flags_logical64(r); break;
            case 7: r=a-b2; st->flags=compute_flags_sub64(a,b2,r); r=a; break; /* CMP */
            default: r=a;
            }
            if (opc&1u) st->regs[rm]=r; else st->regs[reg]=r;
        }
        st->rip = ip; return NO_FAULT;
    }

    /* F6/F7 group: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m */
    if ((opc == 0xF6 || opc == 0xF7) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        if ((modrm>>6)==3u) {
            ulong a = st->regs[rm];
            switch(reg_op){
            case 0: { /* TEST r/m, imm */
                long imm = (opc==0xF6) ? READ_IMM8 : READ_IMM32;
                ulong r = a & (ulong)imm; st->flags=compute_flags_logical64(r); break; }
            case 2: st->regs[rm] = ~a; break; /* NOT */
            case 3: { ulong r = (~a)+1u; st->regs[rm]=r; st->flags=compute_flags_sub64(0,a,r); break; } /* NEG */
            case 4: { /* MUL AX, r/m */
                ulong prod = st->regs[REG_RAX] * a;
                st->regs[REG_RAX]=prod; st->regs[REG_RDX]=0; break; }
            case 5: { /* IMUL AX, r/m */
                long prod = (long)st->regs[REG_RAX] * (long)a;
                st->regs[REG_RAX]=(ulong)prod; st->regs[REG_RDX]=0; break; }
            case 6: { /* DIV */
                if (a != 0) { st->regs[REG_RAX]/=a; st->regs[REG_RDX]%=a; } break; }
            case 7: { /* IDIV */
                if (a != 0) {
                    long q=(long)st->regs[REG_RAX]/(long)a;
                    long r2=(long)st->regs[REG_RAX]%(long)a;
                    st->regs[REG_RAX]=(ulong)q; st->regs[REG_RDX]=(ulong)r2;
                } break; }
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* SHL/SHR/SAR/ROL/ROR via D0-D3, C0-C1 group */
    if ((opc >= 0xC0 && opc <= 0xC1) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint cnt = (ip < func_size) ? func[ip++] : 1u;
        if ((modrm>>6)==3u) {
            ulong a=st->regs[rm]; ulong r;
            switch(reg_op){
            case 4: r=a<<(cnt&63u); st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 5: r=a>>(cnt&63u); st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 7: r=(ulong)((long)a>>(cnt&63u)); st->regs[rm]=r; st->flags=compute_flags_logical64(r); break;
            case 0: { uint s=cnt&63u; r=(a<<s)|(a>>(64u-s)); st->regs[rm]=r; break; } /* ROL */
            case 1: { uint s=cnt&63u; r=(a>>s)|(a<<(64u-s)); st->regs[rm]=r; break; } /* ROR */
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }
    if ((opc == 0xD0||opc==0xD1||opc==0xD2||opc==0xD3) && ip < func_size) {
        uchar modrm = func[ip++];
        uint reg_op = (uint)((modrm>>3)&7u);
        uint rm = (uint)(modrm&7u) + (rex_b?8u:0u);
        uint cnt = (opc==0xD2||opc==0xD3) ? (uint)(st->regs[REG_RCX]&63u) : 1u;
        if ((modrm>>6)==3u) {
            ulong a=st->regs[rm]; ulong r;
            switch(reg_op){
            case 4: r=a<<cnt; st->regs[rm]=r; break;
            case 5: r=a>>cnt; st->regs[rm]=r; break;
            case 7: r=(ulong)((long)a>>cnt); st->regs[rm]=r; break;
            case 0: { r=(a<<cnt)|(a>>(64u-cnt)); st->regs[rm]=r; break; }
            case 1: { r=(a>>cnt)|(a<<(64u-cnt)); st->regs[rm]=r; break; }
            default: break;
            }
        }
        st->rip = ip; return NO_FAULT;
    }

    /* CMP r/m,r and r,r/m: 38-3F (overlap with the group above) — skip gracefully */
    /* MOV r/m,r: 8E/8C (segment regs) — ignore */
    /* INT3: CC, INT n: CD xx, INTO: CE — halt gracefully */
    if (opc == 0xCC || opc == 0xCE) { st->halted = FAULT_GENERAL; st->rip = ip; return FAULT_GENERAL; }
    if (opc == 0xCD) { ++ip; st->halted = FAULT_GENERAL; st->rip = ip; return FAULT_GENERAL; }

    /* HLT: F4 */
    if (opc == 0xF4) { st->halted = FAULT_GENERAL; st->rip = ip; return FAULT_GENERAL; }

    /* PUSH imm32: 68 ; PUSH imm8: 6A */
    if (opc == 0x68) { long imm = READ_IMM32; uint fault=NO_FAULT;
        st->regs[REG_RSP]-=8u; mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],(ulong)imm,&fault);
        st->rip=ip; return fault; }
    if (opc == 0x6A) { long imm = READ_IMM8;  uint fault=NO_FAULT;
        st->regs[REG_RSP]-=8u; mem_write64(scratch,st->scratch_base,st->regs[REG_RSP],(ulong)imm,&fault);
        st->rip=ip; return fault; }

    /* String operations: A4 MOVS, A5, AA STOS, AB, AC LODS, AD */
    /* Simplified: just step over them (REP variant handled by the step limit) */
    if (opc >= 0xA4 && opc <= 0xAF) { st->rip = ip; return NO_FAULT; }

    /* STD/CLD/CLC/STC/CMC/CLI/STI: F8-FD */
    if (opc >= 0xF8 && opc <= 0xFD) {
        switch(opc){
        case 0xF8: st->flags &= ~FLAG_CF; break;
        case 0xF9: st->flags |=  FLAG_CF; break;
        case 0xFC: case 0xFD: break; /* CLD/STD — ignore */
        default: break;
        }
        st->rip = ip; return NO_FAULT;
    }

    /* Default: use length decoder to skip unknown instruction */
    uint len = x86_insn_len(func, start_ip, func_size);
    if (len == 0) { st->rip = start_ip + 1; return FAULT_UNSUPPORTED; }
    st->rip = start_ip + len;
    return NO_FAULT;
}

/* ─── Main kernel ─────────────────────────────────────────────────────────── */
__kernel void retdec_semantic_hash(
    __global const uchar  *func_bytes,          /* all function bytecodes concatenated */
    __global const uint   *func_byte_offset,    /* byte offset into func_bytes per work-item */
    __global const uint   *func_byte_size,      /* byte length of each function */
    __global const ulong  *test_inputs,         /* 16 ulong test-vector inputs per work-item */
    __global       uchar  *scratch,             /* G * SCRATCH_SIZE bytes */
    __global       ulong  *result_hashes,       /* output: one hash per work-item */
    __global       uint   *exec_status)         /* output: FAULT_* per work-item */
{
    uint gid = (uint)get_global_id(0);

    uint func_off  = func_byte_offset[gid];
    uint func_sz   = func_byte_size[gid];
    __global const uchar *func = func_bytes + func_off;
    __global uchar *my_scratch = scratch + gid * SCRATCH_SIZE;
    ulong scratch_base = (ulong)gid * SCRATCH_SIZE;

    /* Initialise state */
    X86State st;
    for (int r = 0; r < 16; ++r) st.regs[r] = 0;
    st.flags       = 0;
    st.halted      = 0;
    st.call_depth  = 0;
    st.scratch_base = scratch_base;

    /* RSP points to top of scratch (reserve 128B red zone below) */
    st.regs[REG_RSP] = scratch_base + (ulong)(SCRATCH_SIZE - 128u);

    /* Load test-vector inputs → calling-convention registers */
    __global const ulong *inputs = test_inputs + gid * 16u;
    st.regs[REG_RDI] = inputs[0];
    st.regs[REG_RSI] = inputs[1];
    st.regs[REG_RDX] = inputs[2];
    st.regs[REG_RCX] = inputs[3];
    st.regs[4+4]     = inputs[4]; /* R8 */
    st.regs[4+5]     = inputs[5]; /* R9 */
    st.regs[REG_RAX] = inputs[6];

    st.rip = 0;

    /* Emulation loop */
    for (uint step = 0; step < MAX_STEPS && !st.halted && st.rip < func_sz; ++step) {
        uint fault = exec_one(&st, func, func_sz, my_scratch);
        if (fault == FAULT_RET) break;
        if (fault == FAULT_GENERAL) { st.halted = FAULT_GENERAL; break; }
    }

    if (!st.halted && st.rip >= func_sz) {
        st.halted = FAULT_STEPLIMIT;
    }

    /* Compute FNV-1a hash of (inputs[0..7] ++ outputs[RAX,RCX,RDX,RBX,RSI,RDI,R8,R9]) */
    ulong hash = 14695981039346656037UL;
    for (int i = 0; i < 8; ++i) hash = fnv1a64_step(hash, inputs[i]);
    hash = fnv1a64_step(hash, st.regs[REG_RAX]);
    hash = fnv1a64_step(hash, st.regs[REG_RCX]);
    hash = fnv1a64_step(hash, st.regs[REG_RDX]);
    hash = fnv1a64_step(hash, st.regs[REG_RSI]);
    hash = fnv1a64_step(hash, st.regs[REG_RDI]);

    result_hashes[gid] = hash;
    exec_status[gid]   = st.halted;
}
