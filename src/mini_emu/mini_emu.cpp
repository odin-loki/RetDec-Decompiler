/**
 * @file src/mini_emu/mini_emu.cpp
 * @brief Emulation-bounded x86-64 unpacker engine.
 *
 * This is a minimal interpretive x86-64 emulator focused on unpacker loops.
 * It implements a representative subset of instructions: MOV, arithmetic,
 * bitwise, PUSH/POP, CALL/RET, conditional jumps, LOOP, REP STOSB/MOVSB,
 * and special-purpose instructions (RDTSC, CPUID, INT).
 *
 * The interpreter executes instructions one at a time in a fetch-decode-execute
 * loop, checking termination conditions after each instruction.
 */

#include <memory>
#include "retdec/mini_emu/mini_emu.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>

namespace retdec {
namespace mini_emu {

// ─── String helpers ───────────────────────────────────────────────────────────

std::string stopReasonToString(StopReason r)
{
    switch (r) {
        case StopReason::EnteredNewCode:   return "EnteredNewCode";
        case StopReason::VProtectExec:     return "VProtectExec";
        case StopReason::MaxInstructions:  return "MaxInstructions";
        case StopReason::Halt:             return "Halt";
        case StopReason::Error:            return "Error";
    }
    return "Unknown";
}

// ─── Memory subsystem ────────────────────────────────────────────────────────

struct MemMap {
    std::map<uint64_t, MemPage> pages; ///< key = page-aligned VA

    static uint64_t pageBase(uint64_t va) { return va & ~(uint64_t)(kPageSize - 1); }
    static uint64_t pageOff (uint64_t va) { return va &  (uint64_t)(kPageSize - 1); }

    MemPage *findPage(uint64_t va) {
        auto it = pages.find(pageBase(va));
        return (it != pages.end()) ? &it->second : nullptr;
    }
    const MemPage *findPage(uint64_t va) const {
        auto it = pages.find(pageBase(va));
        return (it != pages.end()) ? &it->second : nullptr;
    }

    void mapPage(uint64_t va, PagePerms perms, const uint8_t *data = nullptr, size_t size = kPageSize) {
        uint64_t base = pageBase(va);
        auto &pg = pages[base];
        pg.data.assign(kPageSize, 0);
        pg.perms = perms;
        if (data && size > 0) {
            size_t copyLen = std::min(size, kPageSize);
            std::memcpy(pg.data.data(), data, copyLen);
        }
    }

    bool read(uint64_t va, uint8_t &out) const {
        const auto *pg = findPage(va);
        if (!pg || !pg->perms.read) return false;
        out = pg->data[pageOff(va)];
        return true;
    }

    bool write(uint64_t va, uint8_t val) {
        auto *pg = findPage(va);
        if (!pg || !pg->perms.write) return false;
        pg->data[pageOff(va)] = val;
        pg->wasWritten = true;
        return true;
    }

    bool fetch(uint64_t va, uint8_t &out) {
        auto *pg = findPage(va);
        if (!pg || !pg->perms.execute) return false;
        pg->wasExecuted = true;
        out = pg->data[pageOff(va)];
        return true;
    }

    bool readU16(uint64_t va, uint16_t &out) const {
        uint8_t lo, hi;
        if (!read(va, lo) || !read(va+1, hi)) return false;
        out = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        return true;
    }
    bool readU32(uint64_t va, uint32_t &out) const {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) if (!read(va+i, b[i])) return false;
        out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1])<<8) |
              (static_cast<uint32_t>(b[2])<<16) | (static_cast<uint32_t>(b[3])<<24);
        return true;
    }
    bool readU64(uint64_t va, uint64_t &out) const {
        uint32_t lo, hi;
        if (!readU32(va, lo) || !readU32(va+4, hi)) return false;
        out = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        return true;
    }
    bool writeU64(uint64_t va, uint64_t val) {
        for (int i = 0; i < 8; ++i) {
            if (!write(va+i, static_cast<uint8_t>(val >> (8*i)))) return false;
        }
        return true;
    }
    bool writeU32(uint64_t va, uint32_t val) {
        for (int i = 0; i < 4; ++i) {
            if (!write(va+i, static_cast<uint8_t>(val >> (8*i)))) return false;
        }
        return true;
    }
};

// ─── Reg helpers ─────────────────────────────────────────────────────────────

static uint64_t &regRef(CPUState &cpu, int reg64)
{
    switch (reg64 & 15) {
        case 0: return cpu.rax; case 1: return cpu.rcx; case 2: return cpu.rdx;
        case 3: return cpu.rbx; case 4: return cpu.rsp; case 5: return cpu.rbp;
        case 6: return cpu.rsi; case 7: return cpu.rdi; case 8: return cpu.r8;
        case 9: return cpu.r9;  case 10: return cpu.r10; case 11: return cpu.r11;
        case 12: return cpu.r12; case 13: return cpu.r13; case 14: return cpu.r14;
        default: return cpu.r15;
    }
}

