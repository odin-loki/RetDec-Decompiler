/**
 * @file src/idiom_reconstruct/idiom_reconstruct.cpp
 * @brief IdiomEngine core and IdiomInstr helpers.
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"

#include <sstream>

namespace retdec {
namespace idiom_reconstruct {

// ─── IdiomInstr helpers ───────────────────────────────────────────────────────

bool IdiomInstr::sameReg(int srcIdx, const IdiomInstr& other, int otherSrcIdx) const {
    const IdiomOperand& a = (srcIdx==0) ? src0 : (srcIdx==1) ? src1 : src2;
    const IdiomOperand& b = (otherSrcIdx==0) ? other.src0 : (otherSrcIdx==1) ? other.src1 : other.src2;
    return a.kind==OperandKind::Reg && b.kind==OperandKind::Reg && a.reg==b.reg;
}

bool IdiomInstr::dstEqSrc(int srcIdx) const {
    const IdiomOperand& s = (srcIdx==0) ? src0 : (srcIdx==1) ? src1 : src2;
    return dst.kind==OperandKind::Reg && s.kind==OperandKind::Reg && dst.reg==s.reg;
}

// ─── ReplacementNode::debugStr ────────────────────────────────────────────────

std::string ReplacementNode::debugStr() const {
    std::ostringstream os;
    switch (kind) {
    case ReplacementKind::DivSigned:
        os << "r" << outputReg << " = (int" << operandWidth << ")r" << inputReg << " / " << divisor; break;
    case ReplacementKind::DivUnsigned:
        os << "r" << outputReg << " = (uint" << operandWidth << ")r" << inputReg << " / " << (uint64_t)divisor; break;
    case ReplacementKind::ModSigned:
        os << "r" << outputReg << " = (int" << operandWidth << ")r" << inputReg << " % " << divisor; break;
    case ReplacementKind::ModUnsigned:
        os << "r" << outputReg << " = (uint" << operandWidth << ")r" << inputReg << " % " << (uint64_t)divisor; break;
    case ReplacementKind::AbsValue:
        os << "r" << outputReg << " = abs(r" << inputReg << ")"; break;
    case ReplacementKind::LowestSetBit:
        os << "r" << outputReg << " = r" << inputReg << " & -r" << inputReg; break;
    case ReplacementKind::ClearLowestSetBit:
        os << "r" << outputReg << " = r" << inputReg << " & (r" << inputReg << "-1)"; break;
    case ReplacementKind::ToBool:
        os << "r" << outputReg << " = (bool)r" << inputReg; break;
    case ReplacementKind::IsZero:
        os << "r" << outputReg << " = (r" << inputReg << " == 0)"; break;
    case ReplacementKind::Popcount:
        os << "r" << outputReg << " = popcount(r" << inputReg << ")"; break;
    case ReplacementKind::ByteSwap:
        os << "r" << outputReg << " = bswap" << operandWidth << "(r" << inputReg << ")"; break;
    case ReplacementKind::Memset:
        os << "memset(r" << dstReg << ", " << fillValue;
        if (countImm >= 0) os << ", " << countImm;
        else               os << ", r" << countReg;
        os << ")"; break;
    case ReplacementKind::Memcpy:
        os << "memcpy(r" << dstReg << ", r" << srcReg;
        if (countImm >= 0) os << ", " << countImm;
        else               os << ", r" << countReg;
        os << ")"; break;
    case ReplacementKind::Memmove:
        os << "memmove(r" << dstReg << ", r" << srcReg;
        if (countImm >= 0) os << ", " << countImm;
        else               os << ", r" << countReg;
        os << ")"; break;
    }
    os << "  [vma " << std::hex << firstVma << ".." << lastVma
       << ", " << std::dec << instrCount << " instrs]";
    return os.str();
}

// ─── IdiomEngine ──────────────────────────────────────────────────────────────

IdiomEngine::IdiomEngine()  = default;
IdiomEngine::~IdiomEngine() = default;

void IdiomEngine::registerMatcher(std::unique_ptr<IIdiomMatcher> m) {
    matchers_.push_back(std::move(m));
}

std::vector<ReplacementNode> IdiomEngine::process(const InstrWindow& window) const {
    std::vector<ReplacementNode> results;
    const std::size_t n = window.size();
    std::size_t i = 0;

    while (i < n) {
        bool matched = false;
        for (auto& m : matchers_) {
            if (n - i < m->minWindowSize()) continue;
            auto opt = m->match(window, i, profile_);
            if (opt) {
                results.push_back(std::move(*opt));
                i += results.back().instrCount;
                matched = true;
                break;  // greedy: first match wins
            }
        }
        if (!matched) ++i;
    }

    return results;
}

// ─── makeDefaultEngine ────────────────────────────────────────────────────────

IdiomEngine makeDefaultEngine(CompilerProfile profile) {
    IdiomEngine e;
    e.setProfile(profile);
    // Order: division first (so modulo matcher sees the raw MUL-back suffix
    // at the right position), then everything else.
    e.registerMatcher(makeDivisionMatcher());
    e.registerMatcher(makeModuloMatcher());
    e.registerMatcher(makeAbsMatcher());
    e.registerMatcher(makeBitIdiomMatcher());
    e.registerMatcher(makeSimdMemMatcher());
    return e;
}

} // namespace idiom_reconstruct
} // namespace retdec
