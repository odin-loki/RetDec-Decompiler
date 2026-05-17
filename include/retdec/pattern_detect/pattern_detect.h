/**
 * @file include/retdec/pattern_detect/pattern_detect.h
 * @brief Semantic Recovery — Design Pattern Detector (Stage 28).
 *
 * ## Overview
 *
 * This module detects seven classic GoF and idiom-level design patterns from
 * compiled C++ IR and emits annotated, idiomatic C++ source with pattern
 * commentary.
 *
 * Detection operates at the object level — above individual algorithm loops
 * (algo_recover) and below module clustering (task 40). Each detector analyses
 * one or more `SSAFunction` objects (the class's methods) in combination.
 *
 * ## Patterns
 *
 * ### Singleton
 *
 * ```cpp
 * static T* instance = nullptr;
 * T* getInstance() {
 *     if (!instance)            // null-check
 *         instance = new T();   // first-access allocation
 *     return instance;
 * }
 * ```
 *
 * Double-checked-lock variant (thread-safe):
 * ```cpp
 * if (!instance) {
 *     lock_acquire();
 *     if (!instance)            // second null-check inside lock
 *         instance = new T();
 *     lock_release();
 * }
 * ```
 *
 * IR signals:
 *   - Load of a static (global) pointer.
 *   - Compare against zero (null-check).
 *   - Conditional branch: taken path contains malloc/new + Store.
 *   - Return the loaded pointer.
 *   - Optional: two null-checks separated by lock_acquire call.
 *
 * ### Factory
 *
 * ```cpp
 * Base* createProduct(int type) {
 *     switch (type) {
 *     case 0: return new ConcreteA();
 *     case 1: return new ConcreteB();
 *     default: return nullptr;
 *     }
 * }
 * ```
 *
 * IR signals:
 *   - A switch (or if-else chain) over a single integer discriminant.
 *   - Each branch contains exactly one allocation (malloc/new).
 *   - All branches return through a common base pointer type.
 *   - Two or more distinct allocation call-sites (two or more products).
 *
 * ### Observer
 *
 * ```cpp
 * struct EventEmitter {
 *     std::vector<Callback> listeners;
 *     void subscribe(Callback cb) { listeners.push_back(cb); }
 *     void emit(Event e) {
 *         for (auto& cb : listeners) cb(e);
 *     }
 * };
 * ```
 *
 * IR signals (across two functions):
 *   - Register function: push_back/emplace_back of a callback/pointer into a
 *     container stored in a struct field.
 *   - Notify function: a loop over the same container field, calling each
 *     element (indirect call through function pointer or vtable).
 *
 * ### Command
 *
 * ```cpp
 * struct ICommand { virtual void execute() = 0; };
 * // A queue of ICommand* is maintained and sequentially executed.
 * ```
 *
 * IR signals:
 *   - A vtable with a dominant `execute` method (at offset 0 or 1).
 *   - A container (queue/stack) of base class pointers.
 *   - A loop over the container calling the virtual execute method.
 *   - Optional: a second vtable method `undo` for command history.
 *
 * ### Strategy
 *
 * ```cpp
 * struct Context {
 *     IStrategy* strategy_;
 *     void setStrategy(IStrategy* s) { strategy_ = s; }
 *     void execute() { strategy_->doAlgorithm(); }
 * };
 * ```
 *
 * IR signals:
 *   - A struct field holding a pointer-to-interface (vtable pointer-of-pointer).
 *   - A setter function writing to that field.
 *   - An executor function that reads the field and calls through it (indirect call).
 *
 * ### State Machine
 *
 * ```cpp
 * enum State { IDLE, RUNNING, STOPPED };
 * void transition(Event e) {
 *     switch (state_) {
 *     case IDLE:    state_ = RUNNING; break;
 *     case RUNNING: if (e == STOP) state_ = STOPPED; break;
 *     }
 * }
 * ```
 *
 * IR signals:
 *   - An integer state variable (load + store in multiple cases).
 *   - A switch/compare-chain that branches on the state variable's value.
 *   - State variable is modified inside the switch cases.
 *   - At least 2 distinct state values (at least 2 case constants).
 *
 * ### RAII
 *
 * ```cpp
 * struct FileHandle {
 *     FILE* fp_;
 *     FileHandle(const char* n) { fp_ = fopen(n, "r"); }
 *     ~FileHandle()             { fclose(fp_); }
 * };
 * ```
 *
 * IR signals (across constructor + destructor):
 *   - Constructor: Call to resource-acquire function (fopen, pthread_mutex_lock,
 *     malloc, CreateFile, etc.), result stored in struct field.
 *   - Destructor: matching Call to resource-release function (fclose,
 *     pthread_mutex_unlock, free, CloseHandle, etc.) on the same struct field.
 *   - No other significant methods beyond ctor/dtor.
 */