// ─── Flags ───────────────────────────────────────────────────────────────────

static void setZSF(CPUState &cpu, uint64_t result, int bits)
{
    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1);
    uint64_t r    = result & mask;
    // ZF
    if (r == 0) cpu.rflags |=  (1ULL << 6);
    else        cpu.rflags &= ~(1ULL << 6);
    // SF
    uint64_t sign = 1ULL << (bits - 1);
    if (r & sign) cpu.rflags |=  (1ULL << 7);
    else          cpu.rflags &= ~(1ULL << 7);
}

static void setAdd(CPUState &cpu, uint64_t a, uint64_t b, uint64_t r, int bits)
{
    setZSF(cpu, r, bits);
    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1);
    // CF: carry out
    bool cf = (r & mask) < (a & mask);
    if (cf) cpu.rflags |=  1ULL; else cpu.rflags &= ~1ULL;
    // OF: signed overflow
    bool of = !((a ^ b) & (1ULL << (bits-1))) && ((a ^ r) & (1ULL << (bits-1)));
    if (of) cpu.rflags |=  (1ULL << 11); else cpu.rflags &= ~(1ULL << 11);
}

static void setSub(CPUState &cpu, uint64_t a, uint64_t b, uint64_t r, int bits)
{
    setZSF(cpu, r, bits);
    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1);
    bool cf = (a & mask) < (b & mask);
    if (cf) cpu.rflags |=  1ULL; else cpu.rflags &= ~1ULL;
    bool of = ((a ^ b) & (1ULL << (bits-1))) && ((a ^ r) & (1ULL << (bits-1)));
    if (of) cpu.rflags |=  (1ULL << 11); else cpu.rflags &= ~(1ULL << 11);
}

static bool evalCond(const CPUState &cpu, uint8_t cond)
{
    bool zf = (cpu.rflags >> 6) & 1;
    bool sf = (cpu.rflags >> 7) & 1;
    bool of = (cpu.rflags >> 11) & 1;
    bool cf = cpu.rflags & 1;
    switch (cond & 0xF) {
        case 0: return  of;         // JO
        case 1: return !of;         // JNO
        case 2: return  cf;         // JB/JNAE/JC
        case 3: return !cf;         // JAE/JNB/JNC
        case 4: return  zf;         // JE/JZ
        case 5: return !zf;         // JNE/JNZ
        case 6: return  cf || zf;   // JBE/JNA
        case 7: return !cf && !zf;  // JA/JNBE
        case 8: return  sf;         // JS
        case 9: return !sf;         // JNS
        case 10:return  (cpu.rflags >> 2) & 1; // JP/JPE (PF)
        case 11:return !((cpu.rflags >> 2) & 1); // JNP/JPO
        case 12:return  sf != of;   // JL/JNGE
        case 13:return  sf == of;   // JGE/JNL
        case 14:return  zf || (sf != of); // JLE/JNG
        case 15:return !zf && (sf == of); // JG/JNLE
    }
    return false;
}

// ─── Impl ────────────────────────────────────────────────────────────────────

struct MiniEmu::Impl {
    MemMap   mem;
    CPUState cpu;
    std::vector<uint64_t> origExecPages; ///< page bases of original executable sections

    bool isOrigExec(uint64_t va) const {
        uint64_t base = MemMap::pageBase(va);
        for (uint64_t p : origExecPages) if (p == base) return true;
        return false;
    }

    // ── Byte fetch with RIP advance ──────────────────────────────────────────
    bool fetchByte(uint8_t &b) {
        if (!mem.fetch(cpu.rip, b)) return false;
        ++cpu.rip;
        return true;
    }

