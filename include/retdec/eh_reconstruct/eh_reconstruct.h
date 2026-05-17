/**
 * @file include/retdec/eh_reconstruct/eh_reconstruct.h
 * @brief Exception handling table-driven reconstruction.
 *
 * ## What this module recovers
 *
 * Modern C++ compilers generate structured metadata tables describing every
 * function's exception behaviour.  This module parses those tables and
 * produces a language-level try/catch block tree that the decompiler can
 * emit directly as source code.
 *
 * ## MSVC x64 EH (Structured Exception Handling)
 *
 * MSVC stores EH metadata in the PE exception directory:
 *
 *   IMAGE_DIRECTORY_ENTRY_EXCEPTION → RUNTIME_FUNCTION[]
 *   Each RUNTIME_FUNCTION { BeginAddress, EndAddress, UnwindInfoAddress }
 *
 *   UNWIND_INFO { Version, Flags, SizeOfProlog, CountOfCodes,
 *                 FrameRegister, FrameOffset, UnwindCode[] }
 *
 *   Unwind codes (UNWIND_CODE):
 *     UWOP_PUSH_NONVOL(reg)  → callee-save push, gives reg/offset
 *     UWOP_ALLOC_LARGE/SMALL → SUB RSP, N (frame size)
 *     UWOP_SET_FPREG         → MOV rbp, rsp (or similar)
 *     UWOP_SAVE_NONVOL       → callee-save to frame slot
 *     UWOP_SAVE_XMM128       → XMM save slot
 *     UWOP_PUSH_MACH_FRAME   → kernel-mode machine frame
 *
 *   C++ EH extension (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER):
 *   After the unwind codes (aligned to 4 bytes):
 *     FuncInfo { magicNumber, maxState, pUnwindMap, nTryBlocks,
 *                pTryBlockMap, nIPMapEntries, pIPtoStateMap,
 *                pESTypeList, EHFlags }
 *     TryBlockMapEntry { tryLow, tryHigh, catchHigh, nCatches,
 *                        pHandlerArray }
 *     HandlerType { adjectives, pType (TypeDescriptor*), dispCatchObj,
 *                   addressOfHandler }
 *     TypeDescriptor { pVFTable (RTTI), spare, name[] }
 *
 * ## Itanium DWARF EH (.eh_frame + LSDA)
 *
 *   .eh_frame: a series of CIE + FDE records.
 *     CIE (Common Information Entry): augmentation, code_align, data_align,
 *       return_column, augmentation data (personality pointer, LSDA encoding).
 *     FDE (Frame Description Entry): initial_location (function start),
 *       address_range (function length), augmentation data (LSDA pointer),
 *       CFA directives (DW_CFA_def_cfa, DW_CFA_register, DW_CFA_offset,
 *       DW_CFA_restore, DW_CFA_remember_state, DW_CFA_restore_state).
 *
 *   LSDA (Language Specific Data Area):
 *     lpstart_encoding / lpstart: base for landing pad addresses.
 *     ttype_encoding / ttype_base: base for type table entries.
 *     call_site_encoding: encoding of call-site table entries.
 *     call-site table: { cs_start, cs_len, landing_pad, action } per site.
 *     action table: { type_filter, next } per action.
 *     type table: type_info* entries (backward from ttype_base).
 *
 *   Semantics:
 *     landing_pad == 0            → no handler for this range (terminate)
 *     action == 0                 → cleanup only (no catch)
 *     type_filter == 0 in action  → catch(...)
 *     type_filter > 0             → catch specific type (type_table[-filter])
 *     type_filter < 0             → exception specification (rare)
 *
 * ## Output representation
 *
 * Both parsers produce a unified `EHFunction` structure:
 *   - `unwindInfo`   — callee-save register locations (for VariableRecovery)
 *   - `tryCatchBlocks` — nested try/catch tree
 *
 * Each `TryCatchBlock` has:
 *   - `tryRange`     — [begin, end) VMA of the try body
 *   - `handlers[]`   — catch handlers in order
 *
 * Each `CatchHandler` has:
 *   - `handlerVma`   — entry point of the catch block
 *   - `catchType`    — demangled type name ("..." for catch-all, "" for cleanup)
 *   - `catchVarOffset` — stack offset of the caught exception object
 *   - `isRethrow`    — true if handler immediately rethrows
 *   - `isCatchAll`   — true for catch(...)
 *   - `isCleanup`    — true for destructor cleanup with no user-visible catch
 */

