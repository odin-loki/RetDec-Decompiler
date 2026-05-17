/**
 * @file include/retdec/string_detect/string_detect.h
 * @brief Reference-anchored string detection for decompiled binaries.
 *
 * ## Why reference-anchored?
 *
 * Traditional string finders (e.g. the `strings` utility) perform a blind
 * linear scan of all binary sections looking for runs of printable bytes.
 * This has two fundamental problems:
 *
 *   1. **False positives**: constants, code bytes, padding, and embedded data
 *      that look printable but are not strings are picked up.
 *   2. **Missed strings**: Short Strings Optimisation (SSO) strings stored
 *      inline in an object's buffer never appear at a stable address and are
 *      therefore invisible to a linear scan.
 *
 * Reference-anchored detection instead starts from instruction operands:
 *   - `LEA reg, [RIP+disp]` (x86-64 PC-relative) → address of .rodata datum
 *   - `MOV reg, imm64` where the immediate falls in a data section → same
 *   - `MOVZ/MOVK` chain (AArch64) computing a rodata address
 *   - `LDR reg, [PC, #off]` (ARM32/Thumb) → literal pool address
 *
 * Only addresses that are ACTUALLY REFERENCED by code are candidates.
 * This eliminates nearly all false positives.
 *
 * ## String kinds recognised
 *
 * | Kind             | Terminator / Length field       | Compiler / Language |
 * |------------------|---------------------------------|---------------------|
 * | C NUL-terminated | trailing 0x00                   | C, C++, Rust FFI   |
 * | Pascal           | first byte = length             | Borland/Delphi      |
 * | Length-prefixed  | first 4 bytes (u32) = length    | MSVC std::string    |
 * | UTF-16LE         | trailing 0x0000                 | Windows WideChar    |
 * | UTF-16BE         | trailing 0x0000 (big-endian)    | Java/JVM strings    |
 * | UTF-8            | NUL-terminated, multi-byte seq  | Modern apps         |
 *
 * ## Encoding detection
 *
 * `EncodingDetector` uses byte-frequency analysis in <100 bytes:
 *   - ASCII:    all bytes in [0x09,0x0D] ∪ [0x20,0x7E]
 *   - UTF-8:    valid multi-byte sequences (0xC2-0xF4 lead bytes)
 *   - Latin-1:  bytes in [0xA0,0xFF] (ISO-8859-1 extension range)
 *   - UTF-16LE: even positions 0x00, odd positions non-zero → alternating
 *   - UTF-16BE: odd positions 0x00, even positions non-zero → alternating
 *
 * ## SSO branch recognition
 *
 * The Short String Optimisation stores strings ≤ threshold bytes directly
 * inside the string object rather than heap-allocating.  The compiler emits:
 *
 *   if (len < THRESHOLD) {
 *       // store in inline buffer (object internal storage)
 *   } else {
 *       // heap allocate
 *   }
 *
 * Thresholds by implementation:
 *   - libstdc++ (GCC):   SSO threshold = 15  (fits in 16 bytes incl NUL)
 *   - MSVC STL:          SSO threshold = 15
 *   - libc++ (Clang):    SSO threshold = 22  (23-char capacity)
 *   - folly fbstring:    SSO threshold = 23
 *
 * Detection looks for a compare-then-branch where the compare operand
 * matches one of the known thresholds, following a string constructor call.
 *
 * ## String table detection
 *
 * A region where N consecutive pointer-sized values all point into
 * string-containing sections is classified as `const char* table[N]`.
 * This is common in switch-dispatch tables, error message arrays, etc.
 *
 * ## ARM literal pools
 *
 * ARM32 and Thumb compilers emit literal pools: constant values embedded in
 * the code section accessed by `LDR Rd, [PC, #offset]`.  The target of each
 * such instruction must be marked as DATA to prevent the disassembler from
 * attempting to decode those bytes as instructions.
 */

#ifndef RETDEC_STRING_DETECT_H
#define RETDEC_STRING_DETECT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace string_detect {

// ─── Encoding ────────────────────────────────────────────────────────────────

