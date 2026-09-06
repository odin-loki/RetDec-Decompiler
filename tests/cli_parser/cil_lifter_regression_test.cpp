/**
 * @file tests/cli_parser/cil_lifter_regression_test.cpp
 * @brief Regression tests for the CIL lifter's file-controlled arithmetic.
 *
 * Every test here pins one defect that an SMT counterexample refuted against
 * the expression as it was written, and every one of them fails if the fix at
 * the named site is reverted. The counterexample is quoted in the test that
 * covers it, so a future reader can see what the number was rather than only
 * that there was one.
 *
 * The inputs are hand-assembled method bodies. Nothing here is a real .NET
 * assembly, because none of these values survive a compiler -- they are what a
 * file that was written to be parsed rather than executed contains.
 */

#include "retdec/cli_parser/cil_lifter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace retdec::cli_parser;
using namespace retdec::bc_module;

namespace {

void u8 (std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

/// ECMA-335 II.25.4.2: a tiny method header is one byte, low 2 bits 0x2 and the
/// code size in the high 6. Callers below keep @p code under 64 bytes.
std::vector<uint8_t> tinyBody(const std::vector<uint8_t>& code) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(0x02 | (code.size() << 2)));
    body.insert(body.end(), code.begin(), code.end());
    return body;
}

/// ECMA-335 II.25.4.3: a fat header is 12 bytes -- Flags/Size word, MaxStack,
/// CodeSize, LocalVarSigTok. Flags 0x3 is the fat marker; 0x8 is
/// CorILMethod_MoreSects, which says an exception-handling section follows the
/// code. The high nibble of the first word is the header size in dwords (3).
constexpr uint16_t kFatFlagsMoreSects = static_cast<uint16_t>((3u << 12) | 0x00B);

std::vector<uint8_t> fatBodyWithSections(const std::vector<uint8_t>& code,
                                         const std::vector<uint8_t>& sections) {
    std::vector<uint8_t> body;
    u16(body, kFatFlagsMoreSects);
    u16(body, 8);                                       // MaxStack
    u32(body, static_cast<uint32_t>(code.size()));      // CodeSize
    u32(body, 0);                                       // LocalVarSigTok
    body.insert(body.end(), code.begin(), code.end());
    // II.25.4.5: the section table is 4-byte aligned. The callers below pick a
    // code size that already lands on a boundary except where the test is
    // about the alignment itself.
    while (body.size() % 4) body.push_back(0);
    body.insert(body.end(), sections.begin(), sections.end());
    return body;
}

/// One fat exception clause, 24 bytes (II.25.4.6).
void fatClause(std::vector<uint8_t>& v, uint32_t kind, uint32_t tryOffset,
               uint32_t tryLength, uint32_t handlerOffset,
               uint32_t handlerLength, uint32_t classToken) {
    u32(v, kind);
    u32(v, tryOffset);
    u32(v, tryLength);
    u32(v, handlerOffset);
    u32(v, handlerLength);
    u32(v, classToken);
}

bool hasBlockOperand(const BcCFG& cfg) {
    for (const auto& blk : cfg.blocks())
        for (const auto& insn : blk.instrs)
            for (const auto& op : insn.operands)
                if (std::get_if<BcBlockOperand>(&op)) return true;
    return false;
}

} // namespace

// ─── cil_lifter.cpp:341/349 — the declared clause count wrapped ──────────────

// A small exception-handling section whose DataSize field is 0.
//
// DataSize counts the 4-byte section header, so the clause count was
// `(dataSize - 4) / 12`. At dataSize = 0 that subtraction wraps and the count
// is 1537228672809129301: the parser then read every remaining byte of the body
// as exception clauses, stopping only when it ran out of buffer. The body here
// supplies exactly twelve trailing bytes, so the wrapped count harvested one
// clause out of what the section said was no data at all.
TEST(CILLifterEhSectionRegression, ZeroDataSizeDeclaresNoClauses) {
    std::vector<uint8_t> sect;
    u8(sect, 0x01);   // Kind: EH, small format, no more sections
    u8(sect, 0x00);   // DataSize = 0 -- the wrap
    u16(sect, 0);     // reserved
    for (int i = 0; i < 12; ++i) u8(sect, 0xAB);  // one small clause of bytes

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_TRUE(hdr.exceptionClauses.empty());
}

