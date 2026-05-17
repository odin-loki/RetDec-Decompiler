/**
 * @file include/retdec/compiler_abi/abi_descriptor.h
 * @brief Per-compiler ABI descriptor: calling conventions, exception handling,
 *        name demangling, and standard-library function signatures.
 *
 * ## Purpose
 *
 * For each compiler family RetDec supports, an AbiDescriptor captures every
 * ABI detail the decompiler needs to:
 *   1. Correctly map function arguments to parameters (CallingConvention)
 *   2. Detect and annotate exception-handling edges (EhModel)
 *   3. Convert mangled symbol names to C++ declarations (Demangler)
 *   4. Recognise and annotate standard-library calls (StdlibSigTable)
 *
 * ## Supported compilers
 *
 *   CompilerFamily::GCC_Clang  — Itanium ABI, Linux/macOS/Android
 *   CompilerFamily::MSVC       — MSVC ABI, Windows
 *   CompilerFamily::Borland    — Borland/Embarcadero C++ Builder, Windows
 *   CompilerFamily::DMC        — Digital Mars C++, Windows/DOS
 *   CompilerFamily::Watcom     — Open Watcom C++, DOS/Windows/OS2
 *   CompilerFamily::Symbian    — EPOC/Symbian C++, ARM mobile
 *
 * ## Calling conventions
 *
 * Each CallingConventionDesc describes:
 *   - Which integer/pointer registers carry parameters (in order)
 *   - Which float/vector registers carry float parameters
 *   - Which register carries the return value
 *   - Whether the callee or caller cleans the stack
 *   - Whether the `this` pointer has a fixed register
 *   - Stack growth direction and alignment requirement
 *
 * ## EH models
 *
 *   EhModel::None          — no C++ exceptions (C code)
 *   EhModel::ItaniumDwarf  — DWARF2 EH with .eh_frame / .gcc_except_table
 *   EhModel::MsvcSeh       — Structured Exception Handling (SEH, _except_handler)
 *   EhModel::MsvcEh        — MSVC C++ EH on top of SEH (_CxxFrameHandler3/4)
 *   EhModel::BorlandEh     — Borland __ExceptionHandler + try/finally tables
 *   EhModel::DmcEh         — DMC custom EH via __exception_handler symbol
 *   EhModel::WatcomEh      — Watcom __WEH_* runtime routines
 *   EhModel::SymbianLeave  — Symbian Leave/TRAP/CleanupStack mechanism
 *
 * ## Standard library signatures
 *
 * StdlibSig pairs a mangled or plaintext symbol name with a human-readable
 * C++ prototype string and a RetDec-specific type annotation.  The decompiler
 * uses these to replace opaque library call sites with typed calls.
 */

#ifndef RETDEC_COMPILER_ABI_ABI_DESCRIPTOR_H
#define RETDEC_COMPILER_ABI_ABI_DESCRIPTOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace compiler_abi {

// ─── Compiler family ──────────────────────────────────────────────────────────

enum class CompilerFamily : uint8_t {
    Unknown,
    GCC_Clang,  ///< GCC / Clang (Itanium ABI)
    MSVC,       ///< Microsoft Visual C++
    Borland,    ///< Borland / Embarcadero C++ Builder
    DMC,        ///< Digital Mars C++
    Watcom,     ///< Open Watcom C++
    Symbian,    ///< EPOC / Symbian C++ (ARM)
};

const char* compilerFamilyName(CompilerFamily f) noexcept;

// ─── Register model ───────────────────────────────────────────────────────────

enum class Arch : uint8_t {
    X86_32,
    X86_64,
    ARM32,
    ARM64,
    MIPS32,
    MIPS64,
};

/// Named register ID within a specific architecture.
enum class RegId : uint8_t {
    None = 0,
    // x86-32
    EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP,
    // x86-64
    RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP,
    R8,  R9,  R10, R11, R12, R13, R14, R15,
    // float/SSE x86
    XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
    // ARM32
    R0, R1, R2, R3, R4, R5, R6, R7, LR, PC, SP,
    // ARM64
    X0, X1, X2, X3, X4, X5, X6, X7,
    D0, D1, D2, D3, D4, D5, D6, D7,  // NEON float
};

// ─── Calling convention descriptor ───────────────────────────────────────────

/**
 * Describes one calling convention completely.
 *
 * param_regs:  ordered list of integer/pointer parameter registers.
 * float_regs:  ordered list of floating-point parameter registers.
 * return_reg:  primary integer return register.
 * return_float_reg: primary float return register.
 * this_reg:    register carrying `this` (or None if on stack).
 * callee_cleanup: true if the callee pops its own arguments from the stack.
 * stack_align: required stack alignment before CALL (bytes).
 * args_left_to_right: true = leftmost arg pushed first (Pascal/Borland).
 */
