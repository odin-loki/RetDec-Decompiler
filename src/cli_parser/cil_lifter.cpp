/**
 * @file src/cli_parser/cil_lifter.cpp
 * @brief CIL instruction decoder and BcCFG builder.
 */

#include "retdec/cli_parser/cil_lifter.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <set>
#include <unordered_map>

namespace retdec {
namespace cli_parser {

// ─── Little-endian read helpers ───────────────────────────────────────────────

uint8_t  CILLifter::r8 (std::span<const uint8_t> c, size_t p) { return c[p]; }
uint16_t CILLifter::r16(std::span<const uint8_t> c, size_t p) {
    return static_cast<uint16_t>(c[p]) | (static_cast<uint16_t>(c[p+1]) << 8);
}
uint32_t CILLifter::r32(std::span<const uint8_t> c, size_t p) {
    return static_cast<uint32_t>(c[p])         |
           (static_cast<uint32_t>(c[p+1]) << 8)  |
           (static_cast<uint32_t>(c[p+2]) << 16) |
           (static_cast<uint32_t>(c[p+3]) << 24);
}
uint64_t CILLifter::r64(std::span<const uint8_t> c, size_t p) {
    return static_cast<uint64_t>(r32(c, p)) |
           (static_cast<uint64_t>(r32(c, p+4)) << 32);
}
float CILLifter::rf32(std::span<const uint8_t> c, size_t p) {
    uint32_t bits = r32(c, p);
    float v; std::memcpy(&v, &bits, 4); return v;
}
double CILLifter::rf64(std::span<const uint8_t> c, size_t p) {
    uint64_t bits = r64(c, p);
    double v; std::memcpy(&v, &bits, 8); return v;
}

// ─── CIL opcode → BcOpcode mapping ───────────────────────────────────────────

// CIL single-byte opcodes (0x00–0xFE are single byte, 0xFE xx are two-byte)
static constexpr uint16_t kFEPrefix = 0xFE00;

BcOpcode CILLifter::mapOpcode(uint16_t cil) {
    switch (cil) {
    case 0x00: return BcOpcode::DOTNET_NOP;
    case 0x01: return BcOpcode::DOTNET_BREAK;
    case 0x02: return BcOpcode::DOTNET_LDARG_0;
    case 0x03: return BcOpcode::DOTNET_LDARG_1;
    case 0x04: return BcOpcode::DOTNET_LDARG_2;
    case 0x05: return BcOpcode::DOTNET_LDARG_3;
    case 0x06: return BcOpcode::DOTNET_LDLOC_0;
    case 0x07: return BcOpcode::DOTNET_LDLOC_1;
    case 0x08: return BcOpcode::DOTNET_LDLOC_2;
    case 0x09: return BcOpcode::DOTNET_LDLOC_3;
    case 0x0A: return BcOpcode::DOTNET_STLOC_0;
    case 0x0B: return BcOpcode::DOTNET_STLOC_1;
    case 0x0C: return BcOpcode::DOTNET_STLOC_2;
    case 0x0D: return BcOpcode::DOTNET_STLOC_3;
    case 0x0E: return BcOpcode::DOTNET_LDARG_S;
    case 0x0F: return BcOpcode::DOTNET_LDARGA_S;
    case 0x10: return BcOpcode::DOTNET_STARG_S;
    case 0x11: return BcOpcode::DOTNET_LDLOC_S;
    case 0x12: return BcOpcode::DOTNET_LDLOCA_S;
    case 0x13: return BcOpcode::DOTNET_STLOC_S;
    case 0x14: return BcOpcode::DOTNET_LDNULL;
    case 0x15: return BcOpcode::DOTNET_LDC_I4_M1;
    case 0x16: return BcOpcode::DOTNET_LDC_I4_0;
    case 0x17: return BcOpcode::DOTNET_LDC_I4_1;
    case 0x18: return BcOpcode::DOTNET_LDC_I4_2;
    case 0x19: return BcOpcode::DOTNET_LDC_I4_3;
    case 0x1A: return BcOpcode::DOTNET_LDC_I4_4;
    case 0x1B: return BcOpcode::DOTNET_LDC_I4_5;
    case 0x1C: return BcOpcode::DOTNET_LDC_I4_6;
    case 0x1D: return BcOpcode::DOTNET_LDC_I4_7;
    case 0x1E: return BcOpcode::DOTNET_LDC_I4_8;
    case 0x1F: return BcOpcode::DOTNET_LDC_I4_S;
    case 0x20: return BcOpcode::DOTNET_LDC_I4;
    case 0x21: return BcOpcode::DOTNET_LDC_I8;
    case 0x22: return BcOpcode::DOTNET_LDC_R4;
    case 0x23: return BcOpcode::DOTNET_LDC_R8;
    case 0x25: return BcOpcode::DOTNET_DUP;
    case 0x26: return BcOpcode::DOTNET_POP;
    case 0x28: return BcOpcode::DOTNET_CALL;
    case 0x29: return BcOpcode::DOTNET_CALLI;
    case 0x2A: return BcOpcode::DOTNET_RET;
    case 0x2B: return BcOpcode::DOTNET_BR_S;
    case 0x2C: return BcOpcode::DOTNET_BRFALSE_S;
    case 0x2D: return BcOpcode::DOTNET_BRTRUE_S;
    case 0x2E: return BcOpcode::DOTNET_BEQ_S;
    case 0x2F: return BcOpcode::DOTNET_BGE_S;
    case 0x30: return BcOpcode::DOTNET_BGT_S;
    case 0x31: return BcOpcode::DOTNET_BLE_S;
    case 0x32: return BcOpcode::DOTNET_BLT_S;
    case 0x33: return BcOpcode::DOTNET_BNE_UN_S;
    case 0x34: return BcOpcode::DOTNET_BGE_UN_S;
    case 0x35: return BcOpcode::DOTNET_BGT_UN_S;
    case 0x36: return BcOpcode::DOTNET_BLE_UN_S;
    case 0x37: return BcOpcode::DOTNET_BLT_UN_S;
    case 0x38: return BcOpcode::DOTNET_BR;
    case 0x39: return BcOpcode::DOTNET_BRFALSE;
    case 0x3A: return BcOpcode::DOTNET_BRTRUE;
    case 0x3B: return BcOpcode::DOTNET_BEQ;
    case 0x3C: return BcOpcode::DOTNET_BGE;
    case 0x3D: return BcOpcode::DOTNET_BGT;
    case 0x3E: return BcOpcode::DOTNET_BLE;
    case 0x3F: return BcOpcode::DOTNET_BLT;
    case 0x40: return BcOpcode::DOTNET_BNE_UN;
    case 0x41: return BcOpcode::DOTNET_BGE_UN;
    case 0x42: return BcOpcode::DOTNET_BGT_UN;
    case 0x43: return BcOpcode::DOTNET_BLE_UN;
    case 0x44: return BcOpcode::DOTNET_BLT_UN;
    case 0x45: return BcOpcode::DOTNET_SWITCH;
    case 0x46: return BcOpcode::DOTNET_LDIND_I1;
    case 0x47: return BcOpcode::DOTNET_LDIND_U1;
    case 0x48: return BcOpcode::DOTNET_LDIND_I2;
    case 0x49: return BcOpcode::DOTNET_LDIND_U2;
    case 0x4A: return BcOpcode::DOTNET_LDIND_I4;
    case 0x4B: return BcOpcode::DOTNET_LDIND_U4;
    case 0x4C: return BcOpcode::DOTNET_LDIND_I8;
    case 0x4D: return BcOpcode::DOTNET_LDIND_I;
    case 0x4E: return BcOpcode::DOTNET_LDIND_R4;
    case 0x4F: return BcOpcode::DOTNET_LDIND_R8;
    case 0x50: return BcOpcode::DOTNET_LDIND_REF;
    case 0x51: return BcOpcode::DOTNET_STIND_REF;
    case 0x52: return BcOpcode::DOTNET_STIND_I1;
    case 0x53: return BcOpcode::DOTNET_STIND_I2;
    case 0x54: return BcOpcode::DOTNET_STIND_I4;
    case 0x55: return BcOpcode::DOTNET_STIND_I8;
    case 0x56: return BcOpcode::DOTNET_STIND_R4;
    case 0x57: return BcOpcode::DOTNET_STIND_R8;
    case 0x58: return BcOpcode::DOTNET_ADD;
    case 0x59: return BcOpcode::DOTNET_SUB;
    case 0x5A: return BcOpcode::DOTNET_MUL;
    case 0x5B: return BcOpcode::DOTNET_DIV;
    case 0x5C: return BcOpcode::DOTNET_DIV_UN;
    case 0x5D: return BcOpcode::DOTNET_REM;
    case 0x5E: return BcOpcode::DOTNET_REM_UN;
    case 0x5F: return BcOpcode::DOTNET_AND;
    case 0x60: return BcOpcode::DOTNET_OR;
    case 0x61: return BcOpcode::DOTNET_XOR;
    case 0x62: return BcOpcode::DOTNET_SHL;
    case 0x63: return BcOpcode::DOTNET_SHR;
    case 0x64: return BcOpcode::DOTNET_SHR_UN;
    case 0x65: return BcOpcode::DOTNET_NEG;
    case 0x66: return BcOpcode::DOTNET_NOT;
    case 0x67: return BcOpcode::DOTNET_CONV_I1;
    case 0x68: return BcOpcode::DOTNET_CONV_I2;
    case 0x69: return BcOpcode::DOTNET_CONV_I4;
    case 0x6A: return BcOpcode::DOTNET_CONV_I8;
    case 0x6B: return BcOpcode::DOTNET_CONV_R4;
    case 0x6C: return BcOpcode::DOTNET_CONV_R8;
    case 0x6D: return BcOpcode::DOTNET_CONV_U4;
    case 0x6E: return BcOpcode::DOTNET_CONV_U8;
    case 0x6F: return BcOpcode::DOTNET_CALLVIRT;
    case 0x70: return BcOpcode::DOTNET_CPOBJ;
    case 0x71: return BcOpcode::DOTNET_LDOBJ;
    case 0x72: return BcOpcode::DOTNET_LDSTR;
    case 0x73: return BcOpcode::DOTNET_NEWOBJ;
    case 0x74: return BcOpcode::DOTNET_CASTCLASS;
    case 0x75: return BcOpcode::DOTNET_ISINST;
    case 0x76: return BcOpcode::DOTNET_CONV_R_UN;
    case 0x79: return BcOpcode::DOTNET_UNBOX;
    case 0x7A: return BcOpcode::DOTNET_THROW;
    case 0x7B: return BcOpcode::DOTNET_LDFLD;
    case 0x7C: return BcOpcode::DOTNET_LDFLDA;
    case 0x7D: return BcOpcode::DOTNET_STFLD;
    case 0x7E: return BcOpcode::DOTNET_LDSFLD;
    case 0x7F: return BcOpcode::DOTNET_LDSFLDA;
    case 0x80: return BcOpcode::DOTNET_STSFLD;
    case 0x81: return BcOpcode::DOTNET_STOBJ;
    case 0x82: return BcOpcode::DOTNET_CONV_OVF_I1_UN;
    case 0x83: return BcOpcode::DOTNET_CONV_OVF_I2_UN;
    case 0x84: return BcOpcode::DOTNET_CONV_OVF_I4_UN;
    case 0x85: return BcOpcode::DOTNET_CONV_OVF_I8_UN;
    case 0x86: return BcOpcode::DOTNET_CONV_OVF_U1_UN;
    case 0x87: return BcOpcode::DOTNET_CONV_OVF_U2_UN;
    case 0x88: return BcOpcode::DOTNET_CONV_OVF_U4_UN;
    case 0x89: return BcOpcode::DOTNET_CONV_OVF_U8_UN;
    case 0x8A: return BcOpcode::DOTNET_CONV_OVF_I_UN;
    case 0x8B: return BcOpcode::DOTNET_CONV_OVF_U_UN;
    case 0x8C: return BcOpcode::DOTNET_BOX;
    case 0x8D: return BcOpcode::DOTNET_NEWARR;
    case 0x8E: return BcOpcode::DOTNET_LDLEN;
    case 0x8F: return BcOpcode::DOTNET_LDELEMA;
    case 0x90: return BcOpcode::DOTNET_LDELEM_I1;
    case 0x91: return BcOpcode::DOTNET_LDELEM_U1;
    case 0x92: return BcOpcode::DOTNET_LDELEM_I2;
    case 0x93: return BcOpcode::DOTNET_LDELEM_U2;
    case 0x94: return BcOpcode::DOTNET_LDELEM_I4;
    case 0x95: return BcOpcode::DOTNET_LDELEM_U4;
    case 0x96: return BcOpcode::DOTNET_LDELEM_I8;
    case 0x97: return BcOpcode::DOTNET_LDELEM_I;
    case 0x98: return BcOpcode::DOTNET_LDELEM_R4;
    case 0x99: return BcOpcode::DOTNET_LDELEM_R8;
    case 0x9A: return BcOpcode::DOTNET_LDELEM_REF;
    case 0x9B: return BcOpcode::DOTNET_STELEM_I;
    case 0x9C: return BcOpcode::DOTNET_STELEM_I1;
    case 0x9D: return BcOpcode::DOTNET_STELEM_I2;
    case 0x9E: return BcOpcode::DOTNET_STELEM_I4;
    case 0x9F: return BcOpcode::DOTNET_STELEM_I8;
    case 0xA0: return BcOpcode::DOTNET_STELEM_R4;
    case 0xA1: return BcOpcode::DOTNET_STELEM_R8;
    case 0xA2: return BcOpcode::DOTNET_STELEM_REF;
    case 0xA3: return BcOpcode::DOTNET_LDELEM;
    case 0xA4: return BcOpcode::DOTNET_STELEM;
    case 0xA5: return BcOpcode::DOTNET_UNBOX_ANY;
    case 0xB3: return BcOpcode::DOTNET_CONV_OVF_I1;
    case 0xB4: return BcOpcode::DOTNET_CONV_OVF_U1;
    case 0xB5: return BcOpcode::DOTNET_CONV_OVF_I2;
    case 0xB6: return BcOpcode::DOTNET_CONV_OVF_U2;
    case 0xB7: return BcOpcode::DOTNET_CONV_OVF_I4;
    case 0xB8: return BcOpcode::DOTNET_CONV_OVF_U4;
    case 0xB9: return BcOpcode::DOTNET_CONV_OVF_I8;
    case 0xBA: return BcOpcode::DOTNET_CONV_OVF_U8;
    case 0xC2: return BcOpcode::DOTNET_REFANYVAL;
    case 0xC3: return BcOpcode::DOTNET_CKFINITE;
    case 0xC6: return BcOpcode::DOTNET_MKREFANY;
    case 0xD0: return BcOpcode::DOTNET_LDTOKEN;
    case 0xD1: return BcOpcode::DOTNET_CONV_U2;
    case 0xD2: return BcOpcode::DOTNET_CONV_U1;
    case 0xD3: return BcOpcode::DOTNET_CONV_I;
    case 0xD4: return BcOpcode::DOTNET_CONV_OVF_I;
    case 0xD5: return BcOpcode::DOTNET_CONV_OVF_U;
    case 0xD6: return BcOpcode::DOTNET_ADD_OVF;
    case 0xD7: return BcOpcode::DOTNET_ADD_OVF_UN;
    case 0xD8: return BcOpcode::DOTNET_MUL_OVF;
    case 0xD9: return BcOpcode::DOTNET_MUL_OVF_UN;
    case 0xDA: return BcOpcode::DOTNET_SUB_OVF;
    case 0xDB: return BcOpcode::DOTNET_SUB_OVF_UN;
    case 0xDC: return BcOpcode::DOTNET_ENDFINALLY;
    case 0xDD: return BcOpcode::DOTNET_LEAVE;
    case 0xDE: return BcOpcode::DOTNET_LEAVE_S;
    case 0xDF: return BcOpcode::DOTNET_STIND_I;
    case 0xE0: return BcOpcode::DOTNET_CONV_U;
    // 0xFE prefix
    case kFEPrefix | 0x01: return BcOpcode::DOTNET_CEQ;
    case kFEPrefix | 0x02: return BcOpcode::DOTNET_CGT;
    case kFEPrefix | 0x03: return BcOpcode::DOTNET_CGT_UN;
    case kFEPrefix | 0x04: return BcOpcode::DOTNET_CLT;
    case kFEPrefix | 0x05: return BcOpcode::DOTNET_CLT_UN;
    case kFEPrefix | 0x06: return BcOpcode::DOTNET_LDFTN;
    case kFEPrefix | 0x07: return BcOpcode::DOTNET_LDVIRTFTN;
    case kFEPrefix | 0x09: return BcOpcode::DOTNET_LDARG;
    case kFEPrefix | 0x0A: return BcOpcode::DOTNET_LDARGA;
    case kFEPrefix | 0x0B: return BcOpcode::DOTNET_STARG;
    case kFEPrefix | 0x0C: return BcOpcode::DOTNET_LDLOC;
    case kFEPrefix | 0x0D: return BcOpcode::DOTNET_LDLOCA;
    case kFEPrefix | 0x0E: return BcOpcode::DOTNET_STLOC;
    case kFEPrefix | 0x0F: return BcOpcode::DOTNET_LOCALLOC;
    case kFEPrefix | 0x11: return BcOpcode::DOTNET_ENDFILTER;
    case kFEPrefix | 0x12: return BcOpcode::DOTNET_UNALIGNED;
    case kFEPrefix | 0x13: return BcOpcode::DOTNET_VOLATILE;
    case kFEPrefix | 0x14: return BcOpcode::DOTNET_TAIL_CALL;
    case kFEPrefix | 0x15: return BcOpcode::DOTNET_INITOBJ;
    case kFEPrefix | 0x16: return BcOpcode::DOTNET_CONSTRAINED;
    case kFEPrefix | 0x17: return BcOpcode::DOTNET_CPBLK;
    case kFEPrefix | 0x18: return BcOpcode::DOTNET_INITBLK;
    case kFEPrefix | 0x19: return BcOpcode::DOTNET_NO;
    case kFEPrefix | 0x1A: return BcOpcode::DOTNET_RETHROW;
    case kFEPrefix | 0x1C: return BcOpcode::DOTNET_SIZEOF;
    case kFEPrefix | 0x1D: return BcOpcode::DOTNET_REFANYTYPE;
    case kFEPrefix | 0x1E: return BcOpcode::DOTNET_READONLY;
    default:               return BcOpcode::DOTNET_NOP;
    }
}

// ─── CILLifter ────────────────────────────────────────────────────────────────

CILLifter::CILLifter(const ITypeNameResolver* resolver, const Options& opts)
    : resolver_(resolver), opts_(opts) {}

// ─── Header parsing ──────────────────────────────────────────────────────────

bool CILLifter::parseHeader(std::span<const uint8_t> body,
                              CILMethodHeader& hdr, size_t& codeStart) {
    if (body.empty()) return false;

    uint8_t b0 = body[0];

    // Tiny format: low 2 bits = 0x2
    if ((b0 & 0x3) == 0x2) {
        hdr.isTiny    = true;
        hdr.maxStack  = 8;
        hdr.codeSize  = static_cast<uint32_t>(b0 >> 2);
        hdr.localVarSigTok = 0;
        hdr.initLocals = false;
        codeStart = 1;
        return true;
    }

    // Fat format: low 4 bits must be 0x3, high 4 bits = header size in dwords
    if ((b0 & 0x3) != 0x3) return false;
    if (body.size() < 12) return false;

    uint16_t flagsAndSize = static_cast<uint16_t>(body[0]) |
                            (static_cast<uint16_t>(body[1]) << 8);
    uint16_t hdrSizeDW = (flagsAndSize >> 12) & 0xF;
    uint16_t flags     = flagsAndSize & 0xFFF;

    hdr.isTiny     = false;
    hdr.maxStack   = static_cast<uint16_t>(body[2]) | (static_cast<uint16_t>(body[3]) << 8);
    hdr.codeSize   = static_cast<uint32_t>(body[4]) |
                     (static_cast<uint32_t>(body[5]) << 8) |
                     (static_cast<uint32_t>(body[6]) << 16) |
                     (static_cast<uint32_t>(body[7]) << 24);
    hdr.localVarSigTok = static_cast<uint32_t>(body[8])  |
                         (static_cast<uint32_t>(body[9])  << 8) |
                         (static_cast<uint32_t>(body[10]) << 16) |
                         (static_cast<uint32_t>(body[11]) << 24);
    hdr.initLocals = (flags & 0x010) != 0;  // CorILMethod_InitLocals

    size_t hdrBytes = static_cast<size_t>(hdrSizeDW) * 4;
    codeStart = hdrBytes;

    // Parse exception handling sections (immediately after code)
    bool hasMoreSections = (flags & 0x008) != 0;  // CorILMethod_MoreSects
    if (hasMoreSections && body.size() > codeStart + hdr.codeSize) {
        size_t sectStart = codeStart + hdr.codeSize;
        // Align to 4 bytes
        sectStart = (sectStart + 3) & ~3ULL;

        while (sectStart < body.size()) {
            uint8_t sectFlags = body[sectStart];
            bool isFat = (sectFlags & 0x40) != 0;
            bool isEH  = (sectFlags & 0x01) != 0;
            bool hasMore = (sectFlags & 0x80) != 0;

            size_t dataSize;
            size_t clauseSize;
            size_t numClauses;

            if (isFat) {
                if (sectStart + 4 > body.size()) break;
                dataSize   = static_cast<size_t>(body[sectStart + 1]) |
                             (static_cast<size_t>(body[sectStart + 2]) << 8) |
                             (static_cast<size_t>(body[sectStart + 3]) << 16);
                clauseSize = 24;
                numClauses = (dataSize - 4) / clauseSize;
                sectStart += 4;
            } else {
                if (sectStart + 4 > body.size()) break;
                dataSize   = body[sectStart + 1];
                clauseSize = 12;
                numClauses = (dataSize - 4) / clauseSize;
                sectStart += 4;
            }

            if (isEH) {
                for (size_t ci = 0; ci < numClauses && sectStart + clauseSize <= body.size(); ++ci) {
                    CILExceptionClause clause;
                    if (isFat) {
                        clause.kind          = static_cast<EHClauseKind>(
                            static_cast<uint32_t>(body[sectStart])        |
                            (static_cast<uint32_t>(body[sectStart + 1]) << 8) |
                            (static_cast<uint32_t>(body[sectStart + 2]) << 16) |
                            (static_cast<uint32_t>(body[sectStart + 3]) << 24));
                        clause.tryOffset     = static_cast<uint32_t>(body[sectStart + 4])  |
                                               (static_cast<uint32_t>(body[sectStart + 5]) << 8) |
                                               (static_cast<uint32_t>(body[sectStart + 6]) << 16) |
                                               (static_cast<uint32_t>(body[sectStart + 7]) << 24);
                        clause.tryLength     = static_cast<uint32_t>(body[sectStart + 8])  |
                                               (static_cast<uint32_t>(body[sectStart + 9]) << 8) |
                                               (static_cast<uint32_t>(body[sectStart + 10]) << 16) |
                                               (static_cast<uint32_t>(body[sectStart + 11]) << 24);
                        clause.handlerOffset = static_cast<uint32_t>(body[sectStart + 12]) |
                                               (static_cast<uint32_t>(body[sectStart + 13]) << 8) |
                                               (static_cast<uint32_t>(body[sectStart + 14]) << 16) |
                                               (static_cast<uint32_t>(body[sectStart + 15]) << 24);
                        clause.handlerLength = static_cast<uint32_t>(body[sectStart + 16]) |
                                               (static_cast<uint32_t>(body[sectStart + 17]) << 8) |
                                               (static_cast<uint32_t>(body[sectStart + 18]) << 16) |
                                               (static_cast<uint32_t>(body[sectStart + 19]) << 24);
                        clause.classToken    = static_cast<uint32_t>(body[sectStart + 20]) |
                                               (static_cast<uint32_t>(body[sectStart + 21]) << 8) |
                                               (static_cast<uint32_t>(body[sectStart + 22]) << 16) |
                                               (static_cast<uint32_t>(body[sectStart + 23]) << 24);
                        clause.filterOffset  = clause.classToken;
                    } else {
                        clause.kind          = static_cast<EHClauseKind>(
                            static_cast<uint16_t>(body[sectStart]) |
                            (static_cast<uint16_t>(body[sectStart + 1]) << 8));
                        clause.tryOffset     = static_cast<uint16_t>(body[sectStart + 2]) |
                                               (static_cast<uint16_t>(body[sectStart + 3]) << 8);
                        clause.tryLength     = body[sectStart + 4];
                        clause.handlerOffset = static_cast<uint16_t>(body[sectStart + 5]) |
                                               (static_cast<uint16_t>(body[sectStart + 6]) << 8);
                        clause.handlerLength = body[sectStart + 7];
                        clause.classToken    = static_cast<uint32_t>(body[sectStart + 8])  |
                                               (static_cast<uint32_t>(body[sectStart + 9]) << 8) |
                                               (static_cast<uint32_t>(body[sectStart + 10]) << 16) |
                                               (static_cast<uint32_t>(body[sectStart + 11]) << 24);
                        clause.filterOffset  = clause.classToken;
                    }
                    hdr.exceptionClauses.push_back(clause);
                    sectStart += clauseSize;
                }
            } else {
                sectStart += dataSize;
            }

            if (!hasMore) break;
            sectStart = (sectStart + 3) & ~3ULL;
        }
    }

    return true;
}

// ─── Instruction decoding ────────────────────────────────────────────────────

bool CILLifter::decodeOne(std::span<const uint8_t> code, size_t& pos,
                           RawInsn& out) const {
    if (pos >= code.size()) return false;
    out.offset = static_cast<uint32_t>(pos);
    out.operands.clear();

    uint8_t b = code[pos++];
    uint16_t cilOpc = b;

    // Two-byte prefix
    if (b == 0xFE) {
        if (pos >= code.size()) return false;
        cilOpc = kFEPrefix | code[pos++];
    }

    out.opcode = mapOpcode(cilOpc);
    out.size   = static_cast<uint32_t>(pos - out.offset);

    // Decode operands based on opcode
    auto addI4 = [&](int32_t v) {
        out.operands.push_back(BcIntOperand{static_cast<int64_t>(v)});
    };
    auto addI8 = [&](int64_t v) {
        out.operands.push_back(BcIntOperand{v});
    };
    auto addLocal = [&](uint32_t idx) {
        out.operands.push_back(BcLocalOperand{idx});
    };
    auto addTok = [&](uint32_t tok) {
        out.operands.push_back(tokenToOperand(tok));
    };
    auto addBlock = [&](uint32_t target) {
        out.operands.push_back(BcBlockOperand{target});
    };

    switch (cilOpc) {
    // No operand
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x25: case 0x26: case 0x2A:
    case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50:
    case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56:
    case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C:
    case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62:
    case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68:
    case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E:
    case 0x76: case 0x7A: case 0x8E: case 0x9A:
    case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
    case 0xA0: case 0xA1: case 0xA2:
    case kFEPrefix | 0x01: case kFEPrefix | 0x02: case kFEPrefix | 0x03:
    case kFEPrefix | 0x04: case kFEPrefix | 0x05:
    case kFEPrefix | 0x0F: case kFEPrefix | 0x11:
    case kFEPrefix | 0x1A: case kFEPrefix | 0x1D: case kFEPrefix | 0x1E:
        break;

    // InlineI (int32)
    case 0x20: addI4(static_cast<int32_t>(r32(code, pos))); pos += 4; break;
    // InlineI8 (int64)
    case 0x21: addI8(static_cast<int64_t>(r64(code, pos))); pos += 8; break;
    // InlineR4 (float32)
    case 0x22: out.operands.push_back(BcIntOperand{0}); pos += 4; break;
    // InlineR8 (float64)
    case 0x23: out.operands.push_back(BcIntOperand{0}); pos += 8; break;
    // ShortInlineI (int8)
    case 0x1F: addI4(static_cast<int8_t>(code[pos++])); break;

    // ShortInlineVar (uint8 local/arg)
    case 0x0E: case 0x0F: case 0x10: addLocal(code[pos++]); break;
    case 0x11: case 0x12: case 0x13: addLocal(code[pos++]); break;

    // InlineVar (uint16 local/arg)
    case kFEPrefix | 0x09: case kFEPrefix | 0x0A: case kFEPrefix | 0x0B:
        addLocal(r16(code, pos)); pos += 2; break;
    case kFEPrefix | 0x0C: case kFEPrefix | 0x0D: case kFEPrefix | 0x0E:
        addLocal(r16(code, pos)); pos += 2; break;

    // ShortInlineBrTarget (int8 relative offset)
    case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x35: case 0x36: case 0x37: case 0xDE: {
        int8_t delta = static_cast<int8_t>(code[pos++]);
        addBlock(static_cast<uint32_t>(
            static_cast<int64_t>(pos) + delta));
        break;
    }
    // InlineBrTarget (int32 relative offset)
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C:
    case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41:
    case 0x42: case 0x43: case 0x44: case 0xDD: {
        int32_t delta = static_cast<int32_t>(r32(code, pos)); pos += 4;
        addBlock(static_cast<uint32_t>(
            static_cast<int64_t>(pos) + delta));
        break;
    }

    // InlineSwitch
    case 0x45: {
        uint32_t n = r32(code, pos); pos += 4;
        uint32_t afterSwitch = static_cast<uint32_t>(pos + n * 4);
        for (uint32_t i = 0; i < n && pos + 4 <= code.size(); ++i) {
            int32_t delta = static_cast<int32_t>(r32(code, pos)); pos += 4;
            addBlock(static_cast<uint32_t>(
                static_cast<int64_t>(afterSwitch) + delta));
        }
        break;
    }

    // InlineMethod / InlineField / InlineType / InlineSig / InlineTok / InlineString
    case 0x28: case 0x29: case 0x6F: case 0x73: case 0x74: case 0x75:
    case 0x79: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
    case 0x80: case 0x81: case 0x8C: case 0x8D: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
    case 0x96: case 0x97: case 0x98: case 0x99:
    case 0xA3: case 0xA4: case 0xA5:
    case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: case 0xB8:
    case 0xD0: case 0x70: case 0x71: case 0x72:
    case kFEPrefix | 0x06: case kFEPrefix | 0x07:
    case kFEPrefix | 0x15: case kFEPrefix | 0x16:
    case kFEPrefix | 0x1C: {
        uint32_t tok = r32(code, pos); pos += 4;
        if (cilOpc == 0x72)  // ldstr uses #US token
            out.operands.push_back(BcStringOperand{"<string:" + std::to_string(tok) + ">"});
        else
            addTok(tok);
        break;
    }

    // InlineI8 for leave (same as InlineBrTarget but leave can be far)
    // Handled above in InlineBrTarget block

    // Prefix opcodes with 1-byte operand (unaligned / no)
    case kFEPrefix | 0x12: case kFEPrefix | 0x19:
        addI4(code[pos++]); break;

    // kFEPrefix | 0x16 already handled above in InlineMethod block

    default:
        break;
    }

    out.size = static_cast<uint32_t>(pos - out.offset);
    return true;
}

BcOperand CILLifter::tokenToOperand(uint32_t token) const {
    uint8_t tableId = static_cast<uint8_t>(token >> 24);
    uint32_t idx    = token & 0x00FFFFFF;

    if (tableId == static_cast<uint8_t>(TableId::MethodDef) ||
        tableId == static_cast<uint8_t>(TableId::MemberRef)) {
        BcMethodRef ref;
        ref.owner = "<class>";
        ref.name  = tokenToString(token);
        return ref;
    }
    if (tableId == static_cast<uint8_t>(TableId::Field)) {
        BcFieldRef ref;
        ref.owner = "<class>";
        ref.name  = tokenToString(token);
        return ref;
    }
    if (tableId == static_cast<uint8_t>(TableId::TypeDef) ||
        tableId == static_cast<uint8_t>(TableId::TypeRef) ||
        tableId == static_cast<uint8_t>(TableId::TypeSpec)) {
        BcTypeOperand to;
        BcRefType rt;
        rt.kind = BcRefKind::Class;
        rt.className = tokenToString(token);
        to.type = BcType{rt};
        return to;
    }
    // Default: store as integer
    return BcIntOperand{static_cast<int64_t>(idx)};
}

std::string CILLifter::tokenToString(uint32_t token) const {
    if (!resolver_) return "token_" + std::to_string(token);
    uint8_t tableId = static_cast<uint8_t>(token >> 24);
    uint32_t idx    = token & 0x00FFFFFF;
    if (tableId == static_cast<uint8_t>(TableId::TypeDef))
        return resolver_->typeDefName(idx);
    if (tableId == static_cast<uint8_t>(TableId::TypeRef))
        return resolver_->typeRefName(idx);
    if (tableId == static_cast<uint8_t>(TableId::TypeSpec))
        return resolver_->typeSpecType(idx).toString();
    return "tok_" + std::to_string(token);
}

std::vector<CILLifter::RawInsn> CILLifter::decodeInstructions(
        std::span<const uint8_t> code) const {
    std::vector<RawInsn> insns;
    size_t pos = 0;
    while (pos < code.size()) {
        RawInsn insn;
        if (!decodeOne(code, pos, insn)) break;
        insns.push_back(std::move(insn));
    }
    return insns;
}

BcCFG CILLifter::buildCFG(
        const std::vector<RawInsn>& insns,
        const CILMethodHeader& hdr) const {
    BcCFG cfg;
    if (insns.empty()) return cfg;

    // Collect leader offsets
    std::set<uint32_t> leaders;
    leaders.insert(0);

    for (const auto& insn : insns) {
        for (const auto& op : insn.operands) {
            if (auto* bb = std::get_if<BcBlockOperand>(&op))
                leaders.insert(bb->blockId);
        }
        // Branches make the next instruction a leader
        switch (insn.opcode) {
        case BcOpcode::DOTNET_BR: case BcOpcode::DOTNET_BR_S:
        case BcOpcode::DOTNET_RET: case BcOpcode::DOTNET_THROW:
        case BcOpcode::DOTNET_RETHROW: case BcOpcode::DOTNET_ENDFINALLY:
        case BcOpcode::DOTNET_ENDFILTER: case BcOpcode::DOTNET_LEAVE:
        case BcOpcode::DOTNET_LEAVE_S:
            leaders.insert(insn.offset + insn.size);
            break;
        case BcOpcode::DOTNET_BRTRUE:  case BcOpcode::DOTNET_BRFALSE:
        case BcOpcode::DOTNET_BRTRUE_S:case BcOpcode::DOTNET_BRFALSE_S:
        case BcOpcode::DOTNET_BEQ:     case BcOpcode::DOTNET_BEQ_S:
        case BcOpcode::DOTNET_BNE_UN:  case BcOpcode::DOTNET_BNE_UN_S:
        case BcOpcode::DOTNET_BGT:     case BcOpcode::DOTNET_BGT_S:
        case BcOpcode::DOTNET_BGE:     case BcOpcode::DOTNET_BGE_S:
        case BcOpcode::DOTNET_BLT:     case BcOpcode::DOTNET_BLT_S:
        case BcOpcode::DOTNET_BLE:     case BcOpcode::DOTNET_BLE_S:
        case BcOpcode::DOTNET_BGT_UN:  case BcOpcode::DOTNET_BGT_UN_S:
        case BcOpcode::DOTNET_BGE_UN:  case BcOpcode::DOTNET_BGE_UN_S:
        case BcOpcode::DOTNET_BLT_UN:  case BcOpcode::DOTNET_BLT_UN_S:
        case BcOpcode::DOTNET_BLE_UN:  case BcOpcode::DOTNET_BLE_UN_S:
        case BcOpcode::DOTNET_SWITCH:
            leaders.insert(insn.offset + insn.size);
            break;
        default: break;
        }
    }

    // Add EH handler offsets as leaders
    for (const auto& clause : hdr.exceptionClauses) {
        leaders.insert(clause.handlerOffset);
        leaders.insert(clause.tryOffset);
        if (clause.kind == EHClauseKind::Filter)
            leaders.insert(clause.filterOffset);
    }

    // Build offset → block index map
    std::unordered_map<uint32_t, uint32_t> offsetToBlock;
    std::vector<uint32_t> leaderVec(leaders.begin(), leaders.end());
    for (uint32_t i = 0; i < leaderVec.size(); ++i)
        offsetToBlock[leaderVec[i]] = i;

    // Create blocks
    for (size_t i = 0; i < leaderVec.size(); ++i) {
        BcBasicBlock& blk = cfg.addBlock();
        blk.id = static_cast<uint32_t>(i);
    }

    // Populate blocks with instructions
    size_t blockIdx = 0;
    for (const auto& insn : insns) {
        // Advance block if this instruction is a leader
        auto it = offsetToBlock.find(insn.offset);
        if (it != offsetToBlock.end())
            blockIdx = it->second;

        BcInstruction bcInsn;
        bcInsn.opcode   = insn.opcode;
        bcInsn.operands = insn.operands;
        cfg.blocks()[blockIdx].instrs.push_back(std::move(bcInsn));
    }

    // Wire successors
    for (size_t bi = 0; bi < cfg.blocks().size(); ++bi) {
        auto& blk = cfg.blocks()[bi];
        if (blk.instrs.empty()) continue;
        const auto& lastInsn = blk.instrs.back();

        bool isUnconditionalBranch = false;
        bool isReturn = false;

        for (const auto& op : lastInsn.operands) {
            if (const auto* bb = std::get_if<BcBlockOperand>(&op)) {
                auto tit = offsetToBlock.find(bb->blockId);
                if (tit != offsetToBlock.end())
                    blk.succs.push_back(tit->second);
            }
        }

        switch (lastInsn.opcode) {
        case BcOpcode::DOTNET_BR: case BcOpcode::DOTNET_BR_S:
        case BcOpcode::DOTNET_LEAVE: case BcOpcode::DOTNET_LEAVE_S:
            isUnconditionalBranch = true;
            break;
        case BcOpcode::DOTNET_RET: case BcOpcode::DOTNET_THROW:
        case BcOpcode::DOTNET_RETHROW: case BcOpcode::DOTNET_ENDFINALLY:
        case BcOpcode::DOTNET_ENDFILTER:
            isReturn = true;
            break;
        default: break;
        }

        // Fall-through
        if (!isUnconditionalBranch && !isReturn && bi + 1 < cfg.blocks().size()) {
            blk.succs.push_back(static_cast<uint32_t>(bi + 1));
        }
    }

    // Wire EH handlers
    for (const auto& clause : hdr.exceptionClauses) {
        auto tryIt = offsetToBlock.find(clause.tryOffset);
        auto hndIt = offsetToBlock.find(clause.handlerOffset);
        if (tryIt == offsetToBlock.end() || hndIt == offsetToBlock.end()) continue;

        BcExceptionHandler eh;
        eh.startOffset  = clause.tryOffset;
        eh.endOffset    = clause.tryOffset + clause.tryLength;
        eh.handlerBlock = hndIt->second;

        if (clause.kind == EHClauseKind::Finally)
            eh.isFinally = true;
        else if (clause.kind == EHClauseKind::Fault)
            eh.isFault   = true;
        else if (clause.classToken != 0) {
            std::string typeName = tokenToString(clause.classToken);
            BcRefType ref;
            ref.kind = BcRefKind::Class;
            ref.className = typeName;
            eh.catchType = BcType{ref};
        }

        cfg.addExceptionHandler(std::move(eh));
    }

    return cfg;
}

BcCFG CILLifter::lift(std::span<const uint8_t> body,
                       CILMethodHeader& outHeader) const {
    error_.clear();
    size_t codeStart = 0;
    if (!parseHeader(body, outHeader, codeStart)) {
        error_ = "Failed to parse method header";
        return {};
    }

    if (codeStart + outHeader.codeSize > body.size()) {
        error_ = "Method code extends past end of buffer";
        return {};
    }

    auto code = body.subspan(codeStart, outHeader.codeSize);
    auto insns = decodeInstructions(code);

    return buildCFG(insns, outHeader);
}

} // namespace cli_parser
} // namespace retdec
