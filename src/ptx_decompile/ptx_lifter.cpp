/**
 * @file src/ptx_decompile/ptx_lifter.cpp
 * @brief PTX-to-CUDA-C Lifter implementation.
 */

#include "retdec/ptx_decompile/ptx_lifter.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace retdec::ptx_decompile {

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string trimStr(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n,;{}()");
    return s.substr(b, e - b + 1);
}

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// ─── PtxType helpers ──────────────────────────────────────────────────────────

PtxType PtxParser::parseType(const std::string& tok) const {
    static const std::unordered_map<std::string, PtxType> m = {
        {".pred",PtxType::Pred},
        {".b8",PtxType::B8},{".b16",PtxType::B16},{".b32",PtxType::B32},{".b64",PtxType::B64},
        {".s8",PtxType::S8},{".s16",PtxType::S16},{".s32",PtxType::S32},{".s64",PtxType::S64},
        {".u8",PtxType::U8},{".u16",PtxType::U16},{".u32",PtxType::U32},{".u64",PtxType::U64},
        {".f16",PtxType::F16},{".f32",PtxType::F32},{".f64",PtxType::F64},
    };
    auto it = m.find(tok);
    return it != m.end() ? it->second : PtxType::Unknown;
}

PtxSpace PtxParser::parseSpace(const std::string& tok) const {
    static const std::unordered_map<std::string, PtxSpace> m = {
        {".reg",   PtxSpace::Reg},
        {".global",PtxSpace::Global},
        {".shared",PtxSpace::Shared},
        {".local", PtxSpace::Local},
        {".const", PtxSpace::Const},
        {".param", PtxSpace::Param},
    };
    auto it = m.find(tok);
    return it != m.end() ? it->second : PtxSpace::Unknown;
}

// ─── PtxParser tokeniser ──────────────────────────────────────────────────────

std::vector<std::string> PtxParser::tokenise(const std::string& line) {
    std::vector<std::string> toks;
    std::string cur;
    bool inBracket = false;
    for (char c : line) {
        if (c == '[') { inBracket = true; cur += c; continue; }
        if (c == ']') { inBracket = false; cur += c; continue; }
        if (inBracket) { cur += c; continue; }
        if (c == ' ' || c == '\t' || c == ',' || c == ';') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else if (c == '{' || c == '}') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            toks.push_back(std::string(1, c));
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

// ─── PtxParser operand ────────────────────────────────────────────────────────

PtxOperand PtxParser::parseOperand(const std::string& tok) const {
    PtxOperand op;
    if (tok.empty()) return op;

    // Address: [reg+offset] or [symbol]
    if (tok.front() == '[') {
        op.kind = PtxOperandKind::Address;
        std::string inner = tok.substr(1, tok.size() - (tok.back() == ']' ? 2 : 1));
        auto plus = inner.find('+');
        auto minus = inner.find('-', 1);
        if (plus != std::string::npos) {
            op.name   = inner.substr(0, plus);
            op.offset = std::stoll(inner.substr(plus + 1));
        } else if (minus != std::string::npos) {
            op.name   = inner.substr(0, minus);
            op.offset = -std::stoll(inner.substr(minus + 1));
        } else {
            op.name = inner;
        }
        return op;
    }

    // Special registers
    if (startsWith(tok, "%tid") || startsWith(tok, "%ntid") ||
        startsWith(tok, "%ctaid") || startsWith(tok, "%nctaid") ||
        startsWith(tok, "%laneid") || startsWith(tok, "%warpid") ||
        startsWith(tok, "%lanemask") || startsWith(tok, "%smid") ||
        startsWith(tok, "%clock") || startsWith(tok, "%globaltimer")) {
        op.kind = PtxOperandKind::SpecialReg;
        op.name = tok;
        return op;
    }

    // Register (starts with %)
    if (!tok.empty() && tok[0] == '%') {
        op.kind = PtxOperandKind::Register;
        op.name = tok;
        return op;
    }

    // Label (ends with ':' stripped, or is a bare name used as branch target)
    if (!tok.empty() && (std::isalpha(tok[0]) || tok[0] == '_')) {
        op.kind = PtxOperandKind::Label;
        op.name = tok;
        return op;
    }

    // Immediate (hex or decimal)
    op.kind = PtxOperandKind::Immediate;
    try {
        if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
            op.immVal = static_cast<int64_t>(std::stoull(tok, nullptr, 16));
        else if (tok.find('.') != std::string::npos || tok.find('e') != std::string::npos)
        { op.fltVal = std::stod(tok); op.isFloat = true; }
        else
            op.immVal = std::stoll(tok);
    } catch (...) {
        op.kind = PtxOperandKind::Label;
        op.name = tok;
    }
    return op;
}

// ─── PtxParser variable declaration ──────────────────────────────────────────

PtxVarDecl PtxParser::parseVarDecl(const std::vector<std::string>& tokens) const {
    PtxVarDecl decl;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].front() == '.') {
            PtxSpace sp = parseSpace(tokens[i]);
            if (sp != PtxSpace::Unknown) { decl.space = sp; continue; }
            PtxType  ty = parseType(tokens[i]);
            if (ty != PtxType::Unknown) { decl.type = ty; continue; }
            if (startsWith(tokens[i], ".align")) {
                if (i + 1 < tokens.size()) decl.align = std::stoi(tokens[++i]);
                continue;
            }
        }
        // Name with optional count: %r<4>
        if (!tokens[i].empty() && tokens[i][0] == '%') {
            std::string name = tokens[i];
            auto lt = name.find('<');
            if (lt != std::string::npos) {
                auto gt = name.find('>');
                decl.count = std::stoi(name.substr(lt + 1, gt - lt - 1));
                decl.name  = name.substr(0, lt);
            } else {
                decl.name  = name;
                decl.count = 1;
            }
        }
    }
    return decl;
}

