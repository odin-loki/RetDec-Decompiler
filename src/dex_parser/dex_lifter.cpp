/**
 * @file src/dex_parser/dex_lifter.cpp
 * @brief Dalvik bytecode → BcCFG lifter (all 256 Dalvik opcodes).
 *
 * Reference: https://source.android.com/docs/core/runtime/dalvik-bytecode
 *
 * Instruction format encoding (Dalvik):
 *   Each instruction is 1..5 code units (u16 words).
 *   The low byte of word[0] is the opcode.
 *   The high byte and subsequent words carry register/operand data.
 *
 * Register naming: vN (local reg), vA, vB, vC etc. in format tables.
 * We encode register refs as BcOperand::Local{id=regN}.
 */

#include <memory>
#include "retdec/dex_parser/dex_lifter.h"
#include "retdec/bc_module/bc_instr.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <unordered_map>

namespace retdec {
namespace dex_parser {

using namespace bc_module;

// ─── Dalvik opcode constants ─────────────────────────────────────────────────

// Opcode byte values (low byte of first code unit)
enum DalvikOp : uint8_t {
    OP_NOP            = 0x00,
    OP_MOVE           = 0x01, OP_MOVE_FROM16  = 0x02, OP_MOVE_16     = 0x03,
    OP_MOVE_WIDE      = 0x04, OP_MOVE_WIDE16  = 0x05, OP_MOVE_WIDE_16= 0x06,
    OP_MOVE_OBJ       = 0x07, OP_MOVE_OBJ16   = 0x08, OP_MOVE_OBJ_16 = 0x09,
    OP_MOVE_RESULT    = 0x0a, OP_MOVE_RES_WIDE= 0x0b, OP_MOVE_RES_OBJ= 0x0c,
    OP_MOVE_EXCEPTION = 0x0d,
    OP_RETURN_VOID    = 0x0e, OP_RETURN       = 0x0f,
    OP_RETURN_WIDE    = 0x10, OP_RETURN_OBJ   = 0x11,
    OP_CONST_4        = 0x12, OP_CONST_16     = 0x13, OP_CONST       = 0x14,
    OP_CONST_HIGH16   = 0x15,
    OP_CONST_WIDE_16  = 0x16, OP_CONST_WIDE_32= 0x17, OP_CONST_WIDE  = 0x18,
    OP_CONST_WIDE_H16 = 0x19,
    OP_CONST_STR      = 0x1a, OP_CONST_STR_J  = 0x1b,
    OP_CONST_CLASS    = 0x1c,
    OP_MONITOR_ENTER  = 0x1d, OP_MONITOR_EXIT = 0x1e,
    OP_CHECK_CAST     = 0x1f, OP_INSTANCE_OF  = 0x20,
    OP_ARRAY_LEN      = 0x21,
    OP_NEW_INSTANCE   = 0x22, OP_NEW_ARRAY    = 0x23,
    OP_FILLED_NEW_ARR = 0x24, OP_FILLED_NEW_RANGE = 0x25,
    OP_FILL_ARRAY_DATA= 0x26,
    OP_THROW          = 0x27,
    OP_GOTO           = 0x28, OP_GOTO_16      = 0x29, OP_GOTO_32     = 0x2a,
    OP_PACKED_SWITCH  = 0x2b, OP_SPARSE_SWITCH= 0x2c,
    OP_CMPL_FLOAT     = 0x2d, OP_CMPG_FLOAT   = 0x2e,
    OP_CMPL_DOUBLE    = 0x2f, OP_CMPG_DOUBLE  = 0x30,
    OP_CMP_LONG       = 0x31,
    OP_IF_EQ          = 0x32, OP_IF_NE        = 0x33,
    OP_IF_LT          = 0x34, OP_IF_GE        = 0x35,
    OP_IF_GT          = 0x36, OP_IF_LE        = 0x37,
    OP_IF_EQZ         = 0x38, OP_IF_NEZ       = 0x39,
    OP_IF_LTZ         = 0x3a, OP_IF_GEZ       = 0x3b,
    OP_IF_GTZ         = 0x3c, OP_IF_LEZ       = 0x3d,
    // 0x3e-0x43 unused
    OP_AGET           = 0x44, OP_AGET_WIDE    = 0x45, OP_AGET_OBJ    = 0x46,
    OP_AGET_BOOL      = 0x47, OP_AGET_BYTE    = 0x48,
    OP_AGET_CHAR      = 0x49, OP_AGET_SHORT   = 0x4a,
    OP_APUT           = 0x4b, OP_APUT_WIDE    = 0x4c, OP_APUT_OBJ    = 0x4d,
    OP_APUT_BOOL      = 0x4e, OP_APUT_BYTE    = 0x4f,
    OP_APUT_CHAR      = 0x50, OP_APUT_SHORT   = 0x51,
    OP_IGET           = 0x52, OP_IGET_WIDE    = 0x53, OP_IGET_OBJ    = 0x54,
    OP_IGET_BOOL      = 0x55, OP_IGET_BYTE    = 0x56,
    OP_IGET_CHAR      = 0x57, OP_IGET_SHORT   = 0x58,
    OP_IPUT           = 0x59, OP_IPUT_WIDE    = 0x5a, OP_IPUT_OBJ    = 0x5b,
    OP_IPUT_BOOL      = 0x5c, OP_IPUT_BYTE    = 0x5d,
    OP_IPUT_CHAR      = 0x5e, OP_IPUT_SHORT   = 0x5f,
    OP_SGET           = 0x60, OP_SGET_WIDE    = 0x61, OP_SGET_OBJ    = 0x62,
    OP_SGET_BOOL      = 0x63, OP_SGET_BYTE    = 0x64,
    OP_SGET_CHAR      = 0x65, OP_SGET_SHORT   = 0x66,
    OP_SPUT           = 0x67, OP_SPUT_WIDE    = 0x68, OP_SPUT_OBJ    = 0x69,
    OP_SPUT_BOOL      = 0x6a, OP_SPUT_BYTE    = 0x6b,
    OP_SPUT_CHAR      = 0x6c, OP_SPUT_SHORT   = 0x6d,
    OP_INVOKE_VIRTUAL = 0x6e, OP_INVOKE_SUPER = 0x6f,
    OP_INVOKE_DIRECT  = 0x70, OP_INVOKE_STATIC= 0x71,
    OP_INVOKE_INTERFACE=0x72,
    // 0x73 unused
    OP_INVOKE_VIRT_RANGE =0x74, OP_INVOKE_SUPER_RANGE=0x75,
    OP_INVOKE_DIRECT_RANGE=0x76,OP_INVOKE_STATIC_RANGE=0x77,
    OP_INVOKE_IFACE_RANGE =0x78,
    // 0x79-0x7a unused
    OP_NEG_INT        = 0x7b, OP_NOT_INT      = 0x7c,
    OP_NEG_LONG       = 0x7d, OP_NOT_LONG     = 0x7e,
    OP_NEG_FLOAT      = 0x7f, OP_NEG_DOUBLE   = 0x80,
    OP_INT_TO_LONG    = 0x81, OP_INT_TO_FLOAT = 0x82, OP_INT_TO_DOUBLE= 0x83,
    OP_LONG_TO_INT    = 0x84, OP_LONG_TO_FLOAT= 0x85, OP_LONG_TO_DOUBLE=0x86,
    OP_FLOAT_TO_INT   = 0x87, OP_FLOAT_TO_LONG= 0x88, OP_FLOAT_TO_DOUBLE=0x89,
    OP_DOUBLE_TO_INT  = 0x8a, OP_DOUBLE_TO_LONG=0x8b, OP_DOUBLE_TO_FLOAT=0x8c,
    OP_INT_TO_BYTE    = 0x8d, OP_INT_TO_CHAR  = 0x8e, OP_INT_TO_SHORT = 0x8f,
    OP_ADD_INT        = 0x90, OP_SUB_INT      = 0x91,
    OP_MUL_INT        = 0x92, OP_DIV_INT      = 0x93, OP_REM_INT     = 0x94,
    OP_AND_INT        = 0x95, OP_OR_INT       = 0x96, OP_XOR_INT     = 0x97,
    OP_SHL_INT        = 0x98, OP_SHR_INT      = 0x99, OP_USHR_INT    = 0x9a,
    OP_ADD_LONG       = 0x9b, OP_SUB_LONG     = 0x9c,
    OP_MUL_LONG       = 0x9d, OP_DIV_LONG     = 0x9e, OP_REM_LONG    = 0x9f,
    OP_AND_LONG       = 0xa0, OP_OR_LONG      = 0xa1, OP_XOR_LONG    = 0xa2,
    OP_SHL_LONG       = 0xa3, OP_SHR_LONG     = 0xa4, OP_USHR_LONG   = 0xa5,
    OP_ADD_FLOAT      = 0xa6, OP_SUB_FLOAT    = 0xa7,
    OP_MUL_FLOAT      = 0xa8, OP_DIV_FLOAT    = 0xa9, OP_REM_FLOAT   = 0xaa,
    OP_ADD_DOUBLE     = 0xab, OP_SUB_DOUBLE   = 0xac,
    OP_MUL_DOUBLE     = 0xad, OP_DIV_DOUBLE   = 0xae, OP_REM_DOUBLE  = 0xaf,
    OP_ADD_INT_2ADDR  = 0xb0, OP_SUB_INT_2ADDR= 0xb1,
    OP_MUL_INT_2ADDR  = 0xb2, OP_DIV_INT_2ADDR= 0xb3, OP_REM_INT_2ADDR=0xb4,
    OP_AND_INT_2ADDR  = 0xb5, OP_OR_INT_2ADDR = 0xb6, OP_XOR_INT_2ADDR=0xb7,
    OP_SHL_INT_2ADDR  = 0xb8, OP_SHR_INT_2ADDR= 0xb9, OP_USHR_INT_2ADDR=0xba,
    OP_ADD_LONG_2ADDR = 0xbb, OP_SUB_LONG_2ADDR=0xbc,
    OP_MUL_LONG_2ADDR = 0xbd, OP_DIV_LONG_2ADDR=0xbe, OP_REM_LONG_2ADDR=0xbf,
    OP_AND_LONG_2ADDR = 0xc0, OP_OR_LONG_2ADDR =0xc1, OP_XOR_LONG_2ADDR=0xc2,
    OP_SHL_LONG_2ADDR = 0xc3, OP_SHR_LONG_2ADDR=0xc4, OP_USHR_LONG_2ADDR=0xc5,
    OP_ADD_FLOAT_2ADDR= 0xc6, OP_SUB_FLOAT_2ADDR=0xc7,
    OP_MUL_FLOAT_2ADDR= 0xc8, OP_DIV_FLOAT_2ADDR=0xc9, OP_REM_FLOAT_2ADDR=0xca,
    OP_ADD_DBL_2ADDR  = 0xcb, OP_SUB_DBL_2ADDR=0xcc,
    OP_MUL_DBL_2ADDR  = 0xcd, OP_DIV_DBL_2ADDR=0xce, OP_REM_DBL_2ADDR=0xcf,
    OP_ADD_INT_LIT16  = 0xd0, OP_RSUB_INT     = 0xd1,
    OP_MUL_INT_LIT16  = 0xd2, OP_DIV_INT_LIT16= 0xd3, OP_REM_INT_LIT16=0xd4,
    OP_AND_INT_LIT16  = 0xd5, OP_OR_INT_LIT16 = 0xd6, OP_XOR_INT_LIT16=0xd7,
    OP_ADD_INT_LIT8   = 0xd8, OP_RSUB_INT_LIT8= 0xd9,
    OP_MUL_INT_LIT8   = 0xda, OP_DIV_INT_LIT8 = 0xdb, OP_REM_INT_LIT8 =0xdc,
    OP_AND_INT_LIT8   = 0xdd, OP_OR_INT_LIT8  = 0xde, OP_XOR_INT_LIT8 =0xdf,
    OP_SHL_INT_LIT8   = 0xe0, OP_SHR_INT_LIT8 = 0xe1, OP_USHR_INT_LIT8=0xe2,
    // 0xe3-0xf9 unused/internal ART opcodes
    OP_INVOKE_POLYMORPHIC      = 0xfa,
    OP_INVOKE_POLYMORPHIC_RANGE= 0xfb,
    OP_INVOKE_CUSTOM           = 0xfc,
    OP_INVOKE_CUSTOM_RANGE     = 0xfd,
    OP_CONST_METHOD_HANDLE     = 0xfe,
    OP_CONST_METHOD_TYPE       = 0xff,
};

// ─── Code unit size lookup ────────────────────────────────────────────────────

// Returns the instruction size in code units for each opcode.
// 0 = variable/payload (packed-switch, sparse-switch, fill-array-data).
static const uint8_t kInsnSize[256] = {
//  0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
    1, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 1, 1, 1, 1, 1,  // 00-0f
    1, 1, 1, 2, 3, 2, 2, 3, 5, 2, 3, 1, 1, 2, 2, 2,  // 10-1f
    2, 2, 2, 3, 3, 3, 3, 1, 2, 3, 3, 0, 0, 2, 2, 2,  // 20-2f (2b,2c = switch)
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0,  // 30-3f
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 40-4f
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 50-5f
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 60-6f
    3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 1, 1, 1, 1,  // 70-7f
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 80-8f
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 90-9f
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // a0-af
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // b0-bf
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // c0-cf
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // d0-df
    2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // e0-ef
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 3, 3, 2, 2,  // f0-ff
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Convert a DEX type descriptor string to BcType (minimal version for lifter).
static BcType dexDescToType(const std::string& desc) {
    if (desc.empty()) return types::Void();
    switch (desc[0]) {
        case 'V': return types::Void();
        case 'Z': return types::Bool();
        case 'B': return types::Byte();
        case 'S': return types::Short();
        case 'C': return types::Char();
        case 'I': return types::Int();
        case 'J': return types::Long();
        case 'F': return types::Float();
        case 'D': return types::Double();
        default: {
            BcRefType ref;
            ref.kind = BcRefKind::Class;
            ref.className = desc;
            return BcType{ref};
        }
    }
}

// Parse a DEX method descriptor "(params)ret" → BcFuncType (simplified).
static BcFuncType parseDexProto(const std::string& proto) {
    BcFuncType ft;
    if (proto.empty() || proto[0] != '(') {
        ft.returnType = nullptr; // void
        return ft;
    }
    size_t closeP = proto.find(')');
    if (closeP == std::string::npos) { ft.returnType = nullptr; return ft; }
    // Parse params
    size_t i = 1;
    while (i < closeP) {
        char c = proto[i];
        if (c == 'L') {
            size_t end = proto.find(';', i);
            if (end == std::string::npos) break;
            ft.params.push_back(std::make_shared<BcType>(dexDescToType(proto.substr(i, end - i + 1))));
            i = end + 1;
        } else if (c == '[') {
            // Array type — find element
            size_t j = i + 1;
            while (j < closeP && proto[j] == '[') ++j;
            if (j < closeP && proto[j] == 'L') {
                size_t end = proto.find(';', j);
                ft.params.push_back(std::make_shared<BcType>(dexDescToType(proto.substr(i, end - i + 1))));
                i = end + 1;
            } else {
                ft.params.push_back(std::make_shared<BcType>(dexDescToType(proto.substr(i, j - i + 1))));
                i = j + 1;
            }
        } else {
            ft.params.push_back(std::make_shared<BcType>(dexDescToType(std::string(1, c))));
            ++i;
        }
    }
    ft.returnType = std::make_shared<BcType>(dexDescToType(proto.substr(closeP + 1)));
    return ft;
}

static BcOperand makeReg(uint32_t regNum) {
    return BcLocalOperand{regNum};
}
static BcOperand makeInt(int64_t v) {
    return BcIntOperand{v};
}
static BcOperand makeStr(const std::string& s) {
    return BcStringOperand{s};
}
static BcOperand makeTypeRef(const std::string& desc) {
    return BcTypeOperand{dexDescToType(desc)};
}
static BcOperand makeMethodRef(const std::string& owner,
                                const std::string& name,
                                const std::string& proto) {
    BcMethodRef ref;
    ref.owner      = owner;
    ref.name       = name;
    ref.descriptor = parseDexProto(proto);
    return ref;
}
static BcOperand makeFieldRef(const std::string& owner,
                               const std::string& name,
                               const std::string& typeDesc) {
    BcFieldRef ref;
    ref.owner = owner;
    ref.name  = name;
    ref.type  = dexDescToType(typeDesc);
    return ref;
}
static BcOperand makeBlock(uint32_t id) {
    return BcBlockOperand{id};
}

// Extract nibbles from the high byte of word[0]
static uint8_t highA(uint16_t w0) { return (w0 >> 8) & 0xF; }
static uint8_t highB(uint16_t w0) { return (w0 >> 12) & 0xF; }

// ─── DexLifter ───────────────────────────────────────────────────────────────

DexLifter::DexLifter(const DexFile& dexFile, LiftOptions opts)
    : dex_(dexFile), opts_(opts) {}

DexLiftResult DexLifter::lift(const CodeItem& code, uint32_t methodIdx) {
    DexLiftResult result;
    try {
        auto leaders = findLeaders(code);
        buildBlocks(result.cfg, code, leaders);
        if (opts_.emitAnnotations)
            wireExceptions(result.cfg, code, leaders);
    } catch (const std::exception& e) {
        result.status = DexLiftResult::Error;
        result.error  = e.what();
    }
    (void)methodIdx;
    return result;
}

std::vector<uint32_t> DexLifter::findLeaders(const CodeItem& code) const {
    std::set<uint32_t> leaders;
    leaders.insert(0);

    const auto& insns = code.insns;
    const uint32_t total = static_cast<uint32_t>(insns.size());

    uint32_t off = 0;
    while (off < total) {
        uint8_t op = insns[off] & 0xFF;
        uint32_t sz = kInsnSize[op];

        if (sz == 0) {
            // Payload: pseudo-instruction. Size is encoded in payload.
            // packed-switch: word[0]=0x0100, word[1]=size, then entries
            // sparse-switch: word[0]=0x0200, word[1]=size, then keys+targets
            // fill-array-data: word[0]=0x0300, word[1]=elem_width, word[2..3]=num_elems
            uint16_t ident = insns[off];
            if ((ident & 0xFF) == 0x01) {
                // packed-switch payload
                uint16_t n = (off + 1 < total) ? insns[off + 1] : 0;
                sz = 4 + n * 2u;
            } else if ((ident & 0xFF) == 0x02) {
                // sparse-switch payload
                uint16_t n = (off + 1 < total) ? insns[off + 1] : 0;
                sz = 2 + n * 4u;
            } else if ((ident & 0xFF) == 0x03) {
                // fill-array-data payload
                uint16_t elemWidth = (off + 1 < total) ? insns[off + 1] : 1;
                uint32_t numElems  = (off + 3 < total) ?
                    (static_cast<uint32_t>(insns[off + 2]) |
                     (static_cast<uint32_t>(insns[off + 3]) << 16)) : 0;
                sz = 4 + ((numElems * elemWidth + 1) / 2);
            } else {
                sz = 1; // unknown, treat as 1
            }
            off += sz;
            continue;
        }

        switch (op) {
            case OP_GOTO: {
                int8_t offset = static_cast<int8_t>(insns[off] >> 8);
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                leaders.insert(target);
                leaders.insert(off + sz);
                break;
            }
            case OP_GOTO_16: {
                int16_t offset = static_cast<int16_t>(insns[off + 1]);
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                leaders.insert(target);
                leaders.insert(off + sz);
                break;
            }
            case OP_GOTO_32: {
                int32_t offset = static_cast<int32_t>(
                    static_cast<uint32_t>(insns[off + 1]) |
                    (static_cast<uint32_t>(insns[off + 2]) << 16));
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                leaders.insert(target);
                leaders.insert(off + sz);
                break;
            }
            case OP_IF_EQ: case OP_IF_NE: case OP_IF_LT:
            case OP_IF_GE: case OP_IF_GT: case OP_IF_LE:
            case OP_IF_EQZ: case OP_IF_NEZ: case OP_IF_LTZ:
            case OP_IF_GEZ: case OP_IF_GTZ: case OP_IF_LEZ: {
                int16_t offset = static_cast<int16_t>(insns[off + 1]);
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                leaders.insert(target);
                leaders.insert(off + sz);
                break;
            }
            case OP_RETURN_VOID: case OP_RETURN:
            case OP_RETURN_WIDE: case OP_RETURN_OBJ:
            case OP_THROW:
                leaders.insert(off + sz);
                break;
            case OP_PACKED_SWITCH:
            case OP_SPARSE_SWITCH:
                // Payload offset in word[1] relative to current insn
                leaders.insert(off + sz);
                break;
            default:
                break;
        }

        off += sz;
    }

    // Exception handler entries are also leaders
    for (const auto& t : code.tries) {
        leaders.insert(t.startAddr);
        leaders.insert(t.startAddr + t.insnCount);
    }
    for (size_t i = 0; i < code.handlers.handlers.size(); ++i) {
        for (const auto& h : code.handlers.handlers[i])
            leaders.insert(h.addr);
        if (code.handlers.catchAllAddrs[i] != ~0u)
            leaders.insert(code.handlers.catchAllAddrs[i]);
    }

    return std::vector<uint32_t>(leaders.begin(), leaders.end());
}

void DexLifter::buildBlocks(BcCFG& cfg,
                             const CodeItem& code,
                             const std::vector<uint32_t>& leaders) {
    // Map from code-unit offset → block id
    std::unordered_map<uint32_t, BlockId> offsetToBlock;
    for (size_t i = 0; i < leaders.size(); ++i) {
        auto& blk = cfg.addBlock();
        blk.label = "L" + std::to_string(leaders[i]);
        offsetToBlock[leaders[i]] = blk.id;
    }

    const auto& insns = code.insns;
    const uint32_t total = static_cast<uint32_t>(insns.size());

    for (size_t li = 0; li < leaders.size(); ++li) {
        uint32_t start = leaders[li];
        uint32_t end   = (li + 1 < leaders.size()) ? leaders[li + 1] : total;

        if (start >= total)
            continue;

        BcBasicBlock& blkRef = cfg.block(offsetToBlock.at(start));
        BcBasicBlock* blk = &blkRef;

        uint32_t off = start;
        while (off < end && off < total) {
            uint8_t  op = insns[off] & 0xFF;
            uint32_t sz = kInsnSize[op];
            if (sz == 0) {
                // Skip payload blocks
                break;
            }
            uint32_t consumed = decodeInsn(*blk, insns, off, dex_);
            if (consumed == 0)
                consumed = sz ? sz : 1;
            off += consumed;
        }

        // Add fall-through edge if block doesn't end with terminator
        if (li + 1 < leaders.size()) {
            uint32_t nextLeader = leaders[li + 1];
            if (offsetToBlock.count(nextLeader) && !blk->instrs.empty()) {
                auto& lastInsn = blk->instrs.back();
                bool isTerminator =
                    lastInsn.opcode == BcOpcode::DALVIK_GOTO       ||
                    lastInsn.opcode == BcOpcode::DALVIK_RETURN_VOID ||
                    lastInsn.opcode == BcOpcode::DALVIK_RETURN      ||
                    lastInsn.opcode == BcOpcode::DALVIK_THROW;
                if (!isTerminator)
                    cfg.addEdge(blk->id, offsetToBlock.at(nextLeader));
            }
        }

        // Wire branch targets
        if (!blk->instrs.empty()) {
            auto& last = blk->instrs.back();
            for (auto& op : last.operands) {
                if (auto* bop = std::get_if<BcBlockOperand>(&op)) {
                    // Target offset was encoded as blockId during decodeInsn.
                    uint32_t targetOff = bop->blockId;
                    if (offsetToBlock.count(targetOff))
                        cfg.addEdge(blk->id, offsetToBlock.at(targetOff));
                }
            }
        }
    }
}

void DexLifter::wireExceptions(BcCFG& cfg,
                                const CodeItem& code,
                                const std::vector<uint32_t>& leaders) {
    (void)leaders; // Used via offsetToBlock lookup inside buildBlocks
    // For each try entry, locate the handler blocks and register them.
    for (size_t ti = 0; ti < code.tries.size(); ++ti) {
        const TryItem& t = code.tries[ti];
        // Find handler list index via handlerOff (byte offset into handler list)
        // We map by index since we parsed them sequentially.
        if (ti >= code.handlers.handlers.size())
            break;

        BcExceptionHandler eh;
        eh.startOffset  = t.startAddr;
        eh.endOffset    = t.startAddr + t.insnCount;

        // Find the handler block id. We look up by the handler addr.
        // Blocks were labeled "L<offset>".
        for (const auto& handler : code.handlers.handlers[ti]) {
            uint32_t handlerBlock = 0;
            // Search for block by label
            for (const auto& blk : cfg.blocks()) {
                if (blk.label == "L" + std::to_string(handler.addr)) {
                    handlerBlock = blk.id;
                    break;
                }
            }
            if (handler.typeIdx >= 0 &&
                static_cast<uint32_t>(handler.typeIdx) < dex_.typeCount()) {
                BcRefType ref;
                ref.kind = BcRefKind::Class;
                ref.className = dex_.typeName(static_cast<uint32_t>(handler.typeIdx));
                eh.catchType = BcType{ref};
            }
            eh.handlerBlock = handlerBlock;
            cfg.addExceptionHandler(eh);
        }

        if (code.handlers.catchAllAddrs[ti] != ~0u) {
            uint32_t catchAllBlock = 0;
            uint32_t addr = code.handlers.catchAllAddrs[ti];
            for (const auto& blk : cfg.blocks()) {
                if (blk.label == "L" + std::to_string(addr)) {
                    catchAllBlock = blk.id;
                    break;
                }
            }
            BcExceptionHandler catchAll;
            catchAll.startOffset  = t.startAddr;
            catchAll.endOffset    = t.startAddr + t.insnCount;
            catchAll.catchType    = std::nullopt; // empty = catch-all
            catchAll.handlerBlock = catchAllBlock;
            cfg.addExceptionHandler(catchAll);
        }
    }
}

// ─── Instruction decode ───────────────────────────────────────────────────────

uint32_t DexLifter::decodeInsn(BcBasicBlock& blk,
                                const std::vector<uint16_t>& insns,
                                uint32_t off,
                                const DexFile& dex) {
    uint16_t w0 = insns[off];
    uint8_t  op = w0 & 0xFF;
    uint32_t sz = kInsnSize[op];
    if (sz == 0) sz = 1;

    BcInstruction insn;
    insn.id     = static_cast<uint32_t>(blk.instrs.size());
    insn.offset = off * 2u; // byte offset

    auto w = [&](uint32_t i) -> uint16_t {
        return (off + i < insns.size()) ? insns[off + i] : 0;
    };

    // Lambda to build invocation operands (method ref + arg registers)
    auto makeInvoke = [&](BcOpcode opc, uint16_t methIdx,
                          const std::vector<uint32_t>& args) {
        insn.opcode = opc;
        insn.operands.push_back(makeMethodRef(
            dex.methodClass(methIdx), dex.methodName(methIdx), dex.methodProto(methIdx)));
        for (uint32_t r : args)
            insn.operands.push_back(makeReg(r));
    };

    // 35c format: {vC,vD,vE,vF,vG}, method@BBBB
    auto args35c = [&]() -> std::vector<uint32_t> {
        uint8_t count  = (w(0) >> 12) & 0xF;
        uint16_t vidx  = w(2);
        std::vector<uint32_t> args;
        // args are in w(0) high nibbles and w(2)
        uint8_t vC = (w(0) >> 8) & 0xF;
        uint8_t vD = (w(2) >> 0) & 0xF;
        uint8_t vE = (w(2) >> 4) & 0xF;
        uint8_t vF = (w(2) >> 8) & 0xF;
        uint8_t vG = (w(2) >> 12) & 0xF;
        (void)vidx;
        if (count >= 1) args.push_back(vC);
        if (count >= 2) args.push_back(vD);
        if (count >= 3) args.push_back(vE);
        if (count >= 4) args.push_back(vF);
        if (count >= 5) args.push_back(vG);
        return args;
    };

    // 3rc format: {vCCCC .. vNNNN}, method@BBBB
    auto args3rc = [&]() -> std::vector<uint32_t> {
        uint8_t  count = (w(0) >> 8) & 0xFF;
        uint16_t first = w(2);
        std::vector<uint32_t> args;
        for (uint32_t i = 0; i < count; ++i)
            args.push_back(first + i);
        return args;
    };

    uint16_t vA, vB, vC;
    int32_t  litC;

    switch (op) {
        // ── NOP / payload ────────────────────────────────────────────────────
        case OP_NOP:
            insn.opcode = BcOpcode::DALVIK_NOP;
            break;

        // ── MOVE ─────────────────────────────────────────────────────────────
        case OP_MOVE: case OP_MOVE_OBJ:
            insn.opcode = BcOpcode::DALVIK_MOVE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) };
            break;
        case OP_MOVE_FROM16: case OP_MOVE_OBJ16:
            insn.opcode = BcOpcode::DALVIK_MOVE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF), makeReg(w(1)) };
            break;
        case OP_MOVE_16: case OP_MOVE_OBJ_16:
            insn.opcode = BcOpcode::DALVIK_MOVE;
            insn.operands = { makeReg(w(1)), makeReg(w(2)) };
            sz = 3; break;
        case OP_MOVE_WIDE:
            insn.opcode = BcOpcode::DALVIK_MOVE_WIDE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) };
            break;
        case OP_MOVE_WIDE16:
            insn.opcode = BcOpcode::DALVIK_MOVE_WIDE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF), makeReg(w(1)) };
            break;
        case OP_MOVE_WIDE_16:
            insn.opcode = BcOpcode::DALVIK_MOVE_WIDE;
            insn.operands = { makeReg(w(1)), makeReg(w(2)) };
            sz = 3; break;

        // ── MOVE_RESULT ───────────────────────────────────────────────────────
        case OP_MOVE_RESULT: case OP_MOVE_RES_WIDE: case OP_MOVE_RES_OBJ:
            insn.opcode = BcOpcode::DALVIK_MOVE_RESULT;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;
        case OP_MOVE_EXCEPTION:
            insn.opcode = BcOpcode::DALVIK_MOVE_EXCEPTION;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;

        // ── RETURN ────────────────────────────────────────────────────────────
        case OP_RETURN_VOID:
            insn.opcode = BcOpcode::DALVIK_RETURN_VOID;
            break;
        case OP_RETURN: case OP_RETURN_OBJ:
            insn.opcode = BcOpcode::DALVIK_RETURN;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;
        case OP_RETURN_WIDE:
            insn.opcode = BcOpcode::DALVIK_RETURN_WIDE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;

        // ── CONST ─────────────────────────────────────────────────────────────
        case OP_CONST_4:
            insn.opcode = BcOpcode::DALVIK_CONST;
            vA = highA(w0); vB = highB(w0);
            insn.operands = { makeReg(vA),
                              makeInt(static_cast<int64_t>(static_cast<int8_t>(vB << 4) >> 4)) };
            break;
        case OP_CONST_16:
            insn.opcode = BcOpcode::DALVIK_CONST;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(static_cast<int16_t>(w(1)))) };
            break;
        case OP_CONST:
            insn.opcode = BcOpcode::DALVIK_CONST;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(static_cast<int32_t>(
                                  static_cast<uint32_t>(w(1)) |
                                  (static_cast<uint32_t>(w(2)) << 16)))) };
            sz = 3; break;
        case OP_CONST_HIGH16:
            insn.opcode = BcOpcode::DALVIK_CONST;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(static_cast<int32_t>(
                                  static_cast<uint32_t>(w(1)) << 16))) };
            break;
        case OP_CONST_WIDE_16:
            insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(static_cast<int16_t>(w(1)))) };
            break;
        case OP_CONST_WIDE_32:
            insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(static_cast<int32_t>(
                                  static_cast<uint32_t>(w(1)) |
                                  (static_cast<uint32_t>(w(2)) << 16)))) };
            sz = 3; break;
        case OP_CONST_WIDE:
            insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(
                                  static_cast<uint64_t>(w(1)) |
                                  (static_cast<uint64_t>(w(2)) << 16) |
                                  (static_cast<uint64_t>(w(3)) << 32) |
                                  (static_cast<uint64_t>(w(4)) << 48))) };
            sz = 5; break;
        case OP_CONST_WIDE_H16:
            insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int64_t>(
                                  static_cast<uint64_t>(w(1)) << 48)) };
            break;

        // ── CONST_STRING ──────────────────────────────────────────────────────
        case OP_CONST_STR:
            insn.opcode = BcOpcode::DALVIK_CONST_STRING;
            if (opts_.resolveStrings && w(1) < dex.stringCount())
                insn.operands = { makeReg((w0 >> 8) & 0xFF), makeStr(dex.string(w(1))) };
            else
                insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(w(1)) };
            break;
        case OP_CONST_STR_J:
            insn.opcode = BcOpcode::DALVIK_CONST_STRING;
            {
                uint32_t idx = static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16);
                if (opts_.resolveStrings && idx < dex.stringCount())
                    insn.operands = { makeReg((w0 >> 8) & 0xFF), makeStr(dex.string(idx)) };
                else
                    insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(idx) };
                sz = 3;
            }
            break;
        case OP_CONST_CLASS:
            insn.opcode = BcOpcode::DALVIK_CONST_CLASS;
            if (w(1) < dex.typeCount())
                insn.operands = { makeReg((w0 >> 8) & 0xFF), makeTypeRef(dex.typeName(w(1))) };
            else
                insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(w(1)) };
            break;

        // ── MONITOR / CAST / CHECK ────────────────────────────────────────────
        case OP_MONITOR_ENTER:
            insn.opcode = BcOpcode::DALVIK_MONITOR_ENTER;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;
        case OP_MONITOR_EXIT:
            insn.opcode = BcOpcode::DALVIK_MONITOR_EXIT;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;
        case OP_CHECK_CAST:
            insn.opcode = BcOpcode::DALVIK_CHECK_CAST;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "") };
            break;
        case OP_INSTANCE_OF:
            insn.opcode = BcOpcode::DALVIK_INSTANCE_OF;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)),
                              makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "") };
            break;
        case OP_ARRAY_LEN:
            insn.opcode = BcOpcode::DALVIK_ARRAY_LENGTH;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) };
            break;

        // ── NEW ───────────────────────────────────────────────────────────────
        case OP_NEW_INSTANCE:
            insn.opcode = BcOpcode::DALVIK_NEW_INSTANCE;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "") };
            break;
        case OP_NEW_ARRAY:
            insn.opcode = BcOpcode::DALVIK_NEW_ARRAY;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)),
                              makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "") };
            break;
        case OP_FILLED_NEW_ARR:
            insn.opcode = BcOpcode::DALVIK_FILLED_NEW_ARRAY;
            insn.operands = { makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "") };
            for (uint32_t r : args35c()) insn.operands.push_back(makeReg(r));
            break;
        case OP_FILLED_NEW_RANGE:
            insn.opcode = BcOpcode::DALVIK_FILLED_NEW_ARRAY;
            insn.operands = { makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "") };
            for (uint32_t r : args3rc()) insn.operands.push_back(makeReg(r));
            break;
        case OP_FILL_ARRAY_DATA:
            insn.opcode = BcOpcode::DALVIK_FILL_ARRAY_DATA;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int32_t>(
                                  static_cast<uint32_t>(w(1)) |
                                  (static_cast<uint32_t>(w(2)) << 16))) };
            sz = 3; break;

        // ── THROW / GOTO ──────────────────────────────────────────────────────
        case OP_THROW:
            insn.opcode = BcOpcode::DALVIK_THROW;
            insn.operands = { makeReg((w0 >> 8) & 0xFF) };
            break;
        case OP_GOTO:
            insn.opcode = BcOpcode::DALVIK_GOTO;
            {
                int8_t offset = static_cast<int8_t>(w0 >> 8);
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                insn.operands = { makeBlock(static_cast<uint32_t>(target)) };
            }
            break;
        case OP_GOTO_16:
            insn.opcode = BcOpcode::DALVIK_GOTO;
            {
                int16_t offset = static_cast<int16_t>(w(1));
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                insn.operands = { makeBlock(static_cast<uint32_t>(target)) };
            }
            break;
        case OP_GOTO_32:
            insn.opcode = BcOpcode::DALVIK_GOTO;
            {
                int32_t offset = static_cast<int32_t>(
                    static_cast<uint32_t>(w(1)) |
                    (static_cast<uint32_t>(w(2)) << 16));
                uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
                insn.operands = { makeBlock(static_cast<uint32_t>(target)) };
                sz = 3;
            }
            break;

        // ── SWITCH ────────────────────────────────────────────────────────────
        case OP_PACKED_SWITCH: case OP_SPARSE_SWITCH:
            insn.opcode = BcOpcode::DALVIK_SWITCH;
            insn.operands = { makeReg((w0 >> 8) & 0xFF),
                              makeInt(static_cast<int32_t>(
                                  static_cast<uint32_t>(w(1)) |
                                  (static_cast<uint32_t>(w(2)) << 16))) };
            sz = 3; break;

        // ── CMP ───────────────────────────────────────────────────────────────
        case OP_CMPL_FLOAT: case OP_CMPG_FLOAT:
        case OP_CMPL_DOUBLE: case OP_CMPG_DOUBLE: case OP_CMP_LONG:
            insn.opcode = BcOpcode::DALVIK_CMP;
            vA = (w0 >> 8) & 0xFF; vB = w(1) & 0xFF; vC = (w(1) >> 8) & 0xFF;
            insn.operands = { makeReg(vA), makeReg(vB), makeReg(vC), makeInt(op) };
            break;

        // ── IF ────────────────────────────────────────────────────────────────
        case OP_IF_EQ: case OP_IF_NE: case OP_IF_LT:
        case OP_IF_GE: case OP_IF_GT: case OP_IF_LE: {
            insn.opcode = BcOpcode::DALVIK_IF;
            vA = highA(w0); vB = highB(w0);
            int16_t offset = static_cast<int16_t>(w(1));
            uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
            insn.operands = { makeReg(vA), makeReg(vB), makeInt(op),
                              makeBlock(static_cast<uint32_t>(target)) };
            break;
        }
        case OP_IF_EQZ: case OP_IF_NEZ: case OP_IF_LTZ:
        case OP_IF_GEZ: case OP_IF_GTZ: case OP_IF_LEZ: {
            insn.opcode = BcOpcode::DALVIK_IF_Z;
            vA = (w0 >> 8) & 0xFF;
            int16_t offset = static_cast<int16_t>(w(1));
            uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(off) + offset);
            insn.operands = { makeReg(vA), makeInt(op),
                              makeBlock(static_cast<uint32_t>(target)) };
            break;
        }

        // ── ARRAY OPS ─────────────────────────────────────────────────────────
        case OP_AGET: case OP_AGET_WIDE: case OP_AGET_OBJ:
        case OP_AGET_BOOL: case OP_AGET_BYTE: case OP_AGET_CHAR: case OP_AGET_SHORT:
            insn.opcode = BcOpcode::DALVIK_AGET;
            vA = (w0 >> 8) & 0xFF; vB = w(1) & 0xFF; vC = (w(1) >> 8) & 0xFF;
            insn.operands = { makeReg(vA), makeReg(vB), makeReg(vC) };
            break;
        case OP_APUT: case OP_APUT_WIDE: case OP_APUT_OBJ:
        case OP_APUT_BOOL: case OP_APUT_BYTE: case OP_APUT_CHAR: case OP_APUT_SHORT:
            insn.opcode = BcOpcode::DALVIK_APUT;
            vA = (w0 >> 8) & 0xFF; vB = w(1) & 0xFF; vC = (w(1) >> 8) & 0xFF;
            insn.operands = { makeReg(vA), makeReg(vB), makeReg(vC) };
            break;

        // ── IGET / IPUT ───────────────────────────────────────────────────────
        case OP_IGET: case OP_IGET_WIDE: case OP_IGET_OBJ:
        case OP_IGET_BOOL: case OP_IGET_BYTE: case OP_IGET_CHAR: case OP_IGET_SHORT:
            insn.opcode = BcOpcode::DALVIK_IGET;
            vA = highA(w0); vB = highB(w0);
            if (w(1) < dex.fieldCount())
                insn.operands = { makeReg(vA), makeReg(vB),
                    makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1))) };
            else
                insn.operands = { makeReg(vA), makeReg(vB), makeInt(w(1)) };
            break;
        case OP_IPUT: case OP_IPUT_WIDE: case OP_IPUT_OBJ:
        case OP_IPUT_BOOL: case OP_IPUT_BYTE: case OP_IPUT_CHAR: case OP_IPUT_SHORT:
            insn.opcode = BcOpcode::DALVIK_IPUT;
            vA = highA(w0); vB = highB(w0);
            if (w(1) < dex.fieldCount())
                insn.operands = { makeReg(vA), makeReg(vB),
                    makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1))) };
            else
                insn.operands = { makeReg(vA), makeReg(vB), makeInt(w(1)) };
            break;

        // ── SGET / SPUT ───────────────────────────────────────────────────────
        case OP_SGET: case OP_SGET_WIDE: case OP_SGET_OBJ:
        case OP_SGET_BOOL: case OP_SGET_BYTE: case OP_SGET_CHAR: case OP_SGET_SHORT:
            insn.opcode = BcOpcode::DALVIK_SGET;
            if (w(1) < dex.fieldCount())
                insn.operands = { makeReg((w0 >> 8) & 0xFF),
                    makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1))) };
            else
                insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(w(1)) };
            break;
        case OP_SPUT: case OP_SPUT_WIDE: case OP_SPUT_OBJ:
        case OP_SPUT_BOOL: case OP_SPUT_BYTE: case OP_SPUT_CHAR: case OP_SPUT_SHORT:
            insn.opcode = BcOpcode::DALVIK_SPUT;
            if (w(1) < dex.fieldCount())
                insn.operands = { makeReg((w0 >> 8) & 0xFF),
                    makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1))) };
            else
                insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(w(1)) };
            break;

        // ── INVOKE ────────────────────────────────────────────────────────────
        case OP_INVOKE_VIRTUAL:
            makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL,    w(1), args35c()); break;
        case OP_INVOKE_SUPER:
            makeInvoke(BcOpcode::DALVIK_INVOKE_SUPER,      w(1), args35c()); break;
        case OP_INVOKE_DIRECT:
            makeInvoke(BcOpcode::DALVIK_INVOKE_DIRECT,     w(1), args35c()); break;
        case OP_INVOKE_STATIC:
            makeInvoke(BcOpcode::DALVIK_INVOKE_STATIC,     w(1), args35c()); break;
        case OP_INVOKE_INTERFACE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_INTERFACE,  w(1), args35c()); break;
        case OP_INVOKE_VIRT_RANGE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL,    w(1), args3rc()); break;
        case OP_INVOKE_SUPER_RANGE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_SUPER,      w(1), args3rc()); break;
        case OP_INVOKE_DIRECT_RANGE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_DIRECT,     w(1), args3rc()); break;
        case OP_INVOKE_STATIC_RANGE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_STATIC,     w(1), args3rc()); break;
        case OP_INVOKE_IFACE_RANGE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_INTERFACE,  w(1), args3rc()); break;
        case OP_INVOKE_POLYMORPHIC:
            makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL,    w(1), args35c()); break;
        case OP_INVOKE_POLYMORPHIC_RANGE:
            makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL,    w(1), args3rc()); break;
        case OP_INVOKE_CUSTOM:
            insn.opcode = BcOpcode::DALVIK_INVOKE_CUSTOM;
            insn.operands = { makeInt(w(1)) };
            for (uint32_t r : args35c()) insn.operands.push_back(makeReg(r));
            break;
        case OP_INVOKE_CUSTOM_RANGE:
            insn.opcode = BcOpcode::DALVIK_INVOKE_CUSTOM;
            insn.operands = { makeInt(w(1)) };
            for (uint32_t r : args3rc()) insn.operands.push_back(makeReg(r));
            break;

        // ── UNARY OPS ─────────────────────────────────────────────────────────
        case OP_NEG_INT:    insn.opcode = BcOpcode::DALVIK_NEG_INT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_NOT_INT:    insn.opcode = BcOpcode::DALVIK_NOT_INT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_NEG_LONG:   insn.opcode = BcOpcode::DALVIK_NEG_LONG;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_NOT_LONG:   insn.opcode = BcOpcode::DALVIK_NOT_LONG;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_NEG_FLOAT:  insn.opcode = BcOpcode::DALVIK_NEG_FLOAT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_NEG_DOUBLE: insn.opcode = BcOpcode::DALVIK_NEG_DOUBLE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_INT_TO_LONG:    insn.opcode = BcOpcode::DALVIK_INT_TO_LONG;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_INT_TO_FLOAT:   insn.opcode = BcOpcode::DALVIK_INT_TO_FLOAT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_INT_TO_DOUBLE:  insn.opcode = BcOpcode::DALVIK_INT_TO_DOUBLE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_LONG_TO_INT:    insn.opcode = BcOpcode::DALVIK_LONG_TO_INT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_LONG_TO_FLOAT:  insn.opcode = BcOpcode::DALVIK_LONG_TO_FLOAT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_LONG_TO_DOUBLE: insn.opcode = BcOpcode::DALVIK_LONG_TO_DOUBLE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_FLOAT_TO_INT:   insn.opcode = BcOpcode::DALVIK_FLOAT_TO_INT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_FLOAT_TO_LONG:  insn.opcode = BcOpcode::DALVIK_FLOAT_TO_LONG;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_FLOAT_TO_DOUBLE:insn.opcode = BcOpcode::DALVIK_FLOAT_TO_DOUBLE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_DOUBLE_TO_INT:  insn.opcode = BcOpcode::DALVIK_DOUBLE_TO_INT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_DOUBLE_TO_LONG: insn.opcode = BcOpcode::DALVIK_DOUBLE_TO_LONG;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_DOUBLE_TO_FLOAT:insn.opcode = BcOpcode::DALVIK_DOUBLE_TO_FLOAT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_INT_TO_BYTE:    insn.opcode = BcOpcode::DALVIK_INT_TO_BYTE;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_INT_TO_CHAR:    insn.opcode = BcOpcode::DALVIK_INT_TO_CHAR;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;
        case OP_INT_TO_SHORT:   insn.opcode = BcOpcode::DALVIK_INT_TO_SHORT;
            insn.operands = { makeReg(highA(w0)), makeReg(highB(w0)) }; break;

        // ── BINARY OPS (reg/reg form 23x) ─────────────────────────────────────
        case OP_ADD_INT:  insn.opcode = BcOpcode::DALVIK_ADD_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SUB_INT:  insn.opcode = BcOpcode::DALVIK_SUB_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_MUL_INT:  insn.opcode = BcOpcode::DALVIK_MUL_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_DIV_INT:  insn.opcode = BcOpcode::DALVIK_DIV_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_REM_INT:  insn.opcode = BcOpcode::DALVIK_REM_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_AND_INT:  insn.opcode = BcOpcode::DALVIK_AND_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_OR_INT:   insn.opcode = BcOpcode::DALVIK_OR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_XOR_INT:  insn.opcode = BcOpcode::DALVIK_XOR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SHL_INT:  insn.opcode = BcOpcode::DALVIK_SHL_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SHR_INT:  insn.opcode = BcOpcode::DALVIK_SHR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_USHR_INT: insn.opcode = BcOpcode::DALVIK_USHR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;

        case OP_ADD_LONG: insn.opcode=BcOpcode::DALVIK_ADD_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SUB_LONG: insn.opcode=BcOpcode::DALVIK_SUB_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_MUL_LONG: insn.opcode=BcOpcode::DALVIK_MUL_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_DIV_LONG: insn.opcode=BcOpcode::DALVIK_DIV_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_REM_LONG: insn.opcode=BcOpcode::DALVIK_REM_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_AND_LONG: insn.opcode=BcOpcode::DALVIK_AND_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_OR_LONG:  insn.opcode=BcOpcode::DALVIK_OR_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_XOR_LONG: insn.opcode=BcOpcode::DALVIK_XOR_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SHL_LONG: insn.opcode=BcOpcode::DALVIK_SHL_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SHR_LONG: insn.opcode=BcOpcode::DALVIK_SHR_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_USHR_LONG:insn.opcode=BcOpcode::DALVIK_USHR_LONG;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;

        case OP_ADD_FLOAT: insn.opcode=BcOpcode::DALVIK_ADD_FLOAT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SUB_FLOAT: insn.opcode=BcOpcode::DALVIK_SUB_FLOAT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_MUL_FLOAT: insn.opcode=BcOpcode::DALVIK_MUL_FLOAT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_DIV_FLOAT: insn.opcode=BcOpcode::DALVIK_DIV_FLOAT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_REM_FLOAT: insn.opcode=BcOpcode::DALVIK_REM_FLOAT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_ADD_DOUBLE: insn.opcode=BcOpcode::DALVIK_ADD_DOUBLE;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_SUB_DOUBLE: insn.opcode=BcOpcode::DALVIK_SUB_DOUBLE;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_MUL_DOUBLE: insn.opcode=BcOpcode::DALVIK_MUL_DOUBLE;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_DIV_DOUBLE: insn.opcode=BcOpcode::DALVIK_DIV_DOUBLE;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;
        case OP_REM_DOUBLE: insn.opcode=BcOpcode::DALVIK_REM_DOUBLE;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; vC=(w(1)>>8)&0xFF;
            insn.operands={makeReg(vA),makeReg(vB),makeReg(vC)}; break;

        // ── 2ADDR forms (12x): vA op= vB ──────────────────────────────────────
        case OP_ADD_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_ADD_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SUB_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_SUB_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_MUL_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_MUL_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_DIV_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_DIV_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_REM_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_REM_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_AND_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_AND_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_OR_INT_2ADDR:  insn.opcode=BcOpcode::DALVIK_OR_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_XOR_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_XOR_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SHL_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_SHL_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SHR_INT_2ADDR: insn.opcode=BcOpcode::DALVIK_SHR_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_USHR_INT_2ADDR:insn.opcode=BcOpcode::DALVIK_USHR_INT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_ADD_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_ADD_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SUB_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_SUB_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_MUL_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_MUL_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_DIV_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_DIV_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_REM_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_REM_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_AND_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_AND_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_OR_LONG_2ADDR:  insn.opcode=BcOpcode::DALVIK_OR_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_XOR_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_XOR_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SHL_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_SHL_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SHR_LONG_2ADDR: insn.opcode=BcOpcode::DALVIK_SHR_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_USHR_LONG_2ADDR:insn.opcode=BcOpcode::DALVIK_USHR_LONG;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_ADD_FLOAT_2ADDR: insn.opcode=BcOpcode::DALVIK_ADD_FLOAT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SUB_FLOAT_2ADDR: insn.opcode=BcOpcode::DALVIK_SUB_FLOAT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_MUL_FLOAT_2ADDR: insn.opcode=BcOpcode::DALVIK_MUL_FLOAT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_DIV_FLOAT_2ADDR: insn.opcode=BcOpcode::DALVIK_DIV_FLOAT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_REM_FLOAT_2ADDR: insn.opcode=BcOpcode::DALVIK_REM_FLOAT;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_ADD_DBL_2ADDR:   insn.opcode=BcOpcode::DALVIK_ADD_DOUBLE;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_SUB_DBL_2ADDR:   insn.opcode=BcOpcode::DALVIK_SUB_DOUBLE;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_MUL_DBL_2ADDR:   insn.opcode=BcOpcode::DALVIK_MUL_DOUBLE;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_DIV_DBL_2ADDR:   insn.opcode=BcOpcode::DALVIK_DIV_DOUBLE;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;
        case OP_REM_DBL_2ADDR:   insn.opcode=BcOpcode::DALVIK_REM_DOUBLE;
            insn.operands={makeReg(highA(w0)),makeReg(highA(w0)),makeReg(highB(w0))}; break;

        // ── LIT16 forms (22s): vA = vB op lit ─────────────────────────────────
        case OP_ADD_INT_LIT16: insn.opcode=BcOpcode::DALVIK_ADD_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_RSUB_INT: insn.opcode=BcOpcode::DALVIK_RSUB_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_MUL_INT_LIT16: insn.opcode=BcOpcode::DALVIK_MUL_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_DIV_INT_LIT16: insn.opcode=BcOpcode::DALVIK_DIV_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_REM_INT_LIT16: insn.opcode=BcOpcode::DALVIK_REM_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_AND_INT_LIT16: insn.opcode=BcOpcode::DALVIK_AND_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_OR_INT_LIT16:  insn.opcode=BcOpcode::DALVIK_OR_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_XOR_INT_LIT16: insn.opcode=BcOpcode::DALVIK_XOR_INT;
            vA=highA(w0); vB=highB(w0); litC=static_cast<int16_t>(w(1));
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;

        // ── LIT8 forms (22b): vAA = vBB op lit ────────────────────────────────
        case OP_ADD_INT_LIT8: insn.opcode=BcOpcode::DALVIK_ADD_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_RSUB_INT_LIT8: insn.opcode=BcOpcode::DALVIK_RSUB_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_MUL_INT_LIT8: insn.opcode=BcOpcode::DALVIK_MUL_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_DIV_INT_LIT8: insn.opcode=BcOpcode::DALVIK_DIV_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_REM_INT_LIT8: insn.opcode=BcOpcode::DALVIK_REM_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_AND_INT_LIT8: insn.opcode=BcOpcode::DALVIK_AND_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_OR_INT_LIT8:  insn.opcode=BcOpcode::DALVIK_OR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_XOR_INT_LIT8: insn.opcode=BcOpcode::DALVIK_XOR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_SHL_INT_LIT8: insn.opcode=BcOpcode::DALVIK_SHL_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_SHR_INT_LIT8: insn.opcode=BcOpcode::DALVIK_SHR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;
        case OP_USHR_INT_LIT8:insn.opcode=BcOpcode::DALVIK_USHR_INT;
            vA=(w0>>8)&0xFF; vB=w(1)&0xFF; litC=static_cast<int8_t>((w(1)>>8)&0xFF);
            insn.operands={makeReg(vA),makeReg(vB),makeInt(litC)}; break;

        // ── CONST_METHOD_HANDLE / CONST_METHOD_TYPE (DEX 038+) ───────────────
        case OP_CONST_METHOD_HANDLE:
            insn.opcode = BcOpcode::DALVIK_CONST;
            insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(w(1)) };
            break;
        case OP_CONST_METHOD_TYPE:
            insn.opcode = BcOpcode::DALVIK_CONST;
            insn.operands = { makeReg((w0 >> 8) & 0xFF), makeInt(w(1)) };
            break;

        default:
            insn.opcode = BcOpcode::DALVIK_NOP;
            break;
    }

    blk.instrs.push_back(std::move(insn));
    return sz;
}

} // namespace dex_parser
} // namespace retdec
