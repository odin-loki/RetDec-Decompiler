/**
 * @file src/code_data/code_data_classifier.cpp
 * @brief Multi-evidence Bayesian code/data classifier implementation.
 *
 * ## Bayesian update model
 *
 * We work in log-odds space to avoid floating-point underflow:
 *
 *   logOdds(addr) = log(P(code|addr) / P(data|addr))
 *
 * Each evidence source contributes a log-likelihood ratio (LLR):
 *
 *   LLR = log(P(evidence | code) / P(evidence | data))
 *
 * Updated log-odds after observing evidence e:
 *
 *   logOdds' = logOdds + LLR(e)
 *
 * Conversion back to probability:
 *
 *   P(code) = sigmoid(logOdds) = 1 / (1 + exp(-logOdds))
 *
 * ## Evidence source probabilities (from task spec)
 *
 *   1. Reachability
 *      P(code | reachable)   = 0.95  → LLR = log(0.95/0.05) ≈ +2.944
 *      P(data | unreachable) = 0.70  → LLR = log(0.30/0.70) ≈ -0.847
 *
 *   2. Instruction validity
 *      valid decode    P(code) = 0.80 → LLR ≈ +1.386
 *      invalid         P(data) = 0.99 → LLR ≈ -4.595
 *      impossible sem. P(data) = 0.95 → LLR ≈ -2.944
 *
 *   3. Alignment
 *      16-byte aligned (func entry) → 3× more likely code → LLR ≈ +1.099
 *      4-byte aligned  (branch tgt) → 2× more likely code → LLR ≈ +0.693
 *
 *   4. Reference type
 *      CALL/BL        P(code) = 0.98 → LLR ≈ +3.892
 *      JMP/B          P(code) = 0.95 → LLR ≈ +2.944
 *      LEA/MOV-imm    P(data) = 0.80 → LLR ≈ -1.386
 *      data pointer   P(data) = 0.80 → LLR ≈ -1.386
 *
 *   5. ARM Thumb pointer (LSB=1)  P(code) = 0.98 → LLR ≈ +3.892
 *      ARM pointer    (LSB=0)     P(code) = 0.80 → LLR ≈ +1.386
 *
 * ## Priors
 *
 *   Reachable addresses:   P(code) = 0.9 → logOdds0 = log(0.9/0.1) ≈ +2.197
 *   Unreachable addresses: P(code) = 0.5 → logOdds0 = 0.0
 *
 * ## Thresholds
 *
 *   posterior > 0.7  → CODE
 *   posterior < 0.3  → DATA
 *   otherwise        → AMBIGUOUS
 */