// The same for the fat form, where the wrapped count is (0 - 4) / 24 =
// 768614336404564650. Twenty-four trailing bytes are supplied so the wrapped
// count has one fat clause to harvest.
TEST(CILLifterEhSectionRegression, ZeroDataSizeDeclaresNoFatClauses) {
    std::vector<uint8_t> sect;
    u8(sect, 0x41);   // Kind: EH | FatFormat, no more sections
    u8(sect, 0x00);   // DataSize byte 0
    u8(sect, 0x00);   // DataSize byte 1
    u8(sect, 0x00);   // DataSize byte 2 -- DataSize = 0
    for (int i = 0; i < 24; ++i) u8(sect, 0xAB);

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_TRUE(hdr.exceptionClauses.empty());
}

// DataSize = 3 is the largest value that still wraps, and it is not a
// hypothetical byte: it is one less than the header the field is measuring.
TEST(CILLifterEhSectionRegression, DataSizeBelowItsOwnHeaderDeclaresNoClauses) {
    std::vector<uint8_t> sect;
    u8(sect, 0x01);
    u8(sect, 0x03);   // DataSize = 3, one below the 4-byte section header
    u16(sect, 0);
    for (int i = 0; i < 12; ++i) u8(sect, 0xAB);

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_TRUE(hdr.exceptionClauses.empty());
}

// The count must still be believed when the section declares a real one: the
// fix saturates at zero, it does not switch the clause table off.
TEST(CILLifterEhSectionRegression, WellFormedSmallSectionStillYieldsItsClause) {
    std::vector<uint8_t> sect;
    u8(sect, 0x01);
    u8(sect, 4 + 12);  // DataSize = header + one 12-byte clause
    u16(sect, 0);
    u16(sect, 0);      // Flags: catch
    u16(sect, 0);      // TryOffset
    u8(sect, 2);       // TryLength
    u16(sect, 2);      // HandlerOffset
    u8(sect, 2);       // HandlerLength
    u32(sect, 0);      // ClassToken

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    lifter.lift({body.data(), body.size()}, hdr);

    ASSERT_EQ(1u, hdr.exceptionClauses.size());
    EXPECT_EQ(0u, hdr.exceptionClauses[0].tryOffset);
    EXPECT_EQ(2u, hdr.exceptionClauses[0].tryLength);
    EXPECT_EQ(2u, hdr.exceptionClauses[0].handlerOffset);
}

// ─── cil_lifter.cpp:326 — the section table must be found 4-byte aligned ─────

// Not a counterexample test: the wrap in `(sectStart + 3) & ~3ULL` needs a
// position within 3 bytes of SIZE_MAX, and sectStart is bounded by body.size(),
// so no input to lift() reaches it on a 64-bit host. What this pins is that the
// call site still rounds, and rounds forward: a method whose code ends at an
// odd offset keeps its exception section, which it would not if the rounding
// were dropped or replaced by something that moves the cursor the other way.
TEST(CILLifterEhSectionRegression, SectionTableIsFoundAfterUnalignedCode) {
    std::vector<uint8_t> sect;
    u8(sect, 0x01);
    u8(sect, 4 + 12);
    u16(sect, 0);
    u16(sect, 0);      // Flags: catch
    u16(sect, 1);      // TryOffset
    u8(sect, 1);       // TryLength
    u16(sect, 2);      // HandlerOffset
    u8(sect, 1);       // HandlerLength
    u32(sect, 0);

    // Code size 3 leaves the section table one pad byte away from the end of
    // the code: 12 + 3 = 15 rounds up to 16.
    auto body = fatBodyWithSections({0x00, 0x00, 0x2A}, sect);
    ASSERT_EQ(16u + 16u, body.size());

    CILLifter lifter;
    CILMethodHeader hdr;
    lifter.lift({body.data(), body.size()}, hdr);

    ASSERT_EQ(1u, hdr.exceptionClauses.size());
    EXPECT_EQ(1u, hdr.exceptionClauses[0].tryOffset);
}

// ─── cil_lifter.cpp:522-523 / 532-533 — branch targets outside the method ────