// ─── PtxParser param ──────────────────────────────────────────────────────────

PtxParam PtxParser::parseParam(const std::vector<std::string>& tokens) const {
    PtxParam p;
    for (const auto& tok : tokens) {
        if (tok == ".param") { p.space = PtxSpace::Param; continue; }
        PtxType ty = parseType(tok);
        if (ty != PtxType::Unknown) { p.type = ty; continue; }
        if (tok.front() != '.' && tok.front() != '@' && !tok.empty()) {
            p.name = tok;
            if (ty == PtxType::U64 || ty == PtxType::B64)
                p.isPointer = true;
        }
    }
    // Heuristic: 64-bit params in kernel entries are likely pointers
    if (p.type == PtxType::U64 || p.type == PtxType::B64)
        p.isPointer = true;
    return p;
}

// ─── PtxParser instr ─────────────────────────────────────────────────────────

PtxInstr PtxParser::parseInstr(const std::vector<std::string>& tokens) const {
    PtxInstr instr;
    if (tokens.empty()) return instr;

    size_t start = 0;

    // Predicate guard @%p0 or @!%p0
    if (start < tokens.size() && !tokens[start].empty() &&
        tokens[start][0] == '@') {
        std::string guard = tokens[start].substr(1);
        if (!guard.empty() && guard[0] == '!') {
            instr.negPred = true;
            guard = guard.substr(1);
        }
        instr.predReg = guard;
        ++start;
    }

    // Mnemonic (possibly "ld.global.u32" — split on first dot after the base mnemonic)
    if (start < tokens.size()) {
        std::string full = tokens[start];
        ++start;
        // Split into mnemonic + modifier
        // The base mnemonic ends at the first dot
        auto dot = full.find('.');
        if (dot != std::string::npos) {
            instr.mnemonic = full.substr(0, dot);
            instr.modifier = full.substr(dot);
            // Extract type from modifier (last .XX token)
            std::string mod = instr.modifier;
            size_t lastDot = mod.rfind('.');
            if (lastDot != std::string::npos) {
                PtxType ty = parseType(mod.substr(lastDot));
                if (ty != PtxType::Unknown) instr.type = ty;
            }
        } else {
            instr.mnemonic = full;
        }
    }

    // Operands
    for (size_t i = start; i < tokens.size(); ++i)
        instr.operands.push_back(parseOperand(tokens[i]));

    return instr;
}

// ─── PtxParser parse ─────────────────────────────────────────────────────────