enum class EncodingKind : uint8_t {
    Unknown,
    ASCII,      ///< 7-bit ASCII, NUL-terminated
    UTF8,       ///< UTF-8, NUL-terminated
    Latin1,     ///< ISO-8859-1, NUL-terminated
    UTF16LE,    ///< UTF-16 little-endian, 0x0000-terminated
    UTF16BE,    ///< UTF-16 big-endian, 0x0000-terminated
    UTF32LE,    ///< UTF-32 little-endian, 0x00000000-terminated
};

const char* encodingName(EncodingKind e) noexcept;

// ─── String kind ─────────────────────────────────────────────────────────────

enum class StringKind : uint8_t {
    CNulTerminated,  ///< Standard C string: chars + 0x00
    Pascal,          ///< Borland Pascal: u8 length + chars (max 255)
    LengthPrefixed,  ///< MSVC/COM: u32 length + chars (BSTR-like)
    Wide,            ///< Wide string (UTF-16LE/BE), 0x0000-terminated
    SSOInline,       ///< Short String Opt: stored in object buffer, no stable addr
    TableEntry,      ///< Entry in a pointer-to-string table
};

const char* stringKindName(StringKind k) noexcept;

// ─── String literal record ────────────────────────────────────────────────────

/**
 * A recovered string literal.
 */
struct StringLiteral {
    uint64_t    address     = 0;    ///< VMA of first byte (0 for SSOInline)
    std::size_t byteLength  = 0;    ///< Total byte length including terminator
    std::size_t charCount   = 0;    ///< Number of characters (excluding terminator)
    StringKind  kind        = StringKind::CNulTerminated;
    EncodingKind encoding   = EncodingKind::Unknown;
    std::string  value;             ///< UTF-8 representation of the string content
    uint64_t     refVma     = 0;    ///< VMA of the referencing instruction (0 if table)
    bool         inCodeSection = false; ///< True for ARM literal pool strings
    bool         isTableEntry  = false;

    bool valid() const noexcept { return byteLength > 0; }
    std::string debugStr() const;
};

// ─── Binary view (read-only byte accessor) ────────────────────────────────────

/**
 * Abstract read-only view of the binary image.
 * Supplied by the caller; keeps string_detect free of fileformat dependency.
 */
class IBinaryView {
public:
    virtual ~IBinaryView() = default;

    /// Read up to `maxLen` bytes from `vma` into `buf`.
    /// Returns number of bytes actually read (may be 0 if unmapped).
    virtual std::size_t readBytes(uint64_t vma,
                                   uint8_t* buf,
                                   std::size_t maxLen) const = 0;

    /// True if `vma` lies in a data section (not code).
    virtual bool isDataSection(uint64_t vma) const = 0;

    /// True if `vma` lies in a code/text section.
    virtual bool isCodeSection(uint64_t vma) const = 0;

    /// True if `vma` is a valid mapped address.
    virtual bool isMapped(uint64_t vma) const = 0;

    /// Pointer width: 4 for 32-bit binaries, 8 for 64-bit.
    virtual uint32_t pointerWidth() const noexcept = 0;

    /// Read a pointer-sized value from `vma` (little-endian).
    uint64_t readPointer(uint64_t vma) const;
};

// ─── Reference from instruction decoder ──────────────────────────────────────

/**
 * A data reference emitted by the instruction decoder.
 * One per LEA/MOV-imm/LDR-PC that references a data address.
 */
struct InstrRef {
    uint64_t instrVma;   ///< VMA of the referencing instruction
    uint64_t targetVma;  ///< VMA of the referenced data address
    bool     isLiteralPool; ///< ARM LDR [PC, #off] — target is in code section
};

// ─── String typer ─────────────────────────────────────────────────────────────

/**
 * Encoding detection result.
 */
struct EncodingResult {
    EncodingKind kind       = EncodingKind::Unknown;
    float        confidence = 0.0f; ///< [0, 1]
};

/**
 * Attempt to read and classify a string at `vma`.
 * Returns nullopt if the bytes do not form a plausible string.
 *
 * @param view      Binary image reader.
 * @param vma       Start address.
 * @param maxBytes  Maximum bytes to read (default 4096).
 */