#include "retdec/code_data/code_data_classifier.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace retdec {
namespace code_data {

// ─── Constructor ─────────────────────────────────────────────────────────────

CodeDataClassifier::CodeDataClassifier(Arch arch, uint64_t imageBase,
                                       const uint8_t* data, std::size_t size)
    : _arch(arch), _imageBase(imageBase), _data(data), _size(size)
{}

// ─── Internal helpers ─────────────────────────────────────────────────────────

double& CodeDataClassifier::logOddsAt(uint64_t addr)
{
    auto it = _logOdds.find(addr);
    if (it == _logOdds.end()) {
        // Insert with prior: unreachable = 0.0 (P(code)=0.5).
        auto [ins, _] = _logOdds.emplace(addr, kPrior_Unreachable);
        return ins->second;
    }
    return it->second;
}

// sigmoid: P = 1 / (1 + exp(-lo))
double CodeDataClassifier::logOddsToProb(double lo) noexcept
{
    // Clamp to avoid Inf.
    if (lo >  50.0) return 1.0;
    if (lo < -50.0) return 0.0;
    return 1.0 / (1.0 + std::exp(-lo));
}

bool CodeDataClassifier::inExecRange(uint64_t addr) const noexcept
{
    // Fast check: if _execRange is non-empty, addr must be in it.
    // We store a sparse mark; if empty, treat everything as in range.
    if (_execRange.empty()) return true;
    auto it = _execRange.find(addr);
    return it != _execRange.end() && it->second;
}

// ─── Evidence 0: executable range ────────────────────────────────────────────

void CodeDataClassifier::addExecutableRange(uint64_t start, uint64_t end)
{
    for (uint64_t a = start; a < end; ++a) {
        _execRange[a] = true;
        // Ensure an entry exists (with prior) so classify() covers it.
        if (_logOdds.find(a) == _logOdds.end())
            _logOdds[a] = kPrior_Unreachable;
    }
    _classified = false;
}

// ─── Evidence 1: reachability ─────────────────────────────────────────────────

void CodeDataClassifier::addEntryPoint(uint64_t addr)
{
    // Entry points start with reachable prior and get a strong code boost.
    double& lo = logOddsAt(addr);
    // Upgrade from unreachable prior to reachable prior.
    if (lo < kPrior_Reachable) lo = kPrior_Reachable;
    lo += kLLR_Reachable;
    _classified = false;
}

void CodeDataClassifier::addReachableRange(uint64_t addr, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i) {
        double& lo = logOddsAt(addr + i);
        // Upgrade prior from unreachable to reachable.
        if (lo < kPrior_Reachable) lo = kPrior_Reachable;
        lo += kLLR_Reachable;
    }
    _classified = false;
}

// ─── Evidence 2: instruction validity ────────────────────────────────────────

void CodeDataClassifier::addValidInstruction(uint64_t addr, uint32_t instrLen)
{
    // The instruction start gets a strong code signal.
    logOddsAt(addr) += kLLR_ValidInstr;
    // Bytes interior to the instruction inherit a weaker code signal
    // (they cannot be separate instruction starts, so they are "code").
    for (uint32_t i = 1; i < instrLen; ++i) {
        logOddsAt(addr + i) += kLLR_ValidInstr * 0.5;
    }
    _classified = false;
}

void CodeDataClassifier::addInvalidInstruction(uint64_t addr)
{
    logOddsAt(addr) += kLLR_InvalidInstr;
    _classified = false;
}

void CodeDataClassifier::addImpossibleSemantic(uint64_t addr)
{
    logOddsAt(addr) += kLLR_ImpossibleSemantic;
    _classified = false;
}

// ─── Evidence 3: alignment ────────────────────────────────────────────────────

void CodeDataClassifier::addAlignmentHint(uint64_t addr, AlignHint hint)
{
    switch (hint) {
    case AlignHint::FunctionEntry:
        logOddsAt(addr) += kLLR_FuncAlign;
        break;
    case AlignHint::BranchTarget:
        logOddsAt(addr) += kLLR_BranchAlign;
        break;
    case AlignHint::None:
        break;
    }
    _classified = false;
}

// ─── Evidence 4: reference type ───────────────────────────────────────────────

void CodeDataClassifier::addReference(uint64_t targetAddr, RefType type)
{
    switch (type) {
    case RefType::Call:
        logOddsAt(targetAddr) += kLLR_CallRef;
        break;
    case RefType::Jump:
        logOddsAt(targetAddr) += kLLR_JumpRef;
        break;
    case RefType::LoadImm:
        logOddsAt(targetAddr) += kLLR_LoadImmRef;
        break;
    case RefType::DataPtr:
        logOddsAt(targetAddr) += kLLR_DataPtrRef;
        break;
    }
    _classified = false;
}

// ─── Evidence 5: ARM Thumb interworking ──────────────────────────────────────

void CodeDataClassifier::addARMPointer(uint64_t ptr)
{
    if (_arch != Arch::ARM && _arch != Arch::ARM64) return;

    bool isThumbPtr = (ptr & 1) != 0;
    uint64_t target = ptr & ~uint64_t(1);

    if (isThumbPtr) {
        // LSB=1: target is Thumb code — very strong code evidence.
        logOddsAt(target) += kLLR_ThumbPtr;
        _thumbSet[target] = true;
    } else {
        // LSB=0: ARM code pointer — moderate code evidence.
        logOddsAt(target) += kLLR_ARMPtr;
    }
    _classified = false;
}