PtxModule PtxParser::parse(const std::string& src) {
    PtxModule mod;
    std::istringstream ss(src);
    std::string line;

    PtxKernel* currentKernel = nullptr;
    int braceDepth = 0;
    bool inKernelBody = false;
    bool inParamList  = false;

    while (std::getline(ss, line)) {
        // Strip comment
        auto commentPos = line.find("//");
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);

        // Pre-detect structural characters BEFORE trimStr (which strips '(', ')', '{', '}')
        bool hasOpenParen  = line.find('(') != std::string::npos;
        bool hasCloseParen = line.find(')') != std::string::npos;
        int  netBraces     = 0;
        for (char c : line) {
            if (c == '{') ++netBraces;
            else if (c == '}') --netBraces;
        }

        line = trimStr(line);

        if (line.empty()) {
            // Apply brace changes for lines that trim to empty (lone '{' / '}')
            braceDepth += netBraces;
            if (netBraces > 0 && currentKernel) inKernelBody = true;
            if (braceDepth <= 0 && inKernelBody) {
                inKernelBody = false;
                currentKernel = nullptr;
            }
            if (inParamList && hasCloseParen) inParamList = false;
            continue;
        }

        auto toks = tokenise(line);
        if (toks.empty()) continue;

        // .target
        if (toks[0] == ".target" && toks.size() >= 2) {
            std::string sm = toks[1];
            auto p = sm.find("sm_");
            if (p != std::string::npos) {
                try { mod.smVersion = std::stoi(sm.substr(p + 3)); } catch (...) {}
            }
            continue;
        }

        // .entry or .func
        if ((toks[0] == ".entry" || toks[0] == ".func") ||
            (toks.size() >= 2 &&
             (toks[1] == ".entry" || toks[1] == ".func" ||
              (toks.size() >= 3 && (toks[2] == ".entry" || toks[2] == ".func"))))) {
            PtxKernel k;
            std::string kind;
            std::string name;
            for (size_t i = 0; i < toks.size(); ++i) {
                if (toks[i] == ".entry" || toks[i] == ".func") {
                    kind = toks[i];
                    if (i + 1 < toks.size()) name = toks[i + 1];
                    break;
                }
            }
            // Strip any trailing '(' that the tokeniser attached to the name
            // (happens when params are on the same line: "foo(.param .u32 x)")
            auto parenPos = name.find('(');
            if (parenPos != std::string::npos) name = name.substr(0, parenPos);

            k.kind = (kind == ".func") ? PtxKernelKind::Func : PtxKernelKind::Entry;
            k.name = name;
            mod.kernels.push_back(k);
            currentKernel = &mod.kernels.back();
            // Params appear on separate lines only when '(' is present but ')' is not
            inParamList = hasOpenParen && !hasCloseParen;
            // Apply any brace opened on the same line as the entry (e.g. "entry foo() {")
            braceDepth += netBraces;
            if (braceDepth > 0 && currentKernel) inKernelBody = true;
            continue;
        }

        // Parameter list (inside parentheses for .entry)
        if (inParamList && currentKernel) {
            if (toks[0] == ".param") {
                auto p = parseParam(toks);
                if (!p.name.empty()) currentKernel->params.push_back(p);
            }
            // End param list when ')' was seen in the raw line (trimStr strips it)
            if (hasCloseParen) inParamList = false;
            continue;
        }

        // Apply brace changes from inline '{' / '}' on non-entry lines
        braceDepth += netBraces;
        if (netBraces > 0 && currentKernel) inKernelBody = true;
        if (braceDepth <= 0 && inKernelBody) {
            inKernelBody = false;
            currentKernel = nullptr;
        }

        if (!inKernelBody || !currentKernel) continue;

        // Label
        if (!line.empty() && line.back() == ':') {
            PtxInstr lbl;
            lbl.label    = line.substr(0, line.size() - 1);
            lbl.mnemonic = "__label__";
            currentKernel->instrs.push_back(lbl);
            continue;
        }

        // Variable declaration inside kernel
        if (!toks.empty() && (toks[0] == ".reg"    || toks[0] == ".shared" ||
                               toks[0] == ".local"  || toks[0] == ".param"  ||
                               toks[0] == ".const")) {
            auto decl = parseVarDecl(toks);
            if (!decl.name.empty()) currentKernel->decls.push_back(decl);
            continue;
        }

        // Instruction
        if (!toks.empty()) {
            auto instr = parseInstr(toks);
            if (!instr.mnemonic.empty())
                currentKernel->instrs.push_back(instr);
        }
    }

    return mod;
}