    bool fetchU32(uint32_t &v) {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) { if (!fetchByte(b[i])) return false; }
        v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1])<<8) |
            (static_cast<uint32_t>(b[2])<<16) | (static_cast<uint32_t>(b[3])<<24);
        return true;
    }
    bool fetchU64(uint64_t &v) {
        uint32_t lo, hi;
        if (!fetchU32(lo) || !fetchU32(hi)) return false;
        v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi)<<32);
        return true;
    }

    int32_t fetchSImm8() { uint8_t b; fetchByte(b); return static_cast<int8_t>(b); }
    int32_t fetchSImm32() { uint32_t v; fetchU32(v); return static_cast<int32_t>(v); }

    // ── PUSH / POP ────────────────────────────────────────────────────────────
    bool push(uint64_t v) {
        cpu.rsp -= 8;
        return mem.writeU64(cpu.rsp, v);
    }
    bool pop(uint64_t &v) {
        bool ok = mem.readU64(cpu.rsp, v);
        cpu.rsp += 8;
        return ok;
    }

    // ── ModRM / SIB helpers (simplified) ─────────────────────────────────────
    // Returns effective address for memory operand.  Sets *isReg if ModRM encodes
    // a register (mod == 3).  *reg is the /r field (upper reg).
    bool decodeModRM(bool rexB, bool rexR, bool rexX,
                     int &reg, bool &isReg, uint64_t &ea)
    {
        uint8_t modrm; if (!fetchByte(modrm)) return false;
        int mod = modrm >> 6;
        int rm  = modrm & 7;
        reg     = ((modrm >> 3) & 7) | (rexR ? 8 : 0);
        if (rexB) rm |= 8;

        isReg = (mod == 3);
        if (isReg) { ea = rm; return true; }

        // Compute base from rm
        uint64_t base = 0;
        bool hasSIB   = (rm & 7) == 4;
        bool hasDisp8 = (mod == 1);
        bool hasDisp32 = (mod == 2);
        bool rip_rel  = false;

        if (hasSIB) {
            uint8_t sib; if (!fetchByte(sib)) return false;
            int scale  = 1 << (sib >> 6);
            int idx    = (sib >> 3) & 7; if (rexX) idx |= 8;
            int bsib   = sib & 7;        if (rexB) bsib |= 8;
            if (bsib == 5 && mod == 0) {
                // disp32 with no base
                int32_t disp; uint32_t d; if (!fetchU32(d)) return false;
                disp = static_cast<int32_t>(d);
                base = (idx != 4) ? (regRef(cpu, idx) * scale) : 0;
                base += static_cast<uint64_t>(static_cast<int64_t>(disp));
                ea = base; return true;
            }
            base = regRef(cpu, bsib);
            if (idx != 4) base += regRef(cpu, idx) * scale;
        } else if ((rm & 7) == 5 && mod == 0) {
            // RIP-relative
            rip_rel = true;
        } else {
            base = regRef(cpu, rm);
        }

        int64_t disp = 0;
        if (hasDisp8) disp = static_cast<int8_t>(fetchSImm8());
        else if (hasDisp32) { uint32_t d; if (!fetchU32(d)) return false; disp = static_cast<int32_t>(d); }
        else if (rip_rel) { uint32_t d; if (!fetchU32(d)) return false; disp = static_cast<int32_t>(d); base = cpu.rip; }

        ea = base + static_cast<uint64_t>(disp);
        return true;
    }

    // ── Main execute-one-instruction ────────────────────────────────────────

    /**
     * Executes one instruction.  Returns false on hard error.
     * Sets *newTarget and *isJump if a CALL/JMP was decoded.
     */
    bool execOne(StopReason &stop, bool &stopped)
    {
        stopped = false;
        uint8_t rex = 0;
        uint8_t b;

        // Read prefix bytes. rep/opsz must be declared before retry_prefix so
        // they are not re-initialized to false on each iteration of the loop.
        bool rep  = false;
        bool opsz = false;
    retry_prefix:
        if (!fetchByte(b)) { stop = StopReason::Error; stopped = true; return false; }

        // REX prefix
        if (b >= 0x40 && b <= 0x4F) { rex = b; goto retry_prefix; }

        bool rexW = (rex & 8) != 0;
        bool rexR = (rex & 4) != 0;
        bool rexX = (rex & 2) != 0;
        bool rexB = (rex & 1) != 0;

        // LOCK prefix (ignore)
        if (b == 0xF0) goto retry_prefix;
        // REP/REPNZ prefix
        if (b == 0xF3 || b == 0xF2) { rep = true; goto retry_prefix; }
        // Segment overrides (ignore)
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x64 || b == 0x65) goto retry_prefix;
        // Operand-size override (simplified: treat as 16-bit but we'll use 32)
        if (b == 0x66) { opsz = true; goto retry_prefix; }
        // Address-size override (ignore)
        if (b == 0x67) goto retry_prefix;

        int opBits = rexW ? 64 : (opsz ? 16 : 32);

        // ─── Two-byte opcodes (0F xx) ───────────────────────────────────────
        if (b == 0x0F) {
            uint8_t op2; if (!fetchByte(op2)) return false;

            // 0F 31: RDTSC → synthetic monotonic counter
            if (op2 == 0x31) {
                uint64_t tsc = cpu.tscBase;
                cpu.tscBase += cpu.tscStep;
                cpu.rdx = tsc >> 32;
                cpu.rax = tsc & 0xFFFFFFFF;
                return true;
            }

            // 0F A2: CPUID → fake Intel Core i7
            if (op2 == 0xA2) {
                switch (cpu.rax & 0xFFFFFFFF) {
                    case 0: cpu.rax = 0x0B; cpu.rbx = 0x756E6547; cpu.rcx = 0x6C65746E; cpu.rdx = 0x49656E69; break;
                    case 1: cpu.rax = 0x000106E5; cpu.rbx = 0; cpu.rcx = 0x0098E3FD; cpu.rdx = 0xBFEBFBFF; break;
                    default: cpu.rax = cpu.rbx = cpu.rcx = cpu.rdx = 0; break;
                }
                return true;
            }

            // 0F 80..8F: Jcc near rel32
            if (op2 >= 0x80 && op2 <= 0x8F) {
                int32_t rel; uint32_t r; if (!fetchU32(r)) return false;
                rel = static_cast<int32_t>(r);
                if (evalCond(cpu, op2 & 0xF))
                    cpu.rip = static_cast<uint64_t>(static_cast<int64_t>(cpu.rip) + rel);
                return true;
            }

            // 0F B6: MOVZX r32, r/m8
            if (op2 == 0xB6 || op2 == 0xB7) {
                int reg; bool isReg; uint64_t ea;
                if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
                uint8_t v8 = 0;
                if (isReg) { v8 = static_cast<uint8_t>(regRef(cpu, static_cast<int>(ea))); }
                else { mem.read(ea, v8); }
                regRef(cpu, reg) = static_cast<uint64_t>(v8);
                return true;
            }

            // 0F BE: MOVSX r32, r/m8
            if (op2 == 0xBE || op2 == 0xBF) {
                int reg; bool isReg; uint64_t ea;
                if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
                uint8_t v8 = 0;
                if (isReg) v8 = static_cast<uint8_t>(regRef(cpu, static_cast<int>(ea)));
                else mem.read(ea, v8);
                regRef(cpu, reg) = rexW
                    ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v8)))
                    : static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int8_t>(v8)));
                return true;
            }

            return true; // skip unknown 2-byte
        }

        // ─── Single-byte opcodes ───────────────────────────────────────────

        switch (b) {

        // NOP
        case 0x90: return true;

        // HLT
        case 0xF4: stop = StopReason::Halt; stopped = true; return true;

        // INT3 / INT n
        case 0xCC: return true; // treat as NOP
        case 0xCD: { uint8_t v; fetchByte(v); (void)v; return true; }

        // PUSH r64 (50..57)
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: {
            int r = (b & 7) | (rexB ? 8 : 0);
            return push(regRef(cpu, r));
        }

        // POP r64 (58..5F)
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            int r = (b & 7) | (rexB ? 8 : 0);
            uint64_t v; if (!pop(v)) return false;
            regRef(cpu, r) = v;
            return true;
        }

        // CALL rel32
        case 0xE8: {
            int32_t rel; uint32_t r; if (!fetchU32(r)) return false;
            rel = static_cast<int32_t>(r);
            uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(cpu.rip) + rel);
            if (!push(cpu.rip)) return false;
            cpu.rip = target;
            // Termination check: jumping to non-original, written region
            const MemPage *pg = mem.findPage(target);
            if (!pg) { stop = StopReason::EnteredNewCode; stopped = true; return true; }
            if (pg->wasWritten && !isOrigExec(target)) {
                stop = StopReason::EnteredNewCode; stopped = true;
            }
            return true;
        }

        // RET near
        case 0xC3: case 0xC2: {
            if (b == 0xC2) { uint8_t lo, hi; fetchByte(lo); fetchByte(hi); (void)lo; (void)hi; }
            uint64_t ra; if (!pop(ra)) return true; // stack underflow → just stop
            cpu.rip = ra;
            return true;
        }

        // JMP rel8
        case 0xEB: {
            int32_t rel = fetchSImm8();
            cpu.rip = static_cast<uint64_t>(static_cast<int64_t>(cpu.rip) + rel);
            return true;
        }

        // JMP rel32
        case 0xE9: {
            int32_t rel = fetchSImm32();
            uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(cpu.rip) + rel);
            cpu.rip = target;
            const MemPage *pg = mem.findPage(target);
            if (!pg) { stop = StopReason::EnteredNewCode; stopped = true; return true; }
            if (pg->wasWritten && !isOrigExec(target)) {
                stop = StopReason::EnteredNewCode; stopped = true;
            }
            return true;
        }

        // JMP r/m64 (FF /4)
        case 0xFF: {
            uint8_t modrm; if (!fetchByte(modrm)) return false;
            int reg = (modrm >> 3) & 7;
            int mod = modrm >> 6;
            int rm  = modrm & 7;
            if (rexB) rm |= 8;
            if (rexR) reg |= 8;
            if (reg == 4) { // JMP r/m64
                uint64_t target = 0;
                if (mod == 3) {
                    target = regRef(cpu, rm);
                } else {
                    uint64_t ea = regRef(cpu, rm);
                    int64_t disp = 0;
                    if (mod == 1) disp = fetchSImm8();
                    else if (mod == 2) { uint32_t d; fetchU32(d); disp = static_cast<int32_t>(d); }
                    ea += disp;
                    if (!mem.readU64(ea, target)) return false;
                }
                cpu.rip = target;
                const MemPage *pg = mem.findPage(target);
                if (!pg || (pg->wasWritten && !isOrigExec(target))) {
                    stop = StopReason::EnteredNewCode; stopped = true;
                }
            } else if (reg == 2) { // CALL r/m64
                uint64_t target = 0;
                if (mod == 3) {
                    target = regRef(cpu, rm);
                } else {
                    uint64_t ea = regRef(cpu, rm);
                    if (mod == 1) { int32_t d = fetchSImm8(); ea += d; }
                    else if (mod == 2) { uint32_t d; fetchU32(d); ea += static_cast<int32_t>(d); }
                    if (!mem.readU64(ea, target)) return false;
                }
                if (!push(cpu.rip)) return false;
                cpu.rip = target;
                const MemPage *pg = mem.findPage(target);
                if (!pg || (pg->wasWritten && !isOrigExec(target))) {
                    stop = StopReason::EnteredNewCode; stopped = true;
                }
            } else if (reg == 6) { // PUSH r/m64
                uint64_t val = 0;
                if (mod == 3) val = regRef(cpu, rm);
                else {
                    uint64_t ea = regRef(cpu, rm);
                    if (mod == 1) { int32_t d = fetchSImm8(); ea += d; }
                    else if (mod == 2) { uint32_t d; fetchU32(d); ea += static_cast<int32_t>(d); }
                    if (!mem.readU64(ea, val)) return false;
                }
                push(val);
            }
            return true;
        }

        // Jcc short (70..7F)
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
        case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int32_t rel = fetchSImm8();
            if (evalCond(cpu, b & 0xF))
                cpu.rip = static_cast<uint64_t>(static_cast<int64_t>(cpu.rip) + rel);
            return true;
        }

        // LOOP rel8
        case 0xE2: {
            int32_t rel = fetchSImm8();
            --cpu.rcx;
            if ((cpu.rcx & 0xFFFFFFFF) != 0)
                cpu.rip = static_cast<uint64_t>(static_cast<int64_t>(cpu.rip) + rel);
            return true;
        }

        // MOV r/m, r and r, r/m
        case 0x88: case 0x89: case 0x8A: case 0x8B: {
            bool toRM = (b & 2) == 0;
            int wOp   = (b & 1) ? opBits : 8;
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            if (isReg) {
                // reg-reg move
                if (toRM) regRef(cpu, static_cast<int>(ea)) = regRef(cpu, reg);
                else      regRef(cpu, reg) = regRef(cpu, static_cast<int>(ea));
            } else {
                if (toRM) {
                    uint64_t v = regRef(cpu, reg);
                    if (wOp == 8)  mem.write(ea, static_cast<uint8_t>(v));
                    else if (wOp == 32) mem.writeU32(ea, static_cast<uint32_t>(v));
                    else           mem.writeU64(ea, v);
                } else {
                    uint64_t v = 0;
                    if (wOp == 8)  { uint8_t t; mem.read(ea, t); v = t; }
                    else if (wOp == 32) { uint32_t t; mem.readU32(ea, t); v = t; }
                    else           mem.readU64(ea, v);
                    regRef(cpu, reg) = v;
                }
            }
            return true;
        }

        // MOV r/m8, imm8 (C6 /0)
        case 0xC6: {
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            uint8_t imm; if (!fetchByte(imm)) return false;
            if (isReg) { regRef(cpu, static_cast<int>(ea)) = imm; }
            else       { mem.write(ea, imm); }
            return true;
        }

        // MOV r/m, imm (C7 /0 = imm32)
        case 0xC7: {
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            uint32_t imm; if (!fetchU32(imm)) return false;
            uint64_t v = rexW ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(imm))) : imm;
            if (isReg) regRef(cpu, static_cast<int>(ea)) = v;
            else { if (rexW) mem.writeU64(ea, v); else mem.writeU32(ea, static_cast<uint32_t>(v)); }
            return true;
        }

        // MOV r8, imm8 (B0..B7) — write low byte of register
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            int r = (b & 7) | (rexB ? 8 : 0);
            uint8_t v; if (!fetchByte(v)) return false;
            uint64_t& reg = regRef(cpu, r);
            reg = (reg & ~0xFFULL) | v;
            return true;
        }

        // MOV r64, imm64 / r32, imm32 (B8..BF)
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            int r = (b & 7) | (rexB ? 8 : 0);
            if (rexW) {
                uint64_t v; if (!fetchU64(v)) return false;
                regRef(cpu, r) = v;
            } else {
                uint32_t v; if (!fetchU32(v)) return false;
                regRef(cpu, r) = static_cast<uint64_t>(v);
            }
            return true;
        }

        // ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m, imm8 (83)
        case 0x83: {
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            int32_t imm = fetchSImm8();
            uint64_t &dst = isReg ? regRef(cpu, static_cast<int>(ea)) : *(uint64_t*)nullptr;
            uint64_t v = 0;
            if (!isReg) { if (!mem.readU64(ea, v)) return false; }
            else v = regRef(cpu, static_cast<int>(ea));

            uint64_t imm64 = static_cast<uint64_t>(static_cast<int64_t>(imm));
            uint64_t res = 0;
            switch (reg & 7) {
                case 0: res = v + imm64; setAdd(cpu, v, imm64, res, opBits); break; // ADD
                case 1: res = v | imm64; setZSF(cpu, res, opBits); break; // OR
                case 2: res = v + imm64 + ((cpu.rflags)&1); setAdd(cpu, v, imm64, res, opBits); break; // ADC
                case 3: res = v - imm64 - ((cpu.rflags)&1); setSub(cpu, v, imm64, res, opBits); break; // SBB
                case 4: res = v & imm64; setZSF(cpu, res, opBits); break; // AND
                case 5: res = v - imm64; setSub(cpu, v, imm64, res, opBits); break; // SUB
                case 6: res = v ^ imm64; setZSF(cpu, res, opBits); break; // XOR
                case 7: res = v - imm64; setSub(cpu, v, imm64, res, opBits); return true; // CMP (no write)
            }
            if (isReg) regRef(cpu, static_cast<int>(ea)) = res;
            else mem.writeU64(ea, res);
            return true;
        }

        // ADD r/m, imm32 (81)
        case 0x81: {
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            uint32_t imm; if (!fetchU32(imm)) return false;
            uint64_t v = 0;
            if (!isReg) { if (!mem.readU64(ea, v)) return false; }
            else v = regRef(cpu, static_cast<int>(ea));
            uint64_t imm64 = rexW ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(imm))) : imm;
            uint64_t res = v + imm64;
            setAdd(cpu, v, imm64, res, opBits);
            if (isReg) regRef(cpu, static_cast<int>(ea)) = res;
            else mem.writeU64(ea, res);
            return true;
        }

        // INC/DEC r32 (40..47 / 48..4F) — but 48..4F are REX in 64-bit mode
        // INC/DEC r/m (FE/FF)
        case 0xFE: {
            uint8_t modrm; if (!fetchByte(modrm)) return false;
            int mod = modrm >> 6, rm = modrm & 7, reg2 = (modrm >> 3) & 7;
            if (rexB) rm |= 8;
            if (mod == 3) {
                uint64_t &r = regRef(cpu, rm);
                uint8_t v = static_cast<uint8_t>(r);
                if (reg2 == 0) ++v; else --v;
                r = (r & ~0xFFULL) | v;
            }
            return true;
        }

        // STOSB / STOSD / STOSQ (AA / AB)
        case 0xAA: {
            // STOSB: [RDI] = AL; RDI ± 1
            uint8_t dir = (cpu.rflags >> 10) & 1; // DF
            if (rep) {
                while (cpu.rcx > 0) {
                    if (!mem.write(cpu.rdi, static_cast<uint8_t>(cpu.rax))) break;
                    cpu.rdi += dir ? UINT64_MAX : 1; // +1 or -1
                    --cpu.rcx;
                }
            } else {
                mem.write(cpu.rdi, static_cast<uint8_t>(cpu.rax));
                cpu.rdi += dir ? UINT64_MAX : 1;
            }
            return true;
        }
        case 0xAB: { // STOSD / STOSQ
            uint8_t dir = (cpu.rflags >> 10) & 1;
            int stride = rexW ? 8 : 4;
            if (rep) {
                while (cpu.rcx > 0) {
                    if (rexW) { if (!mem.writeU64(cpu.rdi, cpu.rax)) break; }
                    else      { if (!mem.writeU32(cpu.rdi, static_cast<uint32_t>(cpu.rax))) break; }
                    cpu.rdi += dir ? static_cast<uint64_t>(-stride) : stride;
                    --cpu.rcx;
                }
            } else {
                if (rexW) mem.writeU64(cpu.rdi, cpu.rax);
                else      mem.writeU32(cpu.rdi, static_cast<uint32_t>(cpu.rax));
                cpu.rdi += dir ? static_cast<uint64_t>(-stride) : stride;
            }
            return true;
        }

        // MOVSB / MOVSD / MOVSQ (A4 / A5)
        case 0xA4: {
            uint8_t dir = (cpu.rflags >> 10) & 1;
            if (rep) {
                while (cpu.rcx > 0) {
                    uint8_t v; if (!mem.read(cpu.rsi, v)) break;
                    if (!mem.write(cpu.rdi, v)) break;
                    cpu.rsi += dir ? UINT64_MAX : 1;
                    cpu.rdi += dir ? UINT64_MAX : 1;
                    --cpu.rcx;
                }
            } else {
                uint8_t v; mem.read(cpu.rsi, v); mem.write(cpu.rdi, v);
                cpu.rsi += dir ? UINT64_MAX : 1;
                cpu.rdi += dir ? UINT64_MAX : 1;
            }
            return true;
        }

        // XOR r/m, r (30/31) and r, r/m (32/33)
        case 0x30: case 0x31: case 0x32: case 0x33: {
            bool toRM = (b & 2) == 0;
            int wOp   = (b & 1) ? opBits : 8;
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            uint64_t a = isReg ? regRef(cpu, static_cast<int>(ea)) : 0;
            uint64_t c = regRef(cpu, reg);
            if (!isReg) { if (wOp == 64) mem.readU64(ea, a); else { uint32_t t; mem.readU32(ea, t); a = t; } }
            uint64_t res = toRM ? (a ^ c) : (c ^ a);
            setZSF(cpu, res, wOp);
            if (toRM) { if (isReg) regRef(cpu, static_cast<int>(ea)) = res; else { if (wOp==64) mem.writeU64(ea,res); else mem.writeU32(ea,static_cast<uint32_t>(res)); } }
            else regRef(cpu, reg) = res;
            return true;
        }

        // CLD / STD
        case 0xFC: cpu.rflags &= ~(1ULL << 10); return true;
        case 0xFD: cpu.rflags |=  (1ULL << 10); return true;

        // PUSH imm8 / PUSH imm32
        case 0x6A: { uint8_t imm; fetchByte(imm); push(static_cast<uint64_t>(static_cast<int8_t>(imm))); return true; }
        case 0x68: { uint32_t imm; fetchU32(imm); push(static_cast<uint64_t>(static_cast<int32_t>(imm))); return true; }

        // LEA r64, m (8D)
        case 0x8D: {
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            if (!isReg) regRef(cpu, reg) = ea;
            return true;
        }

        // CMP r/m, r (38/39) / r, r/m (3A/3B)
        case 0x38: case 0x39: case 0x3A: case 0x3B: {
            int wOp = (b & 1) ? opBits : 8;
            int reg; bool isReg; uint64_t ea;
            if (!decodeModRM(rexB, rexR, rexX, reg, isReg, ea)) return false;
            uint64_t a = isReg ? regRef(cpu, static_cast<int>(ea)) : 0;
            if (!isReg) { if (wOp==64) mem.readU64(ea,a); else { uint32_t t; mem.readU32(ea,t); a=t; } }
            uint64_t c = regRef(cpu, reg);
            uint64_t res = ((b&2)==0) ? (a - c) : (c - a);
            setSub(cpu, (b&2)==0 ? a : c, (b&2)==0 ? c : a, res, wOp);
            return true;
        }

        default:
            // Unknown opcode — skip 1 byte (may cause cascading errors)
            return true;
        }
    }
};