#ifndef RETDEC_EH_RECONSTRUCT_H
#define RETDEC_EH_RECONSTRUCT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace eh_reconstruct {

// ─── Register save record ─────────────────────────────────────────────────────

/**
 * One callee-saved register's location in the stack frame.
 * Used by VariableRecovery to exclude these stack slots from decompiled output.
 */
struct RegSave {
    uint32_t regId;         ///< Architecture register number
    std::string regName;    ///< Human-readable name (e.g. "rbx", "r12")
    int32_t  frameOffset;   ///< Byte offset from frame base (RBP or RSP-based)
    bool     isXmm;         ///< True for XMM/YMM registers
    uint32_t xmmWidth;      ///< 16 or 32 bytes for XMM saves
};

/**
 * UNWIND_INFO parsed from a RUNTIME_FUNCTION entry (MSVC) or
 * CFA directives from an FDE (Itanium).
 */
struct UnwindInfo {
    uint64_t functionBegin = 0;    ///< Function start VMA
    uint64_t functionEnd   = 0;    ///< Function end VMA (exclusive)
    uint32_t frameSize     = 0;    ///< Total stack frame size in bytes
    uint32_t prologSize    = 0;    ///< Prolog size in bytes
    uint32_t frameReg      = 0;    ///< Frame pointer register (0 = RSP-based)
    uint32_t frameRegOffset= 0;    ///< Frame register offset
    std::vector<RegSave> regSaves; ///< Callee-saved register locations
    bool     hasChainedUnwind = false; ///< MSVC chained UNWIND_INFO
    uint64_t chainedInfoVma   = 0;
};

// ─── Try/catch tree ───────────────────────────────────────────────────────────

/**
 * One catch handler for a try block.
 */
struct CatchHandler {
    uint64_t    handlerVma    = 0;  ///< VMA of handler entry point
    uint64_t    handlerEndVma = 0;  ///< VMA of handler end (if known)
    std::string catchType;          ///< Demangled catch type ("..." = catch-all, "" = cleanup)
    int32_t     catchVarOffset= 0;  ///< Stack offset of caught object
    bool        isCatchAll    = false; ///< catch(...)
    bool        isCleanup     = false; ///< Destructor cleanup, not visible in source
    bool        isRethrow     = false; ///< Handler immediately rethrows
};

/**
 * One try/catch block.  Handlers are in source order (most derived first).
 */
struct TryCatchBlock {
    uint64_t tryBegin  = 0;   ///< First VMA in the try body
    uint64_t tryEnd    = 0;   ///< First VMA after the try body
    std::vector<CatchHandler> handlers;
    std::vector<TryCatchBlock> nested; ///< Nested try blocks within this one

    bool hasCleanupOnly() const {
        return handlers.size()==1 && handlers[0].isCleanup;
    }
    bool hasCatchAll() const {
        for (auto& h : handlers) if (h.isCatchAll) return true;
        return false;
    }
};

/**
 * All EH information recovered for one function.
 */
struct EHFunction {
    uint64_t     functionVma  = 0;   ///< Function start VMA
    uint64_t     functionEnd  = 0;   ///< Function end VMA (exclusive)
    UnwindInfo   unwindInfo;
    std::vector<TryCatchBlock> tryCatchBlocks; ///< Top-level try blocks
    bool         isNoexcept   = false; ///< Function has empty exception spec
    bool         hasEH        = false; ///< True if any try/catch blocks found
    std::string  personalityFn; ///< Personality function name (for diagnostics)
};

// ─── Binary view ─────────────────────────────────────────────────────────────

/**
 * Read-only view of the binary image, same interface as string_detect.
 */
class IBinaryView {
public:
    virtual ~IBinaryView() = default;
    virtual std::size_t readBytes(uint64_t vma, uint8_t* buf, std::size_t len) const = 0;
    virtual bool isMapped(uint64_t vma) const = 0;
    virtual bool isDataSection(uint64_t vma) const = 0;
    virtual uint64_t imageBase() const noexcept = 0;
    virtual uint64_t sectionVma(const char* name) const noexcept = 0;
    virtual std::size_t sectionSize(const char* name) const noexcept = 0;

    // Convenience helpers
    uint8_t  readU8(uint64_t vma) const;
    uint16_t readU16LE(uint64_t vma) const;
    uint32_t readU32LE(uint64_t vma) const;
    uint64_t readU64LE(uint64_t vma) const;
    int32_t  readI32LE(uint64_t vma) const;

    // DWARF LEB128 readers
    uint64_t readULEB128(uint64_t& cursor) const;
    int64_t  readSLEB128(uint64_t& cursor) const;