// ─── ThreadIndexRecovery ──────────────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> kSpecialRegs = {
    {"%tid.x",         "threadIdx.x"},
    {"%tid.y",         "threadIdx.y"},
    {"%tid.z",         "threadIdx.z"},
    {"%ntid.x",        "blockDim.x"},
    {"%ntid.y",        "blockDim.y"},
    {"%ntid.z",        "blockDim.z"},
    {"%ctaid.x",       "blockIdx.x"},
    {"%ctaid.y",       "blockIdx.y"},
    {"%ctaid.z",       "blockIdx.z"},
    {"%nctaid.x",      "gridDim.x"},
    {"%nctaid.y",      "gridDim.y"},
    {"%nctaid.z",      "gridDim.z"},
    {"%laneid",        "(threadIdx.x & 31)"},
    {"%warpid",        "(threadIdx.x >> 5)"},
    {"%lanemask_lt",   "__lanemask_lt()"},
    {"%lanemask_eq",   "__lanemask_eq()"},
    {"%lanemask_le",   "__lanemask_le()"},
    {"%lanemask_gt",   "__lanemask_gt()"},
    {"%lanemask_ge",   "__lanemask_ge()"},
    {"%smid",          "0 /* smid */"},
    {"%nsmid",         "0 /* nsmid */"},
    {"%clock",         "clock()"},
    {"%clock64",       "clock64()"},
    {"%globaltimer",   "clock64() /* globaltimer */"},
    {"%pm0",           "0 /* pm0 */"},
};

std::string ThreadIndexRecovery::resolve(const std::string& name) {
    auto it = kSpecialRegs.find(name);
    return it != kSpecialRegs.end() ? it->second : "";
}

bool ThreadIndexRecovery::isSpecial(const std::string& name) {
    return kSpecialRegs.count(name) > 0;
}

// ─── SharedMemDeclaration ─────────────────────────────────────────────────────

static std::string ptxCType(PtxType t) {
    switch (t) {
    case PtxType::Pred: return "int";
    case PtxType::B8:
    case PtxType::U8:   return "unsigned char";
    case PtxType::S8:   return "signed char";
    case PtxType::B16:
    case PtxType::U16:  return "unsigned short";
    case PtxType::S16:  return "short";
    case PtxType::B32:
    case PtxType::U32:  return "unsigned int";
    case PtxType::S32:  return "int";
    case PtxType::B64:
    case PtxType::U64:  return "unsigned long long";
    case PtxType::S64:  return "long long";
    case PtxType::F16:  return "half";
    case PtxType::F32:  return "float";
    case PtxType::F64:  return "double";
    default:            return "int";
    }
}

std::string SharedMemDeclaration::emit(const PtxVarDecl& decl) {
    std::ostringstream os;
    os << "__shared__ " << ptxCType(decl.type) << " "
       << decl.name.substr(1);  // strip leading %
    if (decl.bytes > 0)
        os << "[" << (decl.bytes / std::max<size_t>(1, decl.count)) << "]";
    else if (decl.count > 1)
        os << "[" << decl.count << "]";
    os << ";";
    return os.str();
}

// ─── InstrLifter helpers ──────────────────────────────────────────────────────

std::string InstrLifter::cTypeStr(PtxType t) { return ptxCType(t); }

std::string InstrLifter::typeStr(PtxType t) {
    switch (t) {
    case PtxType::B8:  case PtxType::U8:   return "u8";
    case PtxType::B16: case PtxType::U16:  return "u16";
    case PtxType::B32: case PtxType::U32:  return "u32";
    case PtxType::B64: case PtxType::U64:  return "u64";
    case PtxType::S8:  return "s8";
    case PtxType::S16: return "s16";
    case PtxType::S32: return "s32";
    case PtxType::S64: return "s64";
    case PtxType::F32: return "f32";
    case PtxType::F64: return "f64";
    default: return "?";
    }
}

