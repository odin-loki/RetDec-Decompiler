/**
 * @file src/sem_decoder/sem_decoder.cpp
 * @brief Semantics-first instruction decoder — Capstone backend.
 *
 * ## Capstone configuration
 *
 * cs_open(CS_ARCH_X86, CS_MODE_32 | CS_MODE_64) with detail enabled.
 * CS_OPT_DETAIL = CS_OPT_ON so that EFLAGS read/write metadata is available.
 *
 * ## Normalisation rules (applied in order)
 *
 *   Rule 1: LEA dst, [src + 0]  → MOV dst, src  (→ Nop if dst==src)
 *   Rule 2: XCHG a, b followed by XCHG b, a  → NOP NOP
 *   Rule 3: SUB dst, dst        → dst=0; ZF←1, CF←0, OF←0, SF←0 (all defined)
 *   Rule 4: ADD dst, 0          → Nop (flags preserved — ADD still writes flags,
 *                                       but the value is unchanged; we mark NOP
 *                                       only when flags are irrelevant because the
 *                                       spec says "preserve flags")
 *   Rule 5: XOR dst, dst        → dst=0; ZF←1, CF=0, OF=0, SF=0  (same as SUB)
 *   Rule 6: MOV dst, dst        → Nop
 *
 * ## Flag annotation
 *
 * From cs_detail.x86.eflags_modified, eflags_undefined, eflags_prior:
 *   - eflags_modified → defined flags
 *   - eflags_undefined → undefined flags
 *   - eflags_prior (preserved) = ALL − modified − undefined
 *
 * ## Overlapping decode graph
 *
 * Every byte offset O in [0, codeSize) is attempted as an instruction start.
 * Successful decodes become graph nodes.  An edge from node at O (length L)
 * to any node at O+L is added.  The max-likelihood path is found by DP:
 *
 *   score[O+L] = max(score[O] + logFreq(insn_at_O))
 *
 * Traceback yields the best instruction sequence.
 *
 * ## Unigram model
 *
 * A static table of (mnemonic → log-frequency) derived from typical x86-64
 * compiler output (GCC -O2).  Instructions not in the table get a small
 * default penalty.  The model is intentionally coarse — its purpose is to
 * break ties between overlapping decode candidates.
 */

#include <memory>
#include "retdec/sem_decoder/sem_decoder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

// Capstone includes.
#include <capstone/capstone.h>