// br.s with a displacement of -128 from a 2-byte method.
//
// The site widened to int64 -- so the sum was right -- and then truncated to
// uint32 with no range check. ESBMC's witness for the backward case is
// pos = 16, delta = -128 in a 32769-byte method: the true target is -112 and
// the recorded leader was 4294967184 (0xFFFFFF90), 4 GB past the end. Here
// pos = 2 and delta = -128 gives the true target -126 and the old leader
// 0xFFFFFF82, which buildCFG turned into a basic block that no instruction
// falls into and made a successor of the block containing the branch.
TEST(CILLifterBranchTargetRegression, BackwardShortBranchBeforeCodeIsNotALeader) {
    auto body = tinyBody({0x2B, 0x80});  // br.s -128

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_FALSE(hasBlockOperand(cfg));
    // Leaders are the method entry and the instruction after an unconditional
    // branch. Without the fix 0xFFFFFF82 is a third.
    EXPECT_EQ(2u, cfg.blockCount());
}

// The forward half of the same defect: pos = 2 with delta = +127 in a 3-byte
// method targets 129, which is 126 bytes past the end. ESBMC's witness is
// codeSize = 1058, pos = 1007, delta = 98, target = 1105.
TEST(CILLifterBranchTargetRegression, ForwardShortBranchPastCodeIsNotALeader) {
    auto body = tinyBody({0x2B, 0x7F, 0x2A});  // br.s +127 ; ret

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_FALSE(hasBlockOperand(cfg));
    // Leaders: the entry, the byte after the branch (2) and the byte after the
    // ret (3). Without the fix 129 is a fourth.
    EXPECT_EQ(3u, cfg.blockCount());
}

// The four-byte form, 0x38 `br`, with the same defect and a displacement wide
// enough to make the truncation obvious: pos = 5, delta = -0x40000000 has the
// true target -1073741819 and truncated to uint32 it is 0xC0000005.
TEST(CILLifterBranchTargetRegression, BackwardLongBranchBeforeCodeIsNotALeader) {
    std::vector<uint8_t> code;
    u8(code, 0x38);              // br <int32>
    u32(code, 0xC0000000u);      // delta = -1073741824
    u8(code, 0x2A);              // ret
    auto body = tinyBody(code);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_FALSE(hasBlockOperand(cfg));
    // Leaders: the entry, the byte after the branch (5) and the byte after the
    // ret (6). Without the fix 0xC0000005 is a fourth.
    EXPECT_EQ(3u, cfg.blockCount());
}

// A branch that does land inside the method must still be recorded: the fix
// refuses out-of-range targets, it does not stop resolving branches.
TEST(CILLifterBranchTargetRegression, InRangeShortBranchIsStillALeader) {
    // ldc.i4.0 ; brfalse.s +1 ; ldc.i4.1 ; ret  -- target 4, inside the method.
    auto body = tinyBody({0x16, 0x2C, 0x01, 0x17, 0x2A});

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_TRUE(hasBlockOperand(cfg));
    EXPECT_GE(cfg.blockCount(), 2u);
}

// ─── cil_lifter.cpp:549-554 — the switch's own base offset ───────────────────

// A switch declaring 3221234185 cases in a 5-byte method.
//
// The base every case target is measured from was
// `(uint32)(pos + (uint64)n * 4)`: the product was formed correctly in 64 bits
// and then truncated. ESBMC's witness is codeSize = 8192, pos = 5,
// n = 3221234185 -- true end 12884936745, truncated 34857, which is 26665 bytes
// past the end of the method. One bad count therefore moved every case label of
// that switch, not one of them. The count is now checked against the code
// before the base exists, and a switch whose labels do not fit is refused the
// same way every other operand that runs off the end is.
TEST(CILLifterSwitchRegression, CaseCountThatCannotFitRefusesTheInstruction) {
    std::vector<uint8_t> code;
    u8(code, 0x45);              // switch
    u32(code, 3221234185u);      // n, with no labels following it at all
    auto body = tinyBody(code);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    // decodeInstructions stops at the first refusal, so there is nothing to
    // build a CFG from. Without the fix the switch decoded, contributed a
    // leader, and left a truncated base behind it.
    EXPECT_EQ(0u, cfg.blockCount());
}