std::string InstrLifter::operandStr(const PtxOperand& op, PtxType) {
    switch (op.kind) {
    case PtxOperandKind::SpecialReg: {
        std::string s = ThreadIndexRecovery::resolve(op.name);
        return s.empty() ? op.name : s;
    }
    case PtxOperandKind::Register:
        return op.name.substr(1); // strip %
    case PtxOperandKind::Address: {
        std::string base;
        if (ThreadIndexRecovery::isSpecial(op.name))
            base = ThreadIndexRecovery::resolve(op.name);
        else
            base = op.name.empty() ? "" : op.name.substr(op.name[0] == '%' ? 1 : 0);
        if (op.offset > 0)  return "*(" + base + " + " + std::to_string(op.offset) + ")";
        if (op.offset < 0)  return "*(" + base + " - " + std::to_string(-op.offset) + ")";
        return "*" + base;
    }
    case PtxOperandKind::Immediate:
        if (op.isFloat) {
            std::ostringstream os;
            os << op.fltVal;
            return os.str();
        }
        if (op.immVal == 0) return "0";
        return "0x" + [](uint64_t v) {
            std::ostringstream os;
            os << std::hex << v;
            return os.str();
        }(static_cast<uint64_t>(op.immVal));
    case PtxOperandKind::Label:
        return op.name;
    }
    return "?";
}

std::string InstrLifter::sepCmp(const std::string& mod) {
    // Extract comparison from modifier like ".s32.lt" → "<"
    if (mod.find(".eq") != std::string::npos) return "==";
    if (mod.find(".ne") != std::string::npos) return "!=";
    if (mod.find(".lt") != std::string::npos) return "<";
    if (mod.find(".le") != std::string::npos) return "<=";
    if (mod.find(".gt") != std::string::npos) return ">";
    if (mod.find(".ge") != std::string::npos) return ">=";
    return "==";
}

// ─── InstrLifter individual handlers ─────────────────────────────────────────

std::string InstrLifter::liftMov(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 2) return ind + "/* mov */\n";
    std::string dst = operandStr(i.operands[0], i.type);
    std::string src = operandStr(i.operands[1], i.type);
    return ind + dst + " = " + src + ";\n";
}

std::string InstrLifter::liftLoad(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 2) return ind + "/* ld */\n";
    std::string dst = operandStr(i.operands[0], i.type);
    std::string src = operandStr(i.operands[1], i.type);
    // If src is not already dereferenced
    if (!i.operands[1].offset && i.operands[1].kind != PtxOperandKind::Address) {
        src = "*" + src;
    }
    return ind + dst + " = " + src + ";\n";
}

std::string InstrLifter::liftStore(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 2) return ind + "/* st */\n";
    std::string dst = operandStr(i.operands[0], i.type);
    std::string src = operandStr(i.operands[1], i.type);
    if (i.operands[0].kind != PtxOperandKind::Address)
        dst = "*" + dst;
    return ind + dst + " = " + src + ";\n";
}

std::string InstrLifter::liftArith(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 3) return ind + "/* arith */\n";
    std::string dst = operandStr(i.operands[0], i.type);
    std::string a   = operandStr(i.operands[1], i.type);
    std::string b   = operandStr(i.operands[2], i.type);

    static const std::unordered_map<std::string, std::string> ops = {
        {"add","+"},{"sub","-"},{"mul","*"},{"div","/"},{"rem","%"},
        {"shl","<<"},{"shr",">>"},{"and","&"},{"or","|"},{"xor","^"},
        {"min","min"},{"max","max"},
    };
    auto it = ops.find(i.mnemonic);
    if (it == ops.end()) return ind + "/* " + i.mnemonic + " */\n";

    std::string op = it->second;
    if (op == "min" || op == "max")
        return ind + dst + " = " + op + "(" + a + ", " + b + ");\n";

    // mad: dst = a * b + c
    if (i.mnemonic == "mad" || i.mnemonic == "fma") {
        std::string c = i.operands.size() >= 4 ?
                        operandStr(i.operands[3], i.type) : "0";
        return ind + dst + " = " + a + " * " + b + " + " + c + ";\n";
    }

    // neg / abs: unary
    if (i.mnemonic == "neg") return ind + dst + " = -" + a + ";\n";
    if (i.mnemonic == "abs") return ind + dst + " = abs(" + a + ");\n";

    return ind + dst + " = " + a + " " + op + " " + b + ";\n";
}