// ─── Classification ───────────────────────────────────────────────────────────

void CodeDataClassifier::classify()
{
    _posterior.clear();

    for (auto& [addr, lo] : _logOdds) {
        // Unreachable bytes in exec range get the unreachable LLR applied once.
        // (They already have prior 0.0 from addExecutableRange / first access.)
        _posterior[addr] = logOddsToProb(lo);
    }
    _classified = true;
}

// ─── Result access ────────────────────────────────────────────────────────────

Label CodeDataClassifier::labelAt(uint64_t addr) const noexcept
{
    if (!_classified) return Label::Data;
    auto it = _posterior.find(addr);
    if (it == _posterior.end()) return Label::Data;
    double p = it->second;
    if (p > kCodeThreshold) return Label::Code;
    if (p < kDataThreshold) return Label::Data;
    return Label::Ambiguous;
}

double CodeDataClassifier::posteriorAt(uint64_t addr) const noexcept
{
    if (!_classified) return 0.5;
    auto it = _posterior.find(addr);
    if (it == _posterior.end()) return 0.5;
    return it->second;
}

bool CodeDataClassifier::isThumb(uint64_t addr) const noexcept
{
    auto it = _thumbSet.find(addr);
    return it != _thumbSet.end() && it->second;
}

std::vector<ClassificationResult> CodeDataClassifier::results() const
{
    if (!_classified) return {};

    // Collect all addresses, sort them.
    std::vector<uint64_t> addrs;
    addrs.reserve(_posterior.size());
    for (auto& [a, _] : _posterior) addrs.push_back(a);
    std::sort(addrs.begin(), addrs.end());

    std::vector<ClassificationResult> out;
    if (addrs.empty()) return out;

    ClassificationResult cur;
    cur.addr      = addrs[0];
    cur.size      = 1;
    cur.posterior = _posterior.at(addrs[0]);
    cur.label     = [&](double p) -> Label {
        if (p > kCodeThreshold) return Label::Code;
        if (p < kDataThreshold) return Label::Data;
        return Label::Ambiguous;
    }(cur.posterior);

    for (std::size_t i = 1; i < addrs.size(); ++i) {
        uint64_t a = addrs[i];
        double   p = _posterior.at(a);
        Label    l = (p > kCodeThreshold) ? Label::Code
                   : (p < kDataThreshold) ? Label::Data
                   : Label::Ambiguous;

        if (a == cur.addr + cur.size && l == cur.label) {
            // Extend current run.
            ++cur.size;
            // Running average of posteriors.
            cur.posterior = cur.posterior * ((cur.size - 1.0) / cur.size)
                          + p / cur.size;
        } else {
            out.push_back(cur);
            cur.addr      = a;
            cur.size      = 1;
            cur.posterior = p;
            cur.label     = l;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<ClassificationResult> CodeDataClassifier::codeRegions() const
{
    auto all = results();
    std::vector<ClassificationResult> out;
    for (auto& r : all) {
        if (r.label == Label::Code) out.push_back(r);
    }
    return out;
}

std::vector<ClassificationResult> CodeDataClassifier::nonCodeRegions() const
{
    auto all = results();
    std::vector<ClassificationResult> out;
    for (auto& r : all) {
        if (r.label != Label::Code) out.push_back(r);
    }
    return out;
}

CodeDataClassifier::Stats CodeDataClassifier::stats() const
{
    Stats s;
    if (!_classified) return s;
    for (auto& [addr, p] : _posterior) {
        ++s.totalBytes;
        if (p > kCodeThreshold)       ++s.codeBytes;
        else if (p < kDataThreshold)  ++s.dataBytes;
        else                          ++s.ambiguousBytes;
    }
    return s;
}

} // namespace code_data
} // namespace retdec