std::optional<StringLiteral> typeString(const IBinaryView& view,
                                         uint64_t            vma,
                                         std::size_t         maxBytes = 4096);

/**
 * Detect the encoding of a byte buffer (without knowing the length).
 */
EncodingResult detectEncoding(const uint8_t* buf, std::size_t len) noexcept;

// ─── SSO detection ───────────────────────────────────────────────────────────

/**
 * Known SSO threshold values by STL implementation.
 */
enum class SSOImpl : uint8_t {
    LibStdCpp,   ///< GCC libstdc++ (threshold 15)
    MsvcStl,     ///< MSVC STL (threshold 15)
    LibCpp,      ///< LLVM libc++ (threshold 22)
    FollyFBString,///< Facebook Folly fbstring (threshold 23)
};

uint32_t ssoThreshold(SSOImpl impl) noexcept;

/**
 * Result of SSO branch analysis at a call site.
 */
struct SSOBranchInfo {
    SSOImpl      impl;
    uint64_t     branchVma;   ///< VMA of the compare+branch instruction
    uint64_t     inlinePath;  ///< VMA of the inline (short) path
    uint64_t     heapPath;    ///< VMA of the heap-allocation path
    uint32_t     threshold;   ///< SSO threshold detected
};

/**
 * Analyse a compare instruction operand to detect an SSO branch.
 *
 * @param compareImm  The immediate value in the compare instruction.
 * @param branchVma   VMA of the compare instruction.
 * @returns SSOBranchInfo if the immediate matches a known threshold.
 */
std::optional<SSOBranchInfo> detectSSOBranch(int64_t  compareImm,
                                               uint64_t branchVma,
                                               uint64_t inlinePath,
                                               uint64_t heapPath) noexcept;

// ─── String table detection ───────────────────────────────────────────────────

/**
 * A detected array of string pointers.
 */
struct StringTable {
    uint64_t              address;    ///< VMA of first pointer
    std::size_t           count;      ///< Number of entries
    std::vector<uint64_t> targets;    ///< Target VMA per entry
    uint32_t              ptrWidth;   ///< 4 or 8 bytes per entry
};

/**
 * Scan a region starting at `vma` for a string pointer table.
 * Reads consecutive pointer-width values and checks each as a string.
 *
 * @param view      Binary view.
 * @param vma       Start address of the candidate region.
 * @param maxEntries Maximum entries to check (default 1024).
 * @returns A StringTable if at least 3 consecutive valid string pointers found.
 */
std::optional<StringTable> detectStringTable(const IBinaryView& view,
                                              uint64_t           vma,
                                              std::size_t        maxEntries = 1024);

// ─── ARM literal pool ─────────────────────────────────────────────────────────

/**
 * One ARM literal pool entry.
 */
struct LiteralPoolEntry {
    uint64_t vma;        ///< VMA of the literal value in the code section
    uint64_t value;      ///< The 32 or 64-bit literal value
    uint32_t width;      ///< 4 or 8 bytes
    bool     isPointer;  ///< True if value is a valid mapped address
};

/**
 * Extract all literal pool entries referenced by instructions in `instrRefs`.
 * Marks each pool entry address so the disassembler skips those bytes.
 */
std::vector<LiteralPoolEntry> extractLiteralPool(
    const IBinaryView&          view,
    const std::vector<InstrRef>& instrRefs);

// ─── Main string detector ─────────────────────────────────────────────────────

/**
 * Callback invoked for each string or table found.
 */
using StringCallback = std::function<void(const StringLiteral&)>;

/**
 * Callback for SSO branch detections.
 */
using SSOCallback = std::function<void(const SSOBranchInfo&)>;

/**
 * Callback for string table detections.
 */
using TableCallback = std::function<void(const StringTable&)>;

/**
 * Callback for literal pool entries (ARM).
 */
using LiteralPoolCallback = std::function<void(const LiteralPoolEntry&)>;

/**
 * Configuration for `StringDetector`.
 */