std::string InstrLifter::liftSetp(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 3) return ind + "/* setp */\n";
    std::string pred = operandStr(i.operands[0], i.type);
    std::string a    = operandStr(i.operands[1], i.type);
    std::string b    = operandStr(i.operands[2], i.type);
    std::string cmp  = sepCmp(i.modifier);
    return ind + pred + " = (" + a + " " + cmp + " " + b + ");\n";
}

std::string InstrLifter::liftSelp(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 4) return ind + "/* selp */\n";
    std::string dst  = operandStr(i.operands[0], i.type);
    std::string a    = operandStr(i.operands[1], i.type);
    std::string b    = operandStr(i.operands[2], i.type);
    std::string pred = operandStr(i.operands[3], i.type);
    return ind + dst + " = " + pred + " ? " + a + " : " + b + ";\n";
}

std::string InstrLifter::liftCvt(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 2) return ind + "/* cvt */\n";
    std::string dst  = operandStr(i.operands[0], i.type);
    std::string src  = operandStr(i.operands[1], i.type);
    std::string cast = cTypeStr(i.type);
    return ind + dst + " = (" + cast + ")" + src + ";\n";
}

std::string InstrLifter::liftBar(const PtxInstr& i, const std::string& ind) const {
    (void)i;
    return ind + "__syncthreads();\n";
}

std::string InstrLifter::liftMembar(const PtxInstr& i, const std::string& ind) const {
    if (i.modifier.find(".gl") != std::string::npos)
        return ind + "__threadfence();\n";
    if (i.modifier.find(".cta") != std::string::npos)
        return ind + "__threadfence_block();\n";
    return ind + "__threadfence_system();\n";
}

std::string InstrLifter::liftAtom(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 3) return ind + "/* atom */\n";
    std::string dst = operandStr(i.operands[0], i.type);
    std::string ptr = operandStr(i.operands[1], i.type);
    std::string val = operandStr(i.operands[2], i.type);

    std::string fn;
    if (i.modifier.find(".add") != std::string::npos) fn = "atomicAdd";
    else if (i.modifier.find(".exch") != std::string::npos) fn = "atomicExch";
    else if (i.modifier.find(".min")  != std::string::npos) fn = "atomicMin";
    else if (i.modifier.find(".max")  != std::string::npos) fn = "atomicMax";
    else if (i.modifier.find(".and")  != std::string::npos) fn = "atomicAnd";
    else if (i.modifier.find(".or")   != std::string::npos) fn = "atomicOr";
    else if (i.modifier.find(".xor")  != std::string::npos) fn = "atomicXor";
    else if (i.modifier.find(".cas")  != std::string::npos) {
        std::string cmp = i.operands.size() >= 4 ?
                          operandStr(i.operands[3], i.type) : "0";
        return ind + dst + " = atomicCAS(&" + ptr + ", " + cmp + ", " + val + ");\n";
    }
    else fn = "atomicAdd";

    return ind + dst + " = " + fn + "(&" + ptr + ", " + val + ");\n";
}

std::string InstrLifter::liftVote(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.empty()) return ind + "/* vote */\n";
    std::string dst  = operandStr(i.operands[0], i.type);
    std::string pred = i.operands.size() >= 2 ? operandStr(i.operands[1], i.type) : "0";
    if (i.modifier.find(".all") != std::string::npos)
        return ind + dst + " = __all_sync(0xffffffff, " + pred + ");\n";
    if (i.modifier.find(".any") != std::string::npos)
        return ind + dst + " = __any_sync(0xffffffff, " + pred + ");\n";
    if (i.modifier.find(".ballot") != std::string::npos)
        return ind + dst + " = __ballot_sync(0xffffffff, " + pred + ");\n";
    return ind + dst + " = __all_sync(0xffffffff, " + pred + ");\n";
}

std::string InstrLifter::liftShfl(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.size() < 3) return ind + "/* shfl */\n";
    std::string dst  = operandStr(i.operands[0], i.type);
    std::string src  = operandStr(i.operands[1], i.type);
    std::string lane = operandStr(i.operands[2], i.type);
    if (i.modifier.find(".up")   != std::string::npos)
        return ind + dst + " = __shfl_up_sync(0xffffffff, " + src + ", " + lane + ");\n";
    if (i.modifier.find(".down") != std::string::npos)
        return ind + dst + " = __shfl_down_sync(0xffffffff, " + src + ", " + lane + ");\n";
    if (i.modifier.find(".xor")  != std::string::npos)
        return ind + dst + " = __shfl_xor_sync(0xffffffff, " + src + ", " + lane + ");\n";
    return ind + dst + " = __shfl_sync(0xffffffff, " + src + ", " + lane + ");\n";
}

