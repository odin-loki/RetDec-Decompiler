/**
 * @file src/opencl/kernels/parallel_disasm.cl
 * @brief Parallel CFG-seed disassembly kernel for x86-64 binaries.
 *
 * Each work-item processes one CFG entry point (seed), walking instructions
 * forward until it hits a branch/ret/invalid, or an already-visited address.
 * An atomic byte-granularity visited bitset prevents duplicate work across
 * work-items.
 *
 * ## Data layout
 *
 *   bytes[]          – raw code section bytes (read-only)
 *   entry_offsets[]  – one seed per work-item (input)
 *   visited_bits[]   – ceil(byte_count/8) bytes, atomic bitset
 *   bb_out[]         – RetDecBasicBlock output array (one slot per work-item)
 *   bb_count         – global atomic counter for number of valid output BBs
 *   error_flags[]    – one uint per work-item; non-zero = decode error
 *
 * ## Limitations / deliberate simplifications
 *  - x86-64 only (64-bit default operand / address sizes).
 *  - Immediate addresses for JMP/JCC/CALL are resolved relative to
 *    the start of the `bytes` buffer (i.e. treat offset 0 as VMA 0).
 *    The host must add the section base VMA.
 *  - 3DNow! and XOP prefixes are treated as "other" (not decoded further).
 *  - VEX/EVEX are length-decoded but branch analysis is skipped.
 */

/* ---- Basic block descriptor (kept in sync with host RetDecBasicBlock) ----- */
#define RETDEC_BB_ADDR_NONE   0xFFFFFFFFFFFFFFFFUL

#define RETDEC_BB_FLAG_HAS_CALL   (1u)
#define RETDEC_BB_FLAG_ENDS_RET   (2u)
#define RETDEC_BB_FLAG_ENDS_JMP   (4u)
#define RETDEC_BB_FLAG_ENDS_JCC   (8u)
#define RETDEC_BB_FLAG_INVALID    (16u)

typedef struct {
    ulong start_addr;
    ulong end_addr;     /* exclusive – points to first byte of next BB */
    ulong successor0;   /* fall-through (JCC) or only target (JMP/CALL) */
    ulong successor1;   /* branch-taken for JCC; RETDEC_BB_ADDR_NONE otherwise */
    uint  insn_count;
    uint  flags;
} RetDecBasicBlock;

/* ---- Atomic visited-bitset helpers --------------------------------------- */

static bool visit_and_check(__global atomic_uint *visited_bits, ulong byte_idx)
{
    ulong word_idx = byte_idx >> 5;          /* divide by 32 (uint = 4 bytes = 32 bits) */
    uint  bit_idx  = (uint)(byte_idx & 31u);
    uint  mask     = 1u << bit_idx;
    uint  old      = atomic_fetch_or_explicit(
                         &visited_bits[word_idx], mask,
                         memory_order_relaxed, memory_scope_device);
    return (old & mask) != 0u;   /* true = already visited */
}

/* ---- Minimal x86-64 instruction length decoder -------------------------- */