#ifndef RETDEC_PATTERN_DETECT_H
#define RETDEC_PATTERN_DETECT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace retdec {
namespace ssa { class SSAFunction; }
} // namespace retdec

namespace retdec {
namespace pattern_detect {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class PatternKind : uint8_t {
    Unknown,
    Singleton,
    Factory,
    Observer,
    Command,
    Strategy,
    StateMachine,
    RAII,
};

// ─── Pattern detection result ─────────────────────────────────────────────────

struct PatternResult {
    PatternKind  kind         = PatternKind::Unknown;
    float        confidence   = 0.0f;
    bool         hasVariant   = false;  ///< e.g. double-checked-lock, undo()
    std::string  variantName;           ///< e.g. "double-checked-lock"
    std::string  emittedForm;           ///< annotated C++ skeleton
    std::string  comment;               ///< pattern commentary for code annotation

    std::string kindName() const noexcept;
    std::string toString() const;
};

// ─── Evidence structs ─────────────────────────────────────────────────────────

struct SingletonEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasStaticPtrLoad = false;
    bool  hasNullCheck     = false;
    bool  hasFirstAlloc    = false;
    bool  hasReturn        = false;
    bool  hasDoubleLock    = false;  ///< double-checked locking variant
};

struct FactoryEvidence {
    bool  found             = false;
    float confidence        = 0.0f;
    bool  hasSwitchOrIfElse = false;
    bool  hasMultipleAllocs = false; ///< ≥2 distinct new/malloc sites in branches
    bool  hasBaseReturn     = false; ///< all paths return same ptr type
    int   branchCount       = 0;
};

struct ObserverEvidence {
    bool  found             = false;
    float confidence        = 0.0f;
    bool  hasRegisterFn     = false; ///< push_back of callback into container field
    bool  hasNotifyFn       = false; ///< loop + indirect call over same container
    bool  hasSameContainer  = false; ///< register + notify act on same field
};

struct CommandEvidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasVtableExecute   = false;
    bool  hasContainerOfPtrs = false;
    bool  hasLoopExecute     = false;
    bool  hasUndo            = false; ///< undo method present (history variant)
};

struct StrategyEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasInterfaceField = false; ///< stored interface pointer in struct
    bool  hasSetter        = false;  ///< setStrategy / setAlgorithm function
    bool  hasIndirectCall  = false;  ///< call through the stored pointer
};

struct StateMachineEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasStateVar      = false;  ///< load + store of same variable
    bool  hasSwitchOnState = false;  ///< switch/compare on that variable
    bool  hasStateModify   = false;  ///< state variable written in branches
    int   stateCount       = 0;      ///< number of distinct case constants
};

struct RAIIEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasAcquireInCtor = false;
    bool  hasReleaseInDtor = false;
    bool  hasMatchingPair  = false;  ///< acquire/release are paired functions
    std::string acquireName;
    std::string releaseName;
};

// ─── Detector interface ───────────────────────────────────────────────────────

/**
 * Base interface for all design pattern detectors.
 *
 * Some patterns require multiple functions (e.g. Observer's register + notify,
 * RAII's ctor + dtor), so detectors accept a list of related functions.
 */
class IPatternDetector {
public:
    virtual ~IPatternDetector() = default;

    /// Detect from a single function (intra-procedural patterns).
    virtual PatternResult detect(const ssa::SSAFunction& fn) const = 0;

    /// Detect from a group of related functions (same class's methods).
    virtual PatternResult detectGroup(
        const std::vector<const ssa::SSAFunction*>& fns) const {
        if (fns.empty()) return {};
        return detect(*fns.front());
    }

    virtual PatternKind kind() const noexcept = 0;
};

// ─── Per-pattern detectors ────────────────────────────────────────────────────