struct CallingConventionDesc {
    std::string            name;
    Arch                   arch              = Arch::X86_32;
    std::vector<RegId>     param_regs;       ///< Integer param registers
    std::vector<RegId>     float_regs;       ///< Float param registers
    RegId                  return_reg        = RegId::None;
    RegId                  return_float_reg  = RegId::None;
    RegId                  this_reg          = RegId::None;
    bool                   callee_cleanup    = false;
    uint32_t               stack_align       = 4;
    bool                   args_left_to_right= false; ///< Pascal order
    uint32_t               shadow_space      = 0;     ///< Win64 home space bytes
};

// ─── Exception handling model ─────────────────────────────────────────────────

enum class EhModel : uint8_t {
    None,
    ItaniumDwarf,   ///< .eh_frame + .gcc_except_table, personality = __gxx_personality_v0
    MsvcSeh,        ///< Win32 SEH  (_except_handler3/4)
    MsvcEh,         ///< MSVC C++ EH on top of SEH (_CxxFrameHandler3)
    BorlandEh,      ///< Borland __ExceptionHandler, try/finally tables
    DmcEh,          ///< DMC __exception_handler
    WatcomEh,       ///< Watcom __WEH_xxx runtime
    SymbianLeave,   ///< Symbian Leave()/User::Leave() + CleanupStack
};

struct EhModelDesc {
    EhModel     model            = EhModel::None;
    std::string personalityFunc; ///< Name of personality / EH-frame function
    std::string landingPadFunc;  ///< Name of the catch-dispatch function
    std::string throwFunc;       ///< Name of the throw/raise function
    std::string rethrowFunc;
    std::string terminateFunc;
    // Symbian-specific
    std::string leaveFunc;       ///< "User::Leave" or "__Leave"
    std::string trapSetupFunc;   ///< "TRAP" macro expansion
    std::string cleanupPushFunc; ///< "CleanupStack::PushL"
    std::string cleanupPopFunc;  ///< "CleanupStack::PopAndDestroy"
};

// ─── Standard library signature table ────────────────────────────────────────

enum class StdlibParamDir : uint8_t { In, Out, InOut };

struct StdlibParam {
    std::string      typeName;
    std::string      paramName;
    StdlibParamDir   dir = StdlibParamDir::In;
};

struct StdlibSig {
    std::string             mangledName;   ///< Exact symbol to match
    std::string             plainName;     ///< Unmangled / alternate name
    std::string             returnType;
    std::vector<StdlibParam> params;
    bool                    noReturn   = false;
    bool                    pure       = false;  ///< No side effects
    std::string             briefDoc;
};

// ─── Name demangler ───────────────────────────────────────────────────────────

/**
 * Demangler interface — converts mangled symbol names to human-readable form.
 * Each compiler family provides a concrete implementation.
 */
class INameDemangler {
public:
    virtual ~INameDemangler() = default;

    /// Demangle a single symbol.  Returns the original string on failure.
    virtual std::string demangle(const std::string& mangled) const = 0;

    /// Return true if `s` looks like a mangled name for this compiler.
    virtual bool isMangled(const std::string& s) const noexcept = 0;

    virtual CompilerFamily family() const noexcept = 0;
};

// Concrete demanglers (defined in abi_descriptor.cpp)
std::unique_ptr<INameDemangler> makeDemangler(CompilerFamily f);

// ─── Full ABI descriptor ──────────────────────────────────────────────────────

struct AbiDescriptor {
    CompilerFamily            family     = CompilerFamily::Unknown;
    Arch                      arch       = Arch::X86_32;

    /// Primary calling convention for free functions.
    CallingConventionDesc     defaultCC;

    /// Member function (this-call) convention.
    CallingConventionDesc     memberCC;

    /// Variadic function convention (if different from defaultCC).
    CallingConventionDesc     variadicCC;

    /// Exception handling model.
    EhModelDesc               ehModel;

    /// Standard library / runtime signatures.
    std::vector<StdlibSig>    stdlibSigs;

    // ── Factory ───────────────────────────────────────────────────────────────

    /**
     * Build a pre-populated AbiDescriptor for the given compiler + arch.
     * Returns a descriptor with all fields correctly initialised.
     */
    static AbiDescriptor forCompiler(CompilerFamily f, Arch a);

    // ── Helpers ───────────────────────────────────────────────────────────────

    const StdlibSig* findSig(const std::string& name) const noexcept;
};

// ─── Convenience free functions ───────────────────────────────────────────────

/// Return the standard calling conventions table for a given arch.
std::vector<CallingConventionDesc> standardCallingConventions(Arch arch);

/// Return a StdlibSig table for the given compiler family.
std::vector<StdlibSig> stdlibSignatures(CompilerFamily f);

} // namespace compiler_abi
} // namespace retdec

#endif // RETDEC_COMPILER_ABI_ABI_DESCRIPTOR_H