/* Returns 0 on decode error, otherwise the byte length of the instruction. */
static uint x86_insn_length(__global const uchar *bytes, ulong off, ulong max_off)
{
    ulong start = off;
    uchar b;

    /* ── Gather prefix bytes ─────────────────────────────────────────── */
    bool has_66  = false;   /* operand-size override */
    bool has_67  = false;   /* address-size override */
    bool has_rex = false;
    bool rex_w   = false;
    bool has_rep = false;

    for (int pfx_limit = 0; pfx_limit < 5 && off < max_off; ++pfx_limit) {
        b = bytes[off];
        if (b == 0x66) { has_66  = true; ++off; }
        else if (b == 0x67) { has_67  = true; ++off; }
        else if (b == 0xF0 || b == 0xF2 || b == 0xF3) { has_rep = true; ++off; }
        else if (b == 0x26 || b == 0x2E || b == 0x36 ||
                 b == 0x3E || b == 0x64 || b == 0x65) { ++off; } /* seg overrides */
        else break;
    }
    if (off >= max_off) return 0;

    /* REX */
    b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) {
        has_rex = true;
        rex_w   = (b & 0x08) != 0;
        ++off;
        if (off >= max_off) return 0;
        b = bytes[off];
    }

    /* ── VEX / EVEX prefixes (length-decode only) ───────────────────── */
    if (b == 0xC4 || b == 0xC5) {   /* VEX 3-byte or 2-byte */
        uint vex_len = (b == 0xC4) ? 3u : 2u;
        off += vex_len;
        if (off >= max_off) return 0;
        /* opcode byte */
        ++off;
        if (off >= max_off) return 0;
        /* ModRM */
        uchar modrm = bytes[off]; ++off;
        uint  mod = (uint)(modrm >> 6);
        uint  rm  = (uint)(modrm & 7u);
        if (mod != 3u) {
            if (!has_67 && rm == 4u) ++off;  /* SIB */
            if (mod == 1u) ++off;             /* disp8 */
            else if (mod == 2u) off += 4u;   /* disp32 */
            else if (rm == 5u) off += 4u;    /* RIP+disp32 */
        }
        /* VEX imm8 for certain opcodes – conservative: skip. */
        if (off > max_off) return 0;
        return (uint)(off - start);
    }
    if (b == 0x62) {   /* EVEX 4-byte */
        off += 4u;
        if (off >= max_off) return 0;
        ++off; /* opcode */
        if (off >= max_off) return 0;
        uchar modrm = bytes[off]; ++off;
        uint mod = (uint)(modrm >> 6);
        uint rm  = (uint)(modrm & 7u);
        if (mod != 3u) {
            if (rm == 4u) ++off;
            if (mod == 1u) ++off;
            else if (mod == 2u) off += 4u;
            else if (rm == 5u) off += 4u;
        }
        if (off > max_off) return 0;
        return (uint)(off - start);
    }

    /* ── Regular opcodes ─────────────────────────────────────────────── */
    uchar opc = bytes[off]; ++off;
    if (off > max_off) return 0;

    bool two_byte = false;
    uchar opc2    = 0;

    if (opc == 0x0F) {
        if (off >= max_off) return 0;
        opc2 = bytes[off]; ++off;
        two_byte = true;
        /* 0F 38 xx / 0F 3A xx: three-byte opcodes */
        if ((opc2 == 0x38 || opc2 == 0x3A) && off < max_off) {
            ++off; /* third opcode byte */
        }
    }

    /* ── ModRM decode ────────────────────────────────────────────────── */

    /* Tables: does this opcode have a ModRM byte? */
    /* One-byte opcodes with ModRM: roughly 0x00-0x3F alt rows, many others. */
    /* We use a conservative approach: check known no-ModRM opcodes, else assume ModRM. */

    bool has_modrm = false;
    int  imm_bytes = 0;

    if (!two_byte) {
        /* No-ModRM single-byte opcodes (selected common ones). */
        /* Pushes/pops, moves to/from segreg, various singles. */
        /* INC/DEC r16/r32: 40-47, 48-4F handled as REX above */
        /* PUSH r: 50-57, POP r: 58-5F */
        if (opc >= 0x50 && opc <= 0x5F) { /* push/pop rN */ }
        /* PUSHF/POPF: 9C 9D */
        else if (opc == 0x9C || opc == 0x9D) { /* pushf/popf */ }
        /* NOP=0x90, XCHG rAX,r: 90-97 */
        else if (opc >= 0x90 && opc <= 0x97) { /* nop/xchg */ }
        /* CBW/CWDE/CDQE: 0x98, CWD/CDQ: 0x99 */
        else if (opc == 0x98 || opc == 0x99) { /* sign-extend */ }
        /* WAIT: 9B */
        else if (opc == 0x9B) { /* wait */ }
        /* SAHF: 0x9E, LAHF: 0x9F */
        else if (opc == 0x9E || opc == 0x9F) { /* sahf/lahf */ }
        /* RET near/far: C2/C3/CA/CB */
        else if (opc == 0xC3 || opc == 0xCB) { /* ret */ }
        else if (opc == 0xC2 || opc == 0xCA) { imm_bytes = 2; }
        /* ENTER: 0xC8 → imm16 + imm8 */
        else if (opc == 0xC8) { imm_bytes = 3; }
        /* LEAVE: 0xC9 */
        else if (opc == 0xC9) { /* leave */ }
        /* INT3/INT n/INTO: CC/CD/CE */
        else if (opc == 0xCC || opc == 0xCE) { /* int3/into */ }
        else if (opc == 0xCD) { imm_bytes = 1; }
        /* IRET: 0xCF */
        else if (opc == 0xCF) { /* iret */ }
        /* AAA/AAS/DAA/DAS: 37/3F/27/2F  (invalid in 64-bit but length-decode) */
        else if (opc == 0x37 || opc == 0x3F || opc == 0x27 || opc == 0x2F) { }
        /* CLC/STC/CLI/STI/CLD/STD: F8-FD */
        else if (opc >= 0xF8 && opc <= 0xFD) { }
        /* HLT: 0xF4, CMC: 0xF5 */
        else if (opc == 0xF4 || opc == 0xF5) { }
        /* XLAT: 0xD7 */
        else if (opc == 0xD7) { }
        /* IN/OUT fixed: E4/E5/EC/ED/E6/E7/EE/EF */
        else if (opc == 0xEC || opc == 0xED || opc == 0xEE || opc == 0xEF) { }
        else if (opc == 0xE4 || opc == 0xE5) { imm_bytes = 1; }
        else if (opc == 0xE6 || opc == 0xE7) { imm_bytes = 1; }
        /* CPUID: not single-byte but handled via 0x0F 0xA2 */
        /* Short Jcc: 70-7F, E3 */
        else if ((opc >= 0x70 && opc <= 0x7F) || opc == 0xEB || opc == 0xE3) {
            imm_bytes = 1;
        }
        /* Near JMP/Jcc: 0xE9 */
        else if (opc == 0xE9) { imm_bytes = 4; }
        /* CALL near rel32: 0xE8 */
        else if (opc == 0xE8) { imm_bytes = 4; }
        /* LOOP/LOOPx: E0/E1/E2 */
        else if (opc >= 0xE0 && opc <= 0xE2) { imm_bytes = 1; }
        /* MOV r,imm: B0-BF */
        else if (opc >= 0xB0 && opc <= 0xB7) { imm_bytes = 1; }
        else if (opc >= 0xB8 && opc <= 0xBF) {
            imm_bytes = rex_w ? 8 : (has_66 ? 2 : 4);
        }
        /* MOV AL/AX/EAX/RAX, moffs and reverse: A0-A3 */
        else if (opc >= 0xA0 && opc <= 0xA3) {
            imm_bytes = has_67 ? 4 : 8;
        }
        /* TEST AL, imm8: A8; TEST rAX, imm: A9 */
        else if (opc == 0xA8) { imm_bytes = 1; }
        else if (opc == 0xA9) { imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4); }
        /* MOVS/STOS/LODS/SCAS: A4-AF — no explicit operands */
        else if (opc >= 0xA4 && opc <= 0xAF) { }
        /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP rAX, imm: 05/0D/15/1D/25/2D/35/3D */
        else if (opc == 0x05 || opc == 0x0D || opc == 0x15 || opc == 0x1D ||
                 opc == 0x25 || opc == 0x2D || opc == 0x35 || opc == 0x3D) {
            imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4);
        }
        /* ADD/OR/… r/m, imm8: 04/0C/14/1C/24/2C/34/3C */
        else if (opc == 0x04 || opc == 0x0C || opc == 0x14 || opc == 0x1C ||
                 opc == 0x24 || opc == 0x2C || opc == 0x34 || opc == 0x3C) {
            imm_bytes = 1;
        }
        /* PUSH imm: 68 = imm32, 6A = imm8 */
        else if (opc == 0x68) { imm_bytes = has_66 ? 2 : 4; }
        else if (opc == 0x6A) { imm_bytes = 1; }
        /* Group 1 imm8: 83 xx /reg */
        else if (opc == 0x83) { has_modrm = true; imm_bytes = 1; }
        /* Group 1 imm: 81 xx /reg */
        else if (opc == 0x81) { has_modrm = true; imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4); }
        /* Group 2 reg,1: D0/D1; Group2 reg,imm8: C0/C1 */
        else if (opc == 0xC0 || opc == 0xC1) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0xD0 || opc == 0xD1 || opc == 0xD2 || opc == 0xD3) { has_modrm = true; }
        /* MOV r/m, imm: C6 (imm8), C7 (imm32/64) */
        else if (opc == 0xC6) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0xC7) { has_modrm = true; imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4); }
        /* IMUL r, r/m, imm: 6B (imm8), 69 (imm32) */
        else if (opc == 0x6B) { has_modrm = true; imm_bytes = 1; }
        else if (opc == 0x69) { has_modrm = true; imm_bytes = has_66 ? 2 : 4; }
        /* Default: assume ModRM, no immediate. */
        else {
            has_modrm = true;
        }
    } else {
        /* Two-byte opcodes (0F xx) */
        /* Near Jcc: 0F 80-8F → rel32 */
        if (opc2 >= 0x80 && opc2 <= 0x8F) { imm_bytes = 4; }
        /* SHLD/SHRD with imm8: 0F A4 / 0F AC */
        else if (opc2 == 0xA4 || opc2 == 0xAC) { has_modrm = true; imm_bytes = 1; }
        /* IMUL r,r/m: 0F AF → ModRM only */
        else if (opc2 == 0xAF) { has_modrm = true; }
        /* MOVZX/MOVSX: 0F B6/B7/BE/BF → ModRM only */
        else if (opc2 == 0xB6 || opc2 == 0xB7 ||
                 opc2 == 0xBE || opc2 == 0xBF) { has_modrm = true; }
        /* CPUID: 0F A2 — no operands */
        else if (opc2 == 0xA2) { }
        /* SYSCALL/SYSRET/RDTSC: 05/07/31 etc — no operands */
        else if (opc2 == 0x05 || opc2 == 0x07 || opc2 == 0x31 ||
                 opc2 == 0x34 || opc2 == 0x35) { }
        /* NOP r/m: 0F 1F */
        else if (opc2 == 0x1F) { has_modrm = true; }
        /* PREFETCH: 0F 18 */
        else if (opc2 == 0x18) { has_modrm = true; }
        /* Default two-byte: assume ModRM */
        else { has_modrm = true; }
    }

    /* ── Decode ModRM + optional SIB + displacement ─────────────────── */
    if (has_modrm) {
        if (off >= max_off) return 0;
        uchar modrm = bytes[off]; ++off;
        uint mod = (uint)(modrm >> 6);
        uint rm  = (uint)(modrm & 7u);

        uint disp = 0;
        if (mod == 1u) {
            disp = 1u;
        } else if (mod == 2u) {
            disp = 4u;
        } else if (mod == 0u && rm == 5u) {
            disp = 4u;   /* RIP-relative */
        }

        bool need_sib = (!has_67 && mod != 3u && rm == 4u);
        if (need_sib) {
            if (off >= max_off) return 0;
            uchar sib = bytes[off]; ++off;
            /* SIB base=5 + mod=0 → disp32 */
            if ((sib & 7u) == 5u && mod == 0u) {
                disp = 4u;
            }
        }
        off += disp;
    }

    off += (ulong)(uint)imm_bytes;

    if (off > max_off || off > start + 15u) {
        return 0;  /* instruction length sanity cap: x86 max = 15 bytes */
    }
    return (uint)(off - start);
}