struct StringDetectorConfig {
    std::size_t maxStringLen    = 4096; ///< Max bytes per string candidate
    std::size_t minStringLen    = 2;    ///< Min char count (excl. terminator)
    bool        detectTables    = true; ///< Enable string-table detection
    bool        detectSSO       = true; ///< Enable SSO branch recognition
    bool        detectLitPool   = true; ///< Enable ARM literal pool marking
    bool        detectWide      = true; ///< Classify UTF-16 strings
    bool        detectPascal    = true; ///< Classify Borland Pascal strings
    bool        detectLenPfx    = true; ///< Classify MSVC length-prefixed strings
    SSOImpl     ssoImpl         = SSOImpl::LibStdCpp;
    uint32_t    tableMinEntries = 3;    ///< Min entries to classify as table
};

/**
 * The main orchestrator.
 *
 * Usage:
 *   StringDetector det(view, config);
 *   det.onString([](auto& s){ ... });
 *   det.onSSO([](auto& b){ ... });
 *   det.onTable([](auto& t){ ... });
 *   det.onLiteralPool([](auto& e){ ... });
 *
 *   // Feed one reference at a time from the instruction decoder:
 *   for (auto& ref : instructionRefs)
 *       det.processRef(ref);
 *
 *   // Or feed SSO branch info:
 *   det.processCompareBranch(compareImm, branchVma, inlinePath, heapPath);
 *
 *   // Or scan a data region for string tables:
 *   det.scanRegion(regionStart, regionEnd);
 */
class StringDetector {
public:
    StringDetector(const IBinaryView&         view,
                   const StringDetectorConfig& cfg = StringDetectorConfig{});
    ~StringDetector();

    void onString(StringCallback cb)      { strCb_     = std::move(cb); }
    void onSSO(SSOCallback cb)            { ssoCb_     = std::move(cb); }
    void onTable(TableCallback cb)        { tableCb_   = std::move(cb); }
    void onLiteralPool(LiteralPoolCallback cb){ litCb_ = std::move(cb); }

    /**
     * Process one instruction reference.
     * If the target address contains a recognisable string, calls onString().
     * If it is an ARM literal pool entry, calls onLiteralPool().
     */
    void processRef(const InstrRef& ref);

    /**
     * Process a batch of references (calls processRef for each).
     * Returns number of strings found.
     */
    std::size_t processRefs(const std::vector<InstrRef>& refs);

    /**
     * Process a compare+branch that might be an SSO branch.
     */
    void processCompareBranch(int64_t  compareImm,
                               uint64_t branchVma,
                               uint64_t inlinePath,
                               uint64_t heapPath);

    /**
     * Scan a data region [start, end) for string pointer tables.
     * Calls onTable() and onString() for each table and its entries.
     */
    void scanRegion(uint64_t start, uint64_t end);

    /**
     * All strings found so far (for batch access).
     */
    const std::vector<StringLiteral>& strings() const { return strings_; }

    /**
     * All SSO branches found.
     */
    const std::vector<SSOBranchInfo>& ssoBranches() const { return sso_; }

    /**
     * All string tables found.
     */
    const std::vector<StringTable>& tables() const { return tables_; }

    /**
     * All literal pool entries found.
     */
    const std::vector<LiteralPoolEntry>& literalPool() const { return litPool_; }

    /**
     * True if `vma` has already been processed as a string start.
     */
    bool isKnownString(uint64_t vma) const;

    /**
     * True if `vma` should be treated as data (literal pool or string).
     */
    bool isDataMarked(uint64_t vma) const;

private:
    const IBinaryView&          view_;
    StringDetectorConfig        cfg_;
    StringCallback              strCb_;
    SSOCallback                 ssoCb_;
    TableCallback               tableCb_;
    LiteralPoolCallback         litCb_;
    std::vector<StringLiteral>  strings_;
    std::vector<SSOBranchInfo>  sso_;
    std::vector<StringTable>    tables_;
    std::vector<LiteralPoolEntry> litPool_;

    // Addresses already classified (to avoid re-processing)
    std::vector<uint64_t> knownStringAddrs_;
    std::vector<uint64_t> dataMarkedAddrs_;

    void emitString(StringLiteral s);
    void processLiteralPoolEntry(const InstrRef& ref);
};

} // namespace string_detect
} // namespace retdec

#endif // RETDEC_STRING_DETECT_H
