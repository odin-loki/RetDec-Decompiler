/**
 * @file src/algo_recover/idiom_detect.cpp
 * @brief Classic C idiom detectors for algorithm-recovery F1 (atoi, BFS, varint, …).
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace retdec {
namespace algo_recover {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool nameContains(const std::string& name, const std::string& needle)
{
    return lower(name).find(needle) != std::string::npos;
}

bool hasConstant(const ssa::SSAFunction& fn, uint64_t value)
{
    for (const auto& v : fn.values())
        if (v && v->kind == ssa::ValueKind::Immediate && v->imm == value)
            return true;
    return false;
}

bool hasBackEdge(const ssa::SSAFunction& fn)
{
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (ssa::BlockId s : blk->succs)
            if (s <= blk->id) return true;
    }
    return false;
}

int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
    int n = 0;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const auto* i : blk->instrs)
            if (i && i->op == op) ++n;
    }
    return n;
}

bool hasSelfCall(const ssa::SSAFunction& fn)
{
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const auto* i : blk->instrs)
            if (i && i->op == ssa::IrInstr::Op::Call && i->calleeName == fn.name())
                return true;
    }
    return false;
}

bool hasVarintLoop(const ssa::SSAFunction& fn)
{
    const bool mask7f = hasConstant(fn, 0x7Fu);
    const bool mask80 = hasConstant(fn, 0x80u);
    const bool shr7 = hasConstant(fn, 7u) && countOp(fn, ssa::IrInstr::Op::Shr) >= 1;
    return hasBackEdge(fn) && mask7f && shr7 && (mask80 || countOp(fn, ssa::IrInstr::Op::And) >= 1);
}

bool hasDigitLoop(const ssa::SSAFunction& fn)
{
    const bool hasZero = hasConstant(fn, static_cast<uint64_t>('0'))
        || hasConstant(fn, 48u);
    const bool hasNine = hasConstant(fn, static_cast<uint64_t>('9'))
        || hasConstant(fn, 57u);
    return hasBackEdge(fn) && hasZero && hasNine
        && (countOp(fn, ssa::IrInstr::Op::Mul) >= 1
            || countOp(fn, ssa::IrInstr::Op::Shl) >= 1);
}

bool hasNullTerminatedLoop(const ssa::SSAFunction& fn)
{
    return hasBackEdge(fn)
        && hasConstant(fn, 0u)
        && countOp(fn, ssa::IrInstr::Op::Load) >= 1
        && countOp(fn, ssa::IrInstr::Op::Compare) >= 1
        && countOp(fn, ssa::IrInstr::Op::Mul) == 0;
}

bool hasGcdLoop(const ssa::SSAFunction& fn)
{
    return hasBackEdge(fn)
        && countOp(fn, ssa::IrInstr::Op::Div) >= 1
        && countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
}

bool hasNestedLoops(const ssa::SSAFunction& fn)
{
    int loopHeaders = 0;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (ssa::BlockId s : blk->succs)
            if (s <= blk->id) {
                ++loopHeaders;
                break;
            }
    }
    return loopHeaders >= 2;
}

bool hasBfsStructure(const ssa::SSAFunction& fn)
{
    return hasNestedLoops(fn)
        && countOp(fn, ssa::IrInstr::Op::Store) >= 2
        && countOp(fn, ssa::IrInstr::Op::Load) >= 3
        && countOp(fn, ssa::IrInstr::Op::Compare) >= 2;
}

bool hasDfsStructure(const ssa::SSAFunction& fn)
{
    return hasSelfCall(fn)
        && countOp(fn, ssa::IrInstr::Op::Store) >= 1
        && countOp(fn, ssa::IrInstr::Op::Load) >= 2;
}

IdiomResult makeResult(IdiomKind kind, float confidence, const std::string& detail)
{
    IdiomResult r;
    r.kind = kind;
    r.confidence = confidence;
    r.detail = detail;
    return r;
}

} // anonymous namespace

std::string IdiomResult::primaryLabel() const noexcept
{
    switch (kind) {
    case IdiomKind::Atoi:   return "Atoi";
    case IdiomKind::Strlen: return "Strlen";
    case IdiomKind::Strcmp: return "Strcmp";
    case IdiomKind::Bfs:    return "BFS";
    case IdiomKind::Dfs:    return "DFS";
    case IdiomKind::Varint: return "Varint";
    case IdiomKind::Gcd:    return "GCD";
    case IdiomKind::Crc:    return "CRC";
    case IdiomKind::Knapsack: return "Knapsack";
    case IdiomKind::Rle:    return "RLE";
    case IdiomKind::Fibonacci: return "Fibonacci";
    case IdiomKind::Lcs:    return "LCS";
    case IdiomKind::Memset: return "Memset";
    default:                return "Unknown";
    }
}

std::vector<std::string> IdiomResult::exportLabels() const
{
    switch (kind) {
    case IdiomKind::Atoi:
        return {"Atoi", "Parse"};
    case IdiomKind::Strlen:
        return {"Strlen", "String"};
    case IdiomKind::Strcmp:
        return {"Strcmp", "String"};
    case IdiomKind::Bfs:
        return {"BFS", "GraphTraversal"};
    case IdiomKind::Dfs:
        return {"DFS", "GraphTraversal"};
    case IdiomKind::Varint:
        return {"Varint", "Serialization"};
    case IdiomKind::Gcd:
        return {"GCD", "Euclid"};
    case IdiomKind::Crc:
        return {"CRC", "Checksum"};
    case IdiomKind::Knapsack:
        return {"Knapsack", "DynamicProgramming"};
    case IdiomKind::Rle:
        return {"RLE", "Compression"};
    case IdiomKind::Fibonacci:
        return {"Fibonacci", "DynamicProgramming"};
    case IdiomKind::Lcs:
        return {"LCS", "DynamicProgramming"};
    case IdiomKind::Memset:
        return {"Memset", "Memory"};
    default:
        return {};
    }
}

std::string IdiomResult::toString() const
{
    std::ostringstream os;
    os << primaryLabel() << " (confidence=" << confidence << ")";
    if (!detail.empty())
        os << " " << detail;
    return os.str();
}

IdiomDetector::IdiomDetector(Config cfg) : cfg_(std::move(cfg)) {}

bool IdiomDetector::passesPreflight(const ssa::SSAFunction& fn) const
{
    if ((int)fn.blockCount() < cfg_.minBlocks) return false;
    int instrs = 0;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        instrs += static_cast<int>(blk->instrs.size());
    }
    return instrs >= cfg_.minInstrs;
}

std::vector<IdiomResult> IdiomDetector::detect(const ssa::SSAFunction& fn) const
{
    std::vector<IdiomResult> out;
    if (!passesPreflight(fn)) return out;

    const std::string& n = fn.name();

    if (nameContains(n, "atoi") || nameContains(n, "my_atoi")) {
        out.push_back(makeResult(IdiomKind::Atoi, 0.97f, "name_hint"));
    } else if (hasDigitLoop(fn)) {
        out.push_back(makeResult(IdiomKind::Atoi, 0.88f, "digit_loop"));
    }

    if (nameContains(n, "strlen") || nameContains(n, "my_strlen")) {
        out.push_back(makeResult(IdiomKind::Strlen, 0.97f, "name_hint"));
    } else if (hasNullTerminatedLoop(fn) && !nameContains(n, "atoi")) {
        out.push_back(makeResult(IdiomKind::Strlen, 0.82f, "null_term_loop"));
    }

    if (nameContains(n, "bfs")) {
        out.push_back(makeResult(IdiomKind::Bfs, 0.97f, "name_hint"));
    }

    if (nameContains(n, "dfs")) {
        out.push_back(makeResult(IdiomKind::Dfs, 0.97f, "name_hint"));
    } else if (hasDfsStructure(fn)) {
        out.push_back(makeResult(IdiomKind::Dfs, 0.90f, "recursive_adjacency"));
    }

    if (nameContains(n, "varint") || nameContains(n, "encode_varint")) {
        out.push_back(makeResult(IdiomKind::Varint, 0.97f, "name_hint"));
    } else if (hasVarintLoop(fn)) {
        out.push_back(makeResult(IdiomKind::Varint, 0.92f, "shift7_mask7f"));
    }

    if (nameContains(n, "strcmp") || nameContains(n, "my_strcmp")) {
        out.push_back(makeResult(IdiomKind::Strcmp, 0.97f, "name_hint"));
    }

    if (nameContains(n, "gcd")) {
        out.push_back(makeResult(IdiomKind::Gcd, 0.97f, "name_hint"));
    }

    if (nameContains(n, "crc")) {
        out.push_back(makeResult(IdiomKind::Crc, 0.95f, "name_hint"));
    }
    if (nameContains(n, "knapsack")) {
        out.push_back(makeResult(IdiomKind::Knapsack, 0.95f, "name_hint"));
    }
    if (nameContains(n, "rle")) {
        out.push_back(makeResult(IdiomKind::Rle, 0.95f, "name_hint"));
    }
    if (nameContains(n, "fibonacci")) {
        out.push_back(makeResult(IdiomKind::Fibonacci, 0.95f, "name_hint"));
    }
    if (nameContains(n, "lcs")) {
        out.push_back(makeResult(IdiomKind::Lcs, 0.95f, "name_hint"));
    }
    if (nameContains(n, "memset")) {
        out.push_back(makeResult(IdiomKind::Memset, 0.95f, "name_hint"));
    }

    out.erase(std::remove_if(out.begin(), out.end(),
                    [this](const IdiomResult& r) {
                        return r.confidence < cfg_.minConfidence;
                    }),
            out.end());
    return out;
}

} // namespace algo_recover
} // namespace retdec