/* ---- Branch-target extraction -------------------------------------------- */

typedef struct {
    int   type;           /* 0=none/fallthrough, 1=JMP, 2=JCC, 3=CALL, 4=RET */
    ulong target;         /* absolute offset of branch target; RETDEC_BB_ADDR_NONE if indirect */
} BranchInfo;

static BranchInfo decode_branch_info(__global const uchar *bytes,
                                     ulong insn_off,
                                     uint  insn_len,
                                     ulong base_vma)
{
    BranchInfo bi;
    bi.type   = 0;
    bi.target = RETDEC_BB_ADDR_NONE;

    if (insn_len == 0) return bi;

    uchar first = bytes[insn_off];

    /* Skip prefixes */
    ulong off = insn_off;
    uchar b;
    for (int i = 0; i < 5; ++i) {
        b = bytes[off];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            ++off;
        } else {
            break;
        }
    }
    /* REX */
    b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) ++off;
    b = bytes[off];

    ulong next_insn = insn_off + (ulong)insn_len;

    if (b == 0x0F) {
        uchar b2 = bytes[off + 1];
        if (b2 >= 0x80 && b2 <= 0x8F) {
            /* Near Jcc rel32 */
            int rel = (int)(
                (uint)(bytes[off + 2])       |
                ((uint)(bytes[off + 3]) << 8) |
                ((uint)(bytes[off + 4]) << 16)|
                ((uint)(bytes[off + 5]) << 24));
            bi.type   = 2;
            bi.target = base_vma + next_insn + (long)rel;
            return bi;
        }
    }

    if (b >= 0x70 && b <= 0x7F) {
        /* Short Jcc rel8 */
        int rel = (int)(schar)(bytes[off + 1]);
        bi.type   = 2;
        bi.target = base_vma + next_insn + (long)rel;
        return bi;
    }
    if (b == 0xE3) {
        /* JECXZ/JRCXZ rel8 */
        int rel = (int)(schar)(bytes[off + 1]);
        bi.type   = 2;
        bi.target = base_vma + next_insn + (long)rel;
        return bi;
    }
    if (b == 0xEB) {
        /* Short JMP rel8 */
        int rel = (int)(schar)(bytes[off + 1]);
        bi.type   = 1;
        bi.target = base_vma + next_insn + (long)rel;
        return bi;
    }
    if (b == 0xE9) {
        /* Near JMP rel32 */
        int rel = (int)(
            (uint)(bytes[off + 1])       |
            ((uint)(bytes[off + 2]) << 8) |
            ((uint)(bytes[off + 3]) << 16)|
            ((uint)(bytes[off + 4]) << 24));
        bi.type   = 1;
        bi.target = base_vma + next_insn + (long)rel;
        return bi;
    }
    if (b == 0xFF) {
        uchar modrm = bytes[off + 1];
        uint  reg   = (uint)((modrm >> 3) & 7u);
        if (reg == 4u) { bi.type = 1; bi.target = RETDEC_BB_ADDR_NONE; return bi; } /* JMP r/m */
        if (reg == 5u) { bi.type = 1; bi.target = RETDEC_BB_ADDR_NONE; return bi; } /* JMP far */
        if (reg == 2u) { bi.type = 3; bi.target = RETDEC_BB_ADDR_NONE; return bi; } /* CALL r/m */
        if (reg == 3u) { bi.type = 3; bi.target = RETDEC_BB_ADDR_NONE; return bi; } /* CALL far */
    }
    if (b == 0xE8) {
        /* CALL rel32 */
        int rel = (int)(
            (uint)(bytes[off + 1])       |
            ((uint)(bytes[off + 2]) << 8) |
            ((uint)(bytes[off + 3]) << 16)|
            ((uint)(bytes[off + 4]) << 24));
        bi.type   = 3;
        bi.target = base_vma + next_insn + (long)rel;
        return bi;
    }
    if (b == 0xC3 || b == 0xCB || b == 0xC2 || b == 0xCA) {
        bi.type = 4; /* RET */
        return bi;
    }
    if (b == 0xCF) {
        bi.type = 4; /* IRET */
        return bi;
    }

    return bi; /* fall-through */
}