// ─── MiniEmu public methods ───────────────────────────────────────────────────

MiniEmu::MiniEmu() : impl_(std::make_unique<Impl>()) {}
MiniEmu::~MiniEmu() = default;

void MiniEmu::mapPage(uint64_t va, PagePerms perms, const uint8_t *data, size_t size)
{
    // Map one or more pages
    uint64_t cur = impl_->mem.pageBase(va);
    size_t remaining = std::max(size, kPageSize);
    size_t copied = 0;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, kPageSize);
        impl_->mem.mapPage(cur, perms, data ? data + copied : nullptr, chunk);
        cur += kPageSize;
        copied += chunk;
        remaining = (remaining > chunk) ? (remaining - chunk) : 0;
    }
}

void MiniEmu::load(const uint8_t *data, size_t size, const FormatResult &fmt)
{
    impl_->origExecPages.clear();

    // Map each section
    for (const auto &sec : fmt.sections) {
        uint64_t va  = sec.virtualAddress;
        size_t   vsz = static_cast<size_t>(sec.virtualSize);
        size_t   fo  = static_cast<size_t>(sec.fileOffset);
        size_t   fsz = static_cast<size_t>(sec.fileSize);

        PagePerms perms;
        perms.read    = sec.isReadable;
        perms.write   = sec.isWritable;
        perms.execute = sec.isExecutable;

        // Map pages for this section
        size_t mapped = 0;
        uint64_t cur  = impl_->mem.pageBase(va);
        while (mapped < vsz) {
            size_t pageOff = (cur < va) ? 0 : static_cast<size_t>(cur - va);
            size_t srcOff  = fo + pageOff;
            size_t copyLen = (srcOff < size && fsz > 0)
                ? std::min({ kPageSize, size - srcOff, fsz - pageOff }) : 0;
            const uint8_t *src = (copyLen > 0) ? (data + srcOff) : nullptr;
            impl_->mem.mapPage(cur, perms, src, kPageSize);

            if (perms.execute) {
                impl_->origExecPages.push_back(cur);
            }
            cur += kPageSize;
            mapped += kPageSize;
        }
    }

    // Map stack
    PagePerms stackPerms{true, true, false};
    for (size_t i = 0; i < kStackSize / kPageSize; ++i) {
        impl_->mem.mapPage(kDefaultStackBase + i * kPageSize, stackPerms);
    }
    impl_->cpu.rsp = kDefaultStackBase + kStackSize - 16;
}