    // DWARF pointer encoding (DW_EH_PE_*)
    uint64_t readEncodedPtr(uint64_t& cursor, uint8_t encoding,
                             uint64_t pcrel_base = 0) const;
};

// ─── EH parser interface ──────────────────────────────────────────────────────

/**
 * Abstract interface for ABI-specific EH table parsers.
 */
class IEHParser {
public:
    virtual ~IEHParser() = default;
    virtual const char* name() const noexcept = 0;

    /**
     * Parse all EH data for the binary and return one EHFunction per function
     * that has EH metadata.
     */
    virtual std::vector<EHFunction> parse(const IBinaryView& view) const = 0;
};

// ─── EH reconstructor ────────────────────────────────────────────────────────

/**
 * Callback types.
 */
using EHFunctionCallback = std::function<void(const EHFunction&)>;

/**
 * Orchestrates all registered EH parsers.
 * Usage:
 *   EHReconstructor rec;
 *   rec.addParser(makeMsvcEHParser());
 *   rec.addParser(makeItaniumEHParser());
 *   rec.onFunction([](auto& f){ ... });
 *   rec.reconstruct(view);
 */
class EHReconstructor {
public:
    EHReconstructor();
    ~EHReconstructor();
    EHReconstructor(const EHReconstructor&) = delete;
    EHReconstructor& operator=(const EHReconstructor&) = delete;
    EHReconstructor(EHReconstructor&&) = default;
    EHReconstructor& operator=(EHReconstructor&&) = default;

    void addParser(std::unique_ptr<IEHParser> parser);
    void onFunction(EHFunctionCallback cb) { cb_ = std::move(cb); }

    /**
     * Run all registered parsers against the binary view.
     * Returns total number of EHFunction records produced.
     */
    std::size_t reconstruct(const IBinaryView& view);

    const std::vector<EHFunction>& functions() const { return functions_; }

    /**
     * Look up the EHFunction covering a given instruction VMA.
     * Returns nullptr if not found.
     */
    const EHFunction* findFunction(uint64_t vma) const;

    /**
     * Find the innermost TryCatchBlock covering `vma` within `func`.
     * Returns nullptr if none.
     */
    static const TryCatchBlock* findInnermostTry(const EHFunction& func,
                                                   uint64_t vma);

private:
    std::vector<std::unique_ptr<IEHParser>> parsers_;
    std::vector<EHFunction>                 functions_;
    EHFunctionCallback                      cb_;
};

// ─── Factory functions ────────────────────────────────────────────────────────

/// Parser for MSVC x64 SEH (IMAGE_DIRECTORY_ENTRY_EXCEPTION + FuncInfo).
std::unique_ptr<IEHParser> makeMsvcEHParser();

/// Parser for Itanium DWARF EH (.eh_frame + LSDA).
std::unique_ptr<IEHParser> makeItaniumEHParser();

/// Parser for ARM EHABI (.ARM.exidx prel31 table + .ARM.extab compact/generic unwind).
/// Handles compact model 0/1/2 unwind byte opcodes and Itanium-compatible LSDA.
std::unique_ptr<IEHParser> makeArmEhabiParser();

/// Parser for Borland/Embarcadero C++ 32-bit frame-chain EH.
/// Scans for PUSH <handler>; PUSH FS:[0]; MOV FS:[0],ESP prologues and
/// parses the referenced static try/catch handler tables.
std::unique_ptr<IEHParser> makeBorlandEHParser();

/// Parser for Digital Mars C++ (DMC) frame-chain EH.
/// Detects DMCExcFrame / ScopeTable patterns and reconstructs try/catch blocks.
std::unique_ptr<IEHParser> makeDmcEHParser();

/// Parser for Open Watcom C++ exception tables (.eh_data section).
/// Parses the WEXC-magic header, WExcEntry records, and handler tables.
std::unique_ptr<IEHParser> makeWatcomEHParser();

/// Parser for Symbian/EPOC C++ Leave-Trap pattern reconstruction.
/// Detects TTrap::Trap / User::Leave / CleanupStack sequences in ARM code.
std::unique_ptr<IEHParser> makeSymbianEHParser();

/// Build a reconstructor pre-populated with all seven parsers.
/// MSVC and Itanium parsers run first; the five others follow.
EHReconstructor makeDefaultReconstructor();

} // namespace eh_reconstruct
} // namespace retdec

#endif // RETDEC_EH_RECONSTRUCT_H