std::string InstrLifter::liftMath(const PtxInstr& i, const std::string& ind) const {
    if (i.operands.empty()) return ind + "/* math */\n";
    std::string dst = operandStr(i.operands[0], i.type);
    std::string src = i.operands.size() >= 2 ? operandStr(i.operands[1], i.type) : "0";
    bool isF32 = (i.type == PtxType::F32);

    if (i.mnemonic == "sqrt")  return ind + dst + " = " + (isF32 ? "__fsqrt_rn(" : "sqrt(") + src + ");\n";
    if (i.mnemonic == "rsqrt") return ind + dst + " = " + (isF32 ? "__frsqrt_rn(" : "1.0/sqrt(") + src + ");\n";
    if (i.mnemonic == "sin")   return ind + dst + " = " + (isF32 ? "__sinf(" : "sin(") + src + ");\n";
    if (i.mnemonic == "cos")   return ind + dst + " = " + (isF32 ? "__cosf(" : "cos(") + src + ");\n";
    if (i.mnemonic == "lg2")   return ind + dst + " = " + (isF32 ? "__log2f(" : "log2(") + src + ");\n";
    if (i.mnemonic == "ex2")   return ind + dst + " = " + (isF32 ? "__exp2f(" : "exp2(") + src + ");\n";
    if (i.mnemonic == "rcp")   return ind + dst + " = " + (isF32 ? "__frcp_rn(" : "1.0/(") + src + (isF32 ? ")" : ")") + ";\n";
    return ind + "/* " + i.mnemonic + " */\n";
}

std::string InstrLifter::liftBranch(const PtxInstr& i, const std::string& ind) const {
    std::string target = !i.operands.empty() ? i.operands[0].name : "?";
    if (!i.predReg.empty()) {
        std::string cond = i.predReg.substr(1); // strip %
        if (i.negPred)
            return ind + "if (!" + cond + ") goto " + target + ";\n";
        return ind + "if (" + cond + ") goto " + target + ";\n";
    }
    return ind + "goto " + target + ";\n";
}

std::string InstrLifter::liftCall(const PtxInstr& i, const std::string& ind) const {
    std::string callee = !i.operands.empty() ? i.operands[0].name : "?";
    std::ostringstream os;
    os << ind << callee << "(";
    for (size_t k = 1; k < i.operands.size(); ++k) {
        if (k > 1) os << ", ";
        os << operandStr(i.operands[k], i.type);
    }
    os << ");\n";
    return os.str();
}

std::string InstrLifter::liftRet(const PtxInstr&, const std::string& ind) const {
    return ind + "return;\n";
}

// ─── InstrLifter::lift dispatcher ─────────────────────────────────────────────