// A count of 1 whose single label is present, but that claims one more label
// than the method holds, is the same defect one step away from the boundary.
TEST(CILLifterSwitchRegression, CaseCountOneBeyondTheCodeRefusesTheInstruction) {
    std::vector<uint8_t> code;
    u8(code, 0x45);
    u32(code, 2);                // two labels declared
    u32(code, 0);                // only one supplied
    auto body = tinyBody(code);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_EQ(0u, cfg.blockCount());
}

// A switch whose labels do fit must still decode and still resolve its targets.
TEST(CILLifterSwitchRegression, WellFormedSwitchStillDecodes) {
    std::vector<uint8_t> code;
    u8(code, 0x45);
    u32(code, 2);
    u32(code, 0);                // case 0 -> afterSwitch + 0 = 13
    u32(code, 0);                // case 1 -> afterSwitch + 0 = 13
    u8(code, 0x2A);              // ret at offset 13
    auto body = tinyBody(code);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    EXPECT_EQ(code.size(), hdr.codeSize);
    EXPECT_TRUE(hasBlockOperand(cfg));
}

// ─── cil_lifter.cpp:779 — the protected region's end wrapped in 32 bits ──────

// A fat clause whose try region ends past 2^32.
//
// `eh.endOffset = clause.tryOffset + clause.tryLength` was a uint32 addition
// stored in a uint32 field whose start offset was not wrapped. ESBMC's witness
// is tryOffset = 310550527 with tryLength = 3984416769, whose true end is
// exactly 2^32 and which stored endOffset = 0 -- a region ending 310550527
// bytes before it began, and `end - start` of 3984416769 for anything that
// subtracts them.
TEST(CILLifterEhRegionRegression, TryRegionThatWrapsIsNotRecorded) {
    std::vector<uint8_t> sect;
    u8(sect, 0x41);              // Kind: EH | FatFormat
    u8(sect, 4 + 24);            // DataSize = header + one fat clause
    u8(sect, 0);
    u8(sect, 0);
    fatClause(sect, /*kind=*/0, /*tryOffset=*/310550527u,
              /*tryLength=*/3984416769u, /*handlerOffset=*/0,
              /*handlerLength=*/1, /*classToken=*/0);

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    ASSERT_EQ(1u, hdr.exceptionClauses.size());  // the clause is read...
    EXPECT_TRUE(cfg.handlers().empty());         // ...and refused as a region.
}

// The spec's own witness: tryOffset = 0xFFFFFFFF with tryLength = 2 wrapped to
// endOffset = 1.
TEST(CILLifterEhRegionRegression, TryOffsetAtUint32MaxIsNotRecorded) {
    std::vector<uint8_t> sect;
    u8(sect, 0x41);
    u8(sect, 4 + 24);
    u8(sect, 0);
    u8(sect, 0);
    fatClause(sect, 0, 0xFFFFFFFFu, 2u, 0u, 1u, 0u);

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    ASSERT_EQ(1u, hdr.exceptionClauses.size());
    EXPECT_TRUE(cfg.handlers().empty());
}

// A region that does fit must still be recorded, with the end the format says.
TEST(CILLifterEhRegionRegression, WellFormedTryRegionIsRecorded) {
    std::vector<uint8_t> sect;
    u8(sect, 0x41);
    u8(sect, 4 + 24);
    u8(sect, 0);
    u8(sect, 0);
    // Code is 4 bytes, so [0, 2) protected with the handler at 2 fits exactly.
    fatClause(sect, /*kind=*/2 /*finally*/, /*tryOffset=*/0, /*tryLength=*/2,
              /*handlerOffset=*/2, /*handlerLength=*/2, /*classToken=*/0);

    auto body = fatBodyWithSections({0x00, 0x00, 0x00, 0x2A}, sect);

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);

    ASSERT_EQ(1u, cfg.handlers().size());
    EXPECT_EQ(0u, cfg.handlers()[0].startOffset);
    EXPECT_EQ(2u, cfg.handlers()[0].endOffset);
    EXPECT_TRUE(cfg.handlers()[0].isFinally);
}