UnpackResult MiniEmu::run(uint64_t entryPoint, uint64_t maxInsns)
{
    UnpackResult result;
    impl_->cpu.rip = entryPoint;
    impl_->cpu.tscBase = 0;

    uint64_t instrCount = 0;
    StopReason stop = StopReason::Error;
    bool stopped = false;

    while (instrCount < maxInsns) {
        if (!impl_->execOne(stop, stopped)) break;
        ++instrCount;
        if (stopped) break;
    }

    if (!stopped) {
        stop = StopReason::MaxInstructions;
    }

    result.stopReason           = stop;
    result.instructionsExecuted = instrCount;
    result.epAfterUnpack        = impl_->cpu.rip;
    result.needsManualReview    = (stop == StopReason::MaxInstructions);
    result.success              = (stop != StopReason::Error);

    // Collect written and/or executed regions
    for (auto &[base, pg] : impl_->mem.pages) {
        if (pg.wasWritten || pg.wasExecuted) {
            UnpackedRegion reg;
            reg.startVA = base;
            reg.bytes   = pg.data;
            reg.isCode  = pg.wasExecuted;
            result.regions.push_back(reg);
        }
    }

    // Build synthetic section table
    if (!result.regions.empty()) {
        // Sort by VA
        std::sort(result.regions.begin(), result.regions.end(),
            [](const UnpackedRegion &a, const UnpackedRegion &b) {
                return a.startVA < b.startVA;
            });

        // Merge contiguous pages into sections
        uint64_t curBase = result.regions[0].startVA;
        bool     curExec = result.regions[0].isCode;
        size_t   curSize = result.regions[0].bytes.size();

        for (size_t ri = 1; ri <= result.regions.size(); ++ri) {
            bool flush = (ri == result.regions.size());
            if (!flush) {
                const auto &r = result.regions[ri];
                if (r.startVA == curBase + curSize) {
                    curSize += r.bytes.size();
                    curExec = curExec || r.isCode;
                    continue;
                }
                flush = true;
            }
            SectionInfo si;
            si.name           = curExec ? ".unpacked_code" : ".unpacked_data";
            si.virtualAddress = curBase;
            si.virtualSize    = curSize;
            si.fileOffset     = 0; // not meaningful for dump
            si.fileSize       = curSize;
            si.isExecutable   = curExec;
            si.isWritable     = true;
            si.isReadable     = true;
            result.syntheticSections.push_back(si);

            if (ri < result.regions.size()) {
                const auto &r = result.regions[ri];
                curBase = r.startVA;
                curExec = r.isCode;
                curSize = r.bytes.size();
            }
        }
    }

    return result;
}

bool MiniEmu::readByte(uint64_t va, uint8_t &out) const { return impl_->mem.read(va, out); }
bool MiniEmu::writeByte(uint64_t va, uint8_t val) { return impl_->mem.write(va, val); }
const CPUState &MiniEmu::cpuState() const { return impl_->cpu; }

} // namespace mini_emu
} // namespace retdec