std::string InstrLifter::lift(const PtxInstr& i, const std::string& ind) const {
    if (i.mnemonic == "__label__") {
        return i.label + ":\n";
    }

    // Wrap in predicate guard if present
    auto emit = [&](std::string body) -> std::string {
        if (i.predReg.empty()) return body;
        std::string cond = i.predReg.substr(1);
        std::string prefix = ind + "if (" + (i.negPred ? "!" : "") + cond + ") {\n";
        return prefix + "  " + body + ind + "}\n";
    };

    const std::string& mn = i.mnemonic;

    if (mn == "mov")  return emit(liftMov(i, ind));
    if (mn == "ld")   return emit(liftLoad(i, ind));
    if (mn == "st")   return emit(liftStore(i, ind));
    if (mn == "add" || mn == "sub" || mn == "mul" || mn == "div" || mn == "rem" ||
        mn == "shl" || mn == "shr" || mn == "and" || mn == "or"  || mn == "xor"  ||
        mn == "neg" || mn == "abs" || mn == "min" || mn == "max" ||
        mn == "mad" || mn == "fma") return emit(liftArith(i, ind));
    if (mn == "setp") return emit(liftSetp(i, ind));
    if (mn == "selp") return emit(liftSelp(i, ind));
    if (mn == "cvt")  return emit(liftCvt(i, ind));
    if (mn == "bar")  return emit(liftBar(i, ind));
    if (mn == "membar") return emit(liftMembar(i, ind));
    if (mn == "atom" || mn == "red") return emit(liftAtom(i, ind));
    if (mn == "vote") return emit(liftVote(i, ind));
    if (mn == "shfl") return emit(liftShfl(i, ind));
    if (mn == "sqrt" || mn == "rsqrt" || mn == "sin" || mn == "cos" ||
        mn == "lg2"  || mn == "ex2"   || mn == "rcp") return emit(liftMath(i, ind));
    if (mn == "bra")  return liftBranch(i, ind);  // handles its own predicate
    if (mn == "call") return liftCall(i, ind);
    if (mn == "ret")  return emit(liftRet(i, ind));
    if (mn == "not")  {
        if (i.operands.size() >= 2) {
            return emit(ind + operandStr(i.operands[0], i.type) +
                        " = ~" + operandStr(i.operands[1], i.type) + ";\n");
        }
    }
    if (mn == "exit") return emit(ind + "return;\n");

    // Unknown: emit as comment
    std::ostringstream os;
    os << ind << "/* " << mn << i.modifier;
    for (const auto& op : i.operands) os << " " << operandStr(op, i.type);
    os << " */\n";
    return os.str();
}

// ─── PtxLifter ────────────────────────────────────────────────────────────────

std::string PtxLifter::paramTypeStr(const PtxParam& p) {
    std::string base = ptxCType(p.type);
    if (p.isPointer) return base + "*";
    return base;
}

std::string PtxLifter::emitSignature(const PtxKernel& k) const {
    std::ostringstream os;
    os << (k.kind == PtxKernelKind::Entry ? "__global__ void " : "__device__ void ");
    os << k.name << "(";
    for (size_t i = 0; i < k.params.size(); ++i) {
        if (i > 0) os << ", ";
        os << paramTypeStr(k.params[i]) << " " << k.params[i].name;
    }
    os << ")";
    return os.str();
}

std::string PtxLifter::emitDecls(const PtxKernel& k) const {
    std::ostringstream os;
    for (const auto& decl : k.decls) {
        if (decl.space == PtxSpace::Shared) {
            os << "    " << SharedMemDeclaration::emit(decl) << "\n";
            continue;
        }
        if (decl.space == PtxSpace::Reg) {
            std::string ctype = ptxCType(decl.type);
            std::string basename = decl.name.substr(1);
            if (decl.count > 1) {
                os << "    " << ctype << " ";
                for (int j = 0; j < decl.count; ++j) {
                    if (j > 0) os << ", ";
                    os << basename << j;
                }
                os << ";\n";
            } else {
                os << "    " << ctype << " " << basename << ";\n";
            }
        }
    }
    return os.str();
}

std::string PtxLifter::emitBody(const PtxKernel& k) const {
    InstrLifter lifter;
    std::ostringstream os;
    for (const auto& instr : k.instrs)
        os << lifter.lift(instr, "    ");
    return os.str();
}

std::string PtxLifter::liftKernel(const PtxKernel& k) const {
    std::ostringstream os;
    os << emitSignature(k) << " {\n";
    os << emitDecls(k);
    if (!k.decls.empty()) os << "\n";
    os << emitBody(k);
    os << "}\n";
    return os.str();
}

std::string PtxLifter::liftModule(const PtxModule& mod) const {
    std::ostringstream os;
    os << "// Lifted from PTX (sm_" << mod.smVersion << ")\n";
    os << "// Generated by RetDec PTX lifter\n\n";
    os << "#include <cuda_runtime.h>\n\n";

    bool hasShared = false;
    for (const auto& k : mod.kernels)
        for (const auto& d : k.decls)
            if (d.space == PtxSpace::Shared) { hasShared = true; break; }
    if (hasShared) os << "// Note: __shared__ variables declared inside kernels\n\n";

    for (const auto& k : mod.kernels) {
        os << liftKernel(k);
        os << "\n";
    }
    return os.str();
}

} // namespace retdec::ptx_decompile