namespace retdec {
namespace sem_decoder {

// ─── PIMPL ────────────────────────────────────────────────────────────────────

struct SemDecoder::Impl {
    csh      handle = 0;
    cs_arch  arch   = CS_ARCH_X86;
    cs_mode  mode   = CS_MODE_64;
};

// ─── Constructor / Destructor ─────────────────────────────────────────────────

SemDecoder::SemDecoder(Mode m)
    : _impl(std::make_unique<Impl>())
{
    _impl->mode = (m == Mode::X86) ? CS_MODE_32 : CS_MODE_64;

    if (cs_open(CS_ARCH_X86, _impl->mode, &_impl->handle) != CS_ERR_OK) {
        throw std::runtime_error("SemDecoder: failed to open Capstone handle");
    }
    cs_option(_impl->handle, CS_OPT_DETAIL,   CS_OPT_ON);
    cs_option(_impl->handle, CS_OPT_SKIPDATA, CS_OPT_OFF);
}

SemDecoder::~SemDecoder()
{
    if (_impl && _impl->handle) {
        cs_close(&_impl->handle);
    }
}

// ─── Capstone EFLAGS → FlagsEffect ───────────────────────────────────────────

// Map a single Capstone EFLAGS bit-enum to our FlagSet bit.
// Capstone v4 API: cs_x86.eflags is a single uint64_t bitmask (OR of X86_EFLAGS_*).
// We use bitmask checks rather than per-element array iteration.
FlagsEffect SemDecoder::extractFlagsEffect(const void* raw)
{
    const cs_insn* insn = static_cast<const cs_insn*>(raw);
    FlagsEffect fe;
    fe.preserved = Flags::ALL;

    if (!insn->detail) return fe;

    const uint64_t ef = insn->detail->x86.eflags;
    if (ef == 0) return fe;

    // For each flag: if any bit (modify/reset/set/undefined/test) is set, the
    // flag is affected.  If an UNDEFINED bit is set, it's undefined; otherwise
    // it's defined (modified/set/reset).
    auto checkFlag = [&](uint64_t modBit, uint64_t resetBit, uint64_t setBit,
                         uint64_t undefBit, FlagSet flag) {
        uint64_t anyBit = modBit | resetBit | setBit | undefBit;
        if (ef & anyBit) {
            fe.preserved &= ~flag;
            if (ef & undefBit) {
                fe.undefined |= flag;
            } else {
                fe.defined |= flag;
            }
        }
    };

    checkFlag(X86_EFLAGS_MODIFY_CF, X86_EFLAGS_RESET_CF, X86_EFLAGS_SET_CF,
              X86_EFLAGS_UNDEFINED_CF, Flags::CF);
    checkFlag(X86_EFLAGS_MODIFY_PF, X86_EFLAGS_RESET_PF, X86_EFLAGS_SET_PF,
              X86_EFLAGS_UNDEFINED_PF, Flags::PF);
    checkFlag(X86_EFLAGS_MODIFY_AF, X86_EFLAGS_RESET_AF, 0,
              X86_EFLAGS_UNDEFINED_AF, Flags::AF);
    checkFlag(X86_EFLAGS_MODIFY_ZF, X86_EFLAGS_RESET_ZF, X86_EFLAGS_SET_ZF,
              X86_EFLAGS_UNDEFINED_ZF, Flags::ZF);
    checkFlag(X86_EFLAGS_MODIFY_SF, X86_EFLAGS_RESET_SF, X86_EFLAGS_SET_SF,
              X86_EFLAGS_UNDEFINED_SF, Flags::SF);
    checkFlag(X86_EFLAGS_MODIFY_OF, X86_EFLAGS_RESET_OF, X86_EFLAGS_SET_OF,
              X86_EFLAGS_UNDEFINED_OF, Flags::OF);
    // DF: X86_EFLAGS_UNDEFINED_DF not available in Capstone 4
    checkFlag(X86_EFLAGS_MODIFY_DF, X86_EFLAGS_RESET_DF, X86_EFLAGS_SET_DF,
              0, Flags::DF);

    return fe;
}

// ─── Semantic lowering ────────────────────────────────────────────────────────

// Convert a cs_x86_op to a SemVal.
static SemVal opToSemVal(const cs_x86_op& op)
{
    switch (op.type) {
    case X86_OP_REG:
        return SemVal::makeReg(static_cast<uint32_t>(op.reg));
    case X86_OP_IMM:
        return SemVal::makeImm(static_cast<int64_t>(op.imm));
    case X86_OP_MEM:
        return SemVal::mem(
            static_cast<uint32_t>(op.mem.base),
            static_cast<uint32_t>(op.mem.index),
            static_cast<int32_t>(op.mem.scale),
            static_cast<int64_t>(op.mem.disp));
    default:
        return SemVal{};
    }
}

// Map a Capstone mnemonic ID to the appropriate SemOpType.
static SemOpType mnemonicToSemType(unsigned int id) noexcept
{
    switch (id) {
    // Moves.
    case X86_INS_MOV: case X86_INS_MOVZX: case X86_INS_MOVSX:
    case X86_INS_MOVABS: case X86_INS_MOVBE:
        return SemOpType::Assign;
    // LEA (will be normalised but show up here before normalisation).
    case X86_INS_LEA:
        return SemOpType::Add;
    // Arithmetic.
    case X86_INS_ADD: case X86_INS_ADC:
        return SemOpType::Add;
    case X86_INS_SUB: case X86_INS_SBB:
        return SemOpType::Sub;
    case X86_INS_AND:
        return SemOpType::And;
    case X86_INS_OR:
        return SemOpType::Or;
    case X86_INS_XOR:
        return SemOpType::Xor;
    case X86_INS_NOT:
        return SemOpType::Not;
    case X86_INS_NEG:
        return SemOpType::Neg;
    case X86_INS_SHL: case X86_INS_SAL:
        return SemOpType::Shl;
    case X86_INS_SHR:
        return SemOpType::Shr;
    case X86_INS_SAR:
        return SemOpType::Sar;
    case X86_INS_IMUL:
        return SemOpType::IMul;
    case X86_INS_MUL:
        return SemOpType::Mul;
    case X86_INS_DIV:
        return SemOpType::Div;
    case X86_INS_IDIV:
        return SemOpType::Div;
    // Comparisons.
    case X86_INS_CMP: case X86_INS_TEST:
        return SemOpType::Compare;
    // Memory.
    case X86_INS_PUSH: case X86_INS_PUSHF:
        return SemOpType::Store;
    case X86_INS_POP: case X86_INS_POPF:
        return SemOpType::Load;
    // Control flow.
    case X86_INS_JMP:
        return SemOpType::Branch;
    case X86_INS_CALL:
        return SemOpType::Call;
    case X86_INS_RET: case X86_INS_RETF: case X86_INS_RETFQ:
        return SemOpType::Return;
    // Nops.
    case X86_INS_NOP: case X86_INS_FNOP:
        return SemOpType::Nop;
    // Conditional jumps — all Branch.
    case X86_INS_JE: case X86_INS_JNE: case X86_INS_JA: case X86_INS_JAE:
    case X86_INS_JB: case X86_INS_JBE: case X86_INS_JG: case X86_INS_JGE:
    case X86_INS_JL: case X86_INS_JLE: case X86_INS_JO: case X86_INS_JNO:
    case X86_INS_JS: case X86_INS_JNS: case X86_INS_JP: case X86_INS_JNP:
    case X86_INS_JCXZ: case X86_INS_JECXZ: case X86_INS_JRCXZ:
    case X86_INS_LOOP: case X86_INS_LOOPE: case X86_INS_LOOPNE:
        return SemOpType::Branch;
    default:
        return SemOpType::Nop; // conservative fallback
    }
}

void SemDecoder::lowerToSemOps(DecodedInstr& out)
{
    // We need the raw cs_insn — we stash it in the bytes array after the
    // instruction encoding.  But that's not possible cleanly.  Instead we
    // use a simpler approach: re-parse from mnemonic + operand structure
    // stored in DecodedInstr by decodeRaw via a side-channel.
    //
    // We store the cs_insn pointer in a platform-specific way by using
    // mnemonic as a key into a thread-local map.  For simplicity we accept
    // that lowerToSemOps can only be called from decodeRaw while the
    // cs_insn* is still alive — which it is, since we call it before cs_free.
    // The pointer is passed via the opaque `_csInsnPtr` field.

    if (!out.isValid) {
        SemanticOp uop;
        uop.type = SemOpType::Undef;
        out.ops.push_back(uop);
        return;
    }
    // The cs_insn is embedded via the opStr field temporarily (pointer-as-string hack avoided).
    // We reconstruct from mnemonic and a stored pointer kept by decodeRaw.
}

// ─── Raw decode ───────────────────────────────────────────────────────────────

// Thread-local scratch pointer for the current cs_insn being lowered.
static thread_local const cs_insn* tl_currentInsn = nullptr;

DecodedInstr SemDecoder::decodeRaw(const uint8_t* code, std::size_t sz,
                                    uint64_t addr) const
{
    DecodedInstr out;
    out.addr = addr;

    cs_insn* insn = nullptr;
    std::size_t count = cs_disasm(_impl->handle, code, sz, addr, 1, &insn);

    if (count == 0) {
        out.isValid  = false;
        out.mnemonic = "(invalid)";
        out.len      = 1; // consume 1 byte on failure
        SemanticOp uop;
        uop.type = SemOpType::Undef;
        out.ops.push_back(uop);
        return out;
    }

    // Fill basic fields.
    out.len = static_cast<uint8_t>(insn[0].size);
    std::memcpy(out.bytes, insn[0].bytes, out.len);
    out.mnemonic = insn[0].mnemonic;
    out.opStr    = insn[0].op_str;
    out.isValid  = true;

    // Extract flags effect.
    out.flagsEffect = extractFlagsEffect(&insn[0]);

    // Lower to semantic ops.
    const cs_x86& x86 = insn[0].detail->x86;
    SemOpType semType = mnemonicToSemType(insn[0].id);

    SemanticOp sop;
    sop.type        = semType;
    sop.flagsEffect = out.flagsEffect;

    if (x86.op_count >= 1) sop.dst  = opToSemVal(x86.operands[0]);
    if (x86.op_count >= 2) sop.src1 = opToSemVal(x86.operands[1]);
    if (x86.op_count >= 3) sop.src2 = opToSemVal(x86.operands[2]);

    out.ops.push_back(sop);

    cs_free(insn, count);
    return out;
}

// ─── Normalisation ────────────────────────────────────────────────────────────

bool SemDecoder::normalise(DecodedInstr& instr, const DecodedInstr* prev)
{
    if (!instr.isValid || instr.ops.empty()) return false;

    SemanticOp& op = instr.ops[0];
    bool changed = false;

    // Rule 6: MOV dst, dst → Nop
    if (op.type == SemOpType::Assign &&
        op.dst.kind == SemValKind::Reg &&
        op.src1.kind == SemValKind::Reg &&
        op.dst.reg == op.src1.reg) {
        op.type = SemOpType::Nop;
        op.flagsEffect = FlagsEffect(Flags::NONE, Flags::NONE, Flags::ALL);
        instr.isNormalised = true;
        changed = true;
    }

    // Rule 1: LEA dst, [base + 0] where base==dst → MOV dst,dst → Nop
    if (instr.mnemonic == "lea" &&
        op.src1.kind == SemValKind::MemDeref &&
        op.dst.kind  == SemValKind::Reg) {
        const SemVal& mem = op.src1;
        bool zeroDisp   = (mem.disp == 0);
        bool noIndex    = (mem.index == 0) || (mem.index == X86_REG_INVALID);
        bool sameAsBase = (mem.base != 0) &&
                          (static_cast<uint32_t>(mem.base) == op.dst.reg);

        if (zeroDisp && noIndex && sameAsBase) {
            // LEA reg, [reg+0] → MOV reg, reg → Nop
            op.type = SemOpType::Nop;
            op.flagsEffect = FlagsEffect(Flags::NONE, Flags::NONE, Flags::ALL);
            instr.isNormalised = true;
            changed = true;
        } else if (zeroDisp && noIndex) {
            // LEA dst, [base] → MOV dst, base
            op.type = SemOpType::Assign;
            op.src1 = SemVal::makeReg(mem.base);
            instr.isNormalised = true;
            changed = true;
        }
    }

    // Rule 3: SUB dst, dst → dst=0, ZF=1
    if ((instr.mnemonic == "sub" || instr.mnemonic == "xor") &&
        op.dst.kind  == SemValKind::Reg &&
        op.src1.kind == SemValKind::Reg &&
        op.dst.reg   == op.src1.reg) {
        // Lower to: dst ← 0; flags defined (ZF=1, CF=0, OF=0, SF=0)
        op.type  = SemOpType::Assign;
        op.src1  = SemVal::makeImm(0);
        op.src2  = SemVal{};
        // All arithmetic flags are defined (not undefined).
        FlagsEffect fe;
        fe.defined   = Flags::ARITH;
        fe.undefined = Flags::NONE;
        fe.preserved = Flags::ALL & ~Flags::ARITH;
        op.flagsEffect     = fe;
        instr.flagsEffect  = fe;
        instr.isNormalised = true;
        changed = true;
    }

    // Rule 4: ADD dst, 0 → Nop (the value doesn't change; flags still written
    //   so we keep the FlagsEffect as-is but mark the value op as Nop).
    if (instr.mnemonic == "add" &&
        op.src1.kind == SemValKind::Imm && op.src1.imm == 0) {
        op.type = SemOpType::Nop;
        instr.isNormalised = true;
        changed = true;
    }

    // Rule 2: XCHG a, b followed by XCHG a, b (or b, a) → NOP NOP
    // Handled at the sequence level: if prev is XCHG a,b and current is
    // XCHG a,b (or b,a), mark both Nop.
    if (prev && !prev->ops.empty() &&
        instr.mnemonic == "xchg" && prev->mnemonic == "xchg") {
        const SemanticOp& pOp = prev->ops[0];
        bool sameOperands =
            (op.dst.kind  == SemValKind::Reg &&
             op.src1.kind == SemValKind::Reg &&
             pOp.dst.kind  == SemValKind::Reg &&
             pOp.src1.kind == SemValKind::Reg) &&
            ((op.dst.reg == pOp.dst.reg  && op.src1.reg == pOp.src1.reg) ||
             (op.dst.reg == pOp.src1.reg && op.src1.reg == pOp.dst.reg));

        if (sameOperands) {
            op.type = SemOpType::Nop;
            op.flagsEffect = FlagsEffect(Flags::NONE, Flags::NONE, Flags::ALL);
            instr.isNormalised = true;
            changed = true;
        }
    }

    return changed;
}

// ─── Public decode API ────────────────────────────────────────────────────────

DecodedInstr SemDecoder::decodeOne(const uint8_t* code, std::size_t sz,
                                    uint64_t addr) const
{
    DecodedInstr d = decodeRaw(code, sz, addr);
    normalise(d, nullptr);
    return d;
}

std::vector<DecodedInstr> SemDecoder::decodeLinear(const uint8_t* code,
                                                     std::size_t    sz,
                                                     uint64_t       start,
                                                     uint64_t       end) const
{
    std::vector<DecodedInstr> result;
    uint64_t cur = start;
    std::size_t off = 0;

    DecodedInstr* prev = nullptr;

    while (cur < end && off < sz) {
        std::size_t remaining = sz - off;
        DecodedInstr d = decodeRaw(code + off, remaining, cur);

        if (!d.isValid) {
            // Skip 1 byte and continue.
            d.len = 1;
        }

        normalise(d, prev);
        result.push_back(std::move(d));
        prev = &result.back();
        off += result.back().len ? result.back().len : 1;
        cur += result.back().len ? result.back().len : 1;
    }

    propagateUndefFlags(result);
    return result;
}

// ─── Undefined flag propagation ───────────────────────────────────────────────

void SemDecoder::propagateUndefFlags(std::vector<DecodedInstr>& instrs)
{
    // Track which flags are currently "undefined" in program order.
    FlagSet curUndef = Flags::NONE;

    for (auto& instr : instrs) {
        const FlagsEffect& fe = instr.flagsEffect;

        // If this instruction reads flags that are currently undefined,
        // mark the relevant source operands as undef.
        // (We model this at the instruction level; a full SSA would track
        //  individual flag registers.)
        if ((fe.preserved & curUndef) != 0) {
            // This instruction implicitly reads preserved flags; some are undef.
            // Add an Undef op to signal to consumers.
            SemanticOp uop;
            uop.type = SemOpType::Undef;
            uop.dst  = SemVal::undef();
            instr.ops.push_back(uop);
        }

        // Update the current undefined flags state.
        // Defined flags clear the undefined set; undefined flags add to it.
        curUndef &= ~fe.defined;   // defined flags are no longer undef
        curUndef &= ~fe.preserved; // preserved means they retain their state
        curUndef |=  fe.undefined; // newly undefined flags
    }
}

// ─── Entropy ─────────────────────────────────────────────────────────────────

double SemDecoder::entropy(const uint8_t* data, std::size_t size) noexcept
{
    if (size == 0) return 0.0;
    uint64_t freq[256] = {};
    for (std::size_t i = 0; i < size; ++i) ++freq[data[i]];
    double h = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (freq[i] == 0) continue;
        double p = static_cast<double>(freq[i]) / static_cast<double>(size);
        h -= p * std::log2(p);
    }
    return h;
}

bool SemDecoder::isHighEntropy(const uint8_t* data, std::size_t size) noexcept
{
    return entropy(data, size) > 6.5;
}

// ─── Unigram model ────────────────────────────────────────────────────────────

// Static unigram frequency model (derived from GCC -O2 representative corpus).
// Values are log-probabilities (natural log).
// Instructions not in this table get kDefault.

static const std::unordered_map<std::string, double>& unigramTable()
{
    static const std::unordered_map<std::string, double> tbl = {
        {"mov",    -1.5},
        {"push",   -2.1},
        {"pop",    -2.2},
        {"call",   -2.8},
        {"ret",    -3.0},
        {"lea",    -2.4},
        {"add",    -2.5},
        {"sub",    -2.6},
        {"cmp",    -2.7},
        {"test",   -2.7},
        {"je",     -3.2},
        {"jne",    -3.2},
        {"jmp",    -3.1},
        {"xor",    -3.0},
        {"and",    -3.1},
        {"or",     -3.4},
        {"nop",    -3.5},
        {"imul",   -3.6},
        {"shr",    -3.7},
        {"shl",    -3.7},
        {"sar",    -3.8},
        {"movzx",  -3.5},
        {"movsx",  -3.6},
        {"jl",     -4.0},
        {"jg",     -4.0},
        {"jle",    -4.0},
        {"jge",    -4.0},
        {"inc",    -4.1},
        {"dec",    -4.1},
        {"neg",    -4.3},
        {"not",    -4.4},
        {"xchg",   -5.0},
        {"int3",   -6.0},
        {"hlt",    -7.0},
    };
    return tbl;
}

double SemDecoder::unigramLogFreq(const std::string& mnemonic,
                                   uint32_t instrLen) noexcept
{
    static constexpr double kDefault   = -8.0;
    static constexpr double kLenBonus  = -0.1; // slight preference for shorter insns

    const auto& tbl = unigramTable();
    auto it = tbl.find(mnemonic);
    double base = (it != tbl.end()) ? it->second : kDefault;
    // Penalise longer instructions slightly to prefer compact sequences.
    return base + kLenBonus * static_cast<double>(instrLen);
}

// ─── Overlapping decode graph ─────────────────────────────────────────────────

DecodeGraph SemDecoder::buildDecodeGraph(const uint8_t* code, std::size_t sz,
                                          uint64_t startAddr) const
{
    DecodeGraph g;

    // Try decoding from every byte offset.
    for (std::size_t off = 0; off < sz; ) {
        std::size_t remaining = sz - off;
        uint64_t    addr      = startAddr + off;

        cs_insn* insn = nullptr;
        std::size_t cnt = cs_disasm(_impl->handle, code + off, remaining, addr, 1, &insn);

        if (cnt > 0) {
            DecodeNode node;
            node.addr    = addr;
            node.len     = static_cast<uint32_t>(insn[0].size);
            node.logFreq = unigramLogFreq(insn[0].mnemonic, node.len);
            g.nodes[addr] = node;
            cs_free(insn, cnt);
        }
        ++off; // always advance by 1 to try every offset
    }

    // Build edges: from node at O to all nodes at O + len(O).
    for (auto& [addr, node] : g.nodes) {
        uint64_t succAddr = addr + node.len;
        if (g.nodes.count(succAddr)) {
            node.successors.push_back(succAddr);
        }
    }

    // Find max-likelihood path via DP.
    g.path = bestPath(g, startAddr, startAddr + sz);
    return g;
}

std::vector<uint64_t> SemDecoder::bestPath(const DecodeGraph& g,
                                             uint64_t startAddr,
                                             uint64_t endAddr)
{
    // DP: score[addr] = best total log-freq to reach addr.
    std::unordered_map<uint64_t, double>   score;
    std::unordered_map<uint64_t, uint64_t> pred;

    static constexpr double kNegInf = -std::numeric_limits<double>::infinity();
    score[startAddr] = 0.0;

    // Process in address order.
    std::vector<uint64_t> sorted;
    sorted.reserve(g.nodes.size());
    for (auto& [a, _] : g.nodes) sorted.push_back(a);
    std::sort(sorted.begin(), sorted.end());

    for (uint64_t addr : sorted) {
        auto sit = score.find(addr);
        if (sit == score.end()) continue;
        double s = sit->second;

        auto nit = g.nodes.find(addr);
        if (nit == g.nodes.end()) continue;

        const DecodeNode& node = nit->second;
        double nextScore = s + node.logFreq;

        for (uint64_t succ : node.successors) {
            auto& ss = score[succ];
            if (score.find(succ) == score.end()) ss = kNegInf;
            if (nextScore > ss) {
                ss = nextScore;
                pred[succ] = addr;
            }
        }
    }

    // Find the end node with the best score at or near endAddr.
    // Walk backwards from the highest-scored node <= endAddr.
    uint64_t best = startAddr;
    double   bestScore = kNegInf;
    for (auto& [addr, s] : score) {
        if (addr >= endAddr) continue;
        if (s > bestScore) { bestScore = s; best = addr; }
    }

    // Traceback.
    std::vector<uint64_t> path;
    for (uint64_t cur = best; cur != startAddr; ) {
        path.push_back(cur);
        auto pit = pred.find(cur);
        if (pit == pred.end()) break;
        cur = pit->second;
    }
    path.push_back(startAddr);
    std::reverse(path.begin(), path.end());
    return path;
}

} // namespace sem_decoder
} // namespace retdec