/** Singleton: static pointer null-check + first-access allocation. */
class SingletonDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternResult detectGroup(const std::vector<const ssa::SSAFunction*>& fns) const override;
    PatternKind   kind() const noexcept override { return PatternKind::Singleton; }
private:
    SingletonEvidence analyse(const ssa::SSAFunction& fn) const;
    float             score(const SingletonEvidence& ev) const;
    bool hasDoubleLock(const ssa::SSAFunction& fn) const;
};

/** Factory: switch/if-else over discriminant, each branch allocates derived type. */
class FactoryDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternKind   kind() const noexcept override { return PatternKind::Factory; }
private:
    FactoryEvidence analyse(const ssa::SSAFunction& fn) const;
    float           score(const FactoryEvidence& ev) const;
};

/** Observer: register() + notify() across two functions on same container field. */
class ObserverDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternResult detectGroup(const std::vector<const ssa::SSAFunction*>& fns) const override;
    PatternKind   kind() const noexcept override { return PatternKind::Observer; }
private:
    bool hasRegisterPattern(const ssa::SSAFunction& fn) const;
    bool hasNotifyPattern(const ssa::SSAFunction& fn) const;
};

/** Command: vtable execute(), container of base ptrs, loop invocation. */
class CommandDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternKind   kind() const noexcept override { return PatternKind::Command; }
private:
    CommandEvidence analyse(const ssa::SSAFunction& fn) const;
    float           score(const CommandEvidence& ev) const;
    bool hasUndoMethod(const ssa::SSAFunction& fn) const;
};

/** Strategy: stored interface pointer, setter, indirect call delegation. */
class StrategyDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternResult detectGroup(const std::vector<const ssa::SSAFunction*>& fns) const override;
    PatternKind   kind() const noexcept override { return PatternKind::Strategy; }
private:
    StrategyEvidence analyse(const ssa::SSAFunction& fn) const;
    float            score(const StrategyEvidence& ev) const;
};

/** State Machine: integer state var, switch-on-self, in-case mutations. */
class StateMachineDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternKind   kind() const noexcept override { return PatternKind::StateMachine; }
private:
    StateMachineEvidence analyse(const ssa::SSAFunction& fn) const;
    float                score(const StateMachineEvidence& ev) const;
    int                  countCaseConstants(const ssa::SSAFunction& fn) const;
};

/** RAII: ctor acquires resource, dtor releases matching resource. */
class RAIIDetector : public IPatternDetector {
public:
    PatternResult detect(const ssa::SSAFunction& fn) const override;
    PatternResult detectGroup(const std::vector<const ssa::SSAFunction*>& fns) const override;
    PatternKind   kind() const noexcept override { return PatternKind::RAII; }
private:
    RAIIEvidence analyse(const ssa::SSAFunction& fn) const;
    float        score(const RAIIEvidence& ev) const;
    bool isAcquireCall(const std::string& callee) const;
    bool isReleaseCall(const std::string& callee) const;
    std::string matchingRelease(const std::string& acquire) const;
};

// ─── Pattern detector orchestrator ───────────────────────────────────────────

/**
 * Runs all pattern detectors on a function or group of functions (a class).
 * Returns all patterns detected above the confidence threshold.
 */
class PatternDetector {
public:
    struct Config {
        float minConfidence = 0.45f;
        int   minBlocks     = 2;
        int   minInstrs     = 4;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        uint32_t functionsAnalysed = 0;
        uint32_t groupsAnalysed    = 0;
        uint32_t detections        = 0;
        std::unordered_map<PatternKind, uint32_t> byKind;
    };

    using ResultList = std::vector<PatternResult>;

    explicit PatternDetector(Config cfg = defaultConfig());

    ResultList detectFunction(const ssa::SSAFunction& fn) const;
    ResultList detectGroup(const std::vector<const ssa::SSAFunction*>& fns) const;

    const Stats& stats() const { return stats_; }

private:
    Config cfg_;
    mutable Stats stats_;
    std::vector<std::unique_ptr<IPatternDetector>> detectors_;

    bool passesPreflight(const ssa::SSAFunction& fn) const;
};

} // namespace pattern_detect
} // namespace retdec

#endif // RETDEC_PATTERN_DETECT_H