/* =========================================================================
 * Main kernel
 * =========================================================================
 * __global ulong *entry_offsets  – one entry per work-item (VMA offsets)
 * __global uint  *num_entries    – total number of valid entry offsets
 * __global ulong  base_vma       – virtual address of bytes[0]
 * __global RetDecBasicBlock *bb_out – output array, one slot per work-item
 * __global atomic_uint *visited  – ceil(byte_count/32) atomic uints
 * __global uint  *error_flags    – one uint per work-item
 */
__kernel
__attribute__((reqd_work_group_size(64, 1, 1)))
void retdec_parallel_disasm(
    __global const uchar       *bytes,
    ulong                       byte_count,
    __global const ulong       *entry_offsets,
    uint                        num_entries,
    ulong                       base_vma,
    __global RetDecBasicBlock  *bb_out,
    __global volatile uint     *visited_words,   /* atomic bitset, 1 bit per byte */
    __global uint              *error_flags)
{
    size_t gid = get_global_id(0);
    if (gid >= (size_t)num_entries) return;

    ulong seed = entry_offsets[gid];
    if (seed >= byte_count) {
        error_flags[gid] = 1u;
        return;
    }

    /* Walk forward from seed to build one basic block. */
    RetDecBasicBlock bb;
    bb.start_addr = base_vma + seed;
    bb.end_addr   = base_vma + seed;
    bb.successor0 = RETDEC_BB_ADDR_NONE;
    bb.successor1 = RETDEC_BB_ADDR_NONE;
    bb.insn_count = 0u;
    bb.flags      = 0u;

    ulong off = seed;

    for (uint steps = 0; steps < 4096u && off < byte_count; ++steps) {
        /* Mark byte as visited; if already visited by another work-item, stop. */
        ulong word_idx = off >> 5;
        uint  bit_idx  = (uint)(off & 31u);
        uint  mask     = 1u << bit_idx;
        __global volatile uint *wordp = visited_words + word_idx;
        uint old = atomic_or(wordp, mask);
        if (old & mask) {
            /* Already visited — terminate BB here, successor = current addr. */
            bb.successor0 = base_vma + off;
            break;
        }

        uint len = x86_insn_length(bytes, off, byte_count);
        if (len == 0u) {
            bb.flags |= RETDEC_BB_FLAG_INVALID;
            break;
        }

        ulong next_off = off + (ulong)len;
        BranchInfo bi  = decode_branch_info(bytes, off, len, base_vma);

        ++bb.insn_count;
        bb.end_addr = base_vma + next_off;

        if (bi.type == 3) {
            /* CALL: record target as hint, continue as fall-through */
            bb.flags |= RETDEC_BB_FLAG_HAS_CALL;
            off = next_off;
        } else if (bi.type == 1) {
            /* Unconditional JMP */
            bb.flags    |= RETDEC_BB_FLAG_ENDS_JMP;
            bb.successor0 = bi.target;
            break;
        } else if (bi.type == 2) {
            /* Conditional JCC: fall-through + taken */
            bb.flags    |= RETDEC_BB_FLAG_ENDS_JCC;
            bb.successor0 = base_vma + next_off;   /* fall-through */
            bb.successor1 = bi.target;               /* taken */
            break;
        } else if (bi.type == 4) {
            /* RET */
            bb.flags |= RETDEC_BB_FLAG_ENDS_RET;
            break;
        } else {
            /* Fall-through to next instruction */
            off = next_off;
        }
    }

    bb_out[gid] = bb;
    error_flags[gid] = 0u;
}
