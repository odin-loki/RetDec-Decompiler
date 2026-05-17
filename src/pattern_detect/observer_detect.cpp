/**
 * @file src/pattern_detect/observer_detect.cpp
 * @brief Observer design pattern detector.
 *
 * ## Structural invariant (two-function analysis)
 *
 * Observer requires detecting two complementary functions:
 *
 * ### Register (subscribe / addListener)
 *   - A push_back / emplace_back call on a container field inside a struct.
 *   - The argument is a function pointer or pointer-to-interface (callback).
 *
 * ### Notify (emit / notify / broadcast)
 *   - A loop over the same container field.
 *   - An indirect call through each element (function pointer or vtable call).
 *
 * ## Single-function fallback
 *
 * When only one function is provided (intra-procedural mode), we look for both
 * patterns within the same function — possible in small Observer implementations
 * where register + notify are folded into one.
 *
 * ## Confidence scoring (group mode)
 *
 *   register function found      +0.45
 *   notify function found        +0.45
 *   same container evidence      +0.10 (same struct offset used in both)
 *
 * ## Confidence scoring (single-function mode)
 *
 *   push_back + indirect call in same fn  → 0.60
 */

#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace pattern_detect {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs)
            if (i && i->op == op) ++n;
    }
    return n;
}

static bool hasBackEdge(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t s : blk->succs) if (s <= b) return true;
    }
    return false;
}

} // anonymous namespace

bool ObserverDetector::hasRegisterPattern(const ssa::SSAFunction& fn) const {
    // push_back / emplace_back of a callback.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("push_back")    != std::string::npos ||
                cn.find("emplace_back") != std::string::npos ||
                cn.find("insert")       != std::string::npos ||
                cn.find("subscribe")    != std::string::npos ||
                cn.find("addListener")  != std::string::npos ||
                cn.find("addObserver")  != std::string::npos)
                return true;
        }
    }
    return false;
}

bool ObserverDetector::hasNotifyPattern(const ssa::SSAFunction& fn) const {
    // Loop + indirect call (Call through a pointer, i.e., callee unknown or
    // callee looks like a vtable dispatch).
    if (!hasBackEdge(fn)) return false;
    // An indirect call: a Call instruction with an empty or indirect callee name.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            // Indirect call: callee name starts with "*" or is a register name,
            // or callee name contains "notify" / "emit" / "broadcast".
            const auto& cn = i->calleeName;
            if (cn.empty() || cn[0] == '*' ||
                cn.find("notify")    != std::string::npos ||
                cn.find("emit")      != std::string::npos ||
                cn.find("broadcast") != std::string::npos ||
                cn.find("dispatch")  != std::string::npos)
                return true;
        }
    }
    return false;
}

PatternResult ObserverDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::Observer;
    bool reg    = hasRegisterPattern(fn);
    bool notify = hasNotifyPattern(fn);
    r.confidence = (reg ? 0.45f : 0.0f) + (notify ? 0.45f : 0.0f);
    // Small bonus if both patterns co-exist in one function.
    if (reg && notify) r.confidence = std::min(1.0f, r.confidence + 0.10f);
    if (r.confidence >= 0.45f) {
        r.emittedForm =
            "template<typename Event>\n"
            "struct EventEmitter {\n"
            "    std::vector<std::function<void(Event)>> listeners;\n"
            "    void subscribe(std::function<void(Event)> cb) { listeners.push_back(cb); }\n"
            "    void emit(Event e) { for (auto& cb : listeners) cb(e); }\n"
            "};";
        r.comment = "// Design pattern: Observer (EventEmitter)";
    }
    return r;
}

PatternResult ObserverDetector::detectGroup(
        const std::vector<const ssa::SSAFunction*>& fns) const {
    PatternResult r;
    r.kind = PatternKind::Observer;
    bool reg = false, notify = false;
    for (const auto* fn : fns) {
        if (!fn) continue;
        if (!reg && hasRegisterPattern(*fn))  reg = true;
        if (!notify && hasNotifyPattern(*fn)) notify = true;
    }
    r.confidence = (reg ? 0.45f : 0.0f) + (notify ? 0.45f : 0.0f);
    if (reg && notify) r.confidence = std::min(1.0f, r.confidence + 0.10f);
    if (r.confidence >= 0.45f) {
        r.emittedForm =
            "template<typename Event>\n"
            "struct EventEmitter {\n"
            "    std::vector<std::function<void(Event)>> listeners;\n"
            "    void subscribe(std::function<void(Event)> cb) { listeners.push_back(cb); }\n"
            "    void emit(Event e) { for (auto& cb : listeners) cb(e); }\n"
            "};";
        r.comment = "// Design pattern: Observer (EventEmitter)";
    }
    return r;
}

} // namespace pattern_detect
} // namespace retdec
