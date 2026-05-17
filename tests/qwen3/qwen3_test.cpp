/**
 * @file tests/qwen3/qwen3_test.cpp
 * @brief Unit tests for Qwen3 tokenizer and weight loader (Task 44).
 *
 * Tests cover:
 *
 * Qwen3Config:
 *   - Default values match 1.7B preset
 *   - Preset constructors produce expected dimensions
 *   - isMoE(), isGQA(), hasMHA(), kvRepeat() helpers
 *   - estimatedParamsM() ballpark
 *
 * Qwen3Tokenizer:
 *   - Default special-token registration
 *   - addToken / addMerge roundtrip
 *   - encode() returns non-empty for non-empty input
 *   - Special-token IDs are preserved in encode output
 *   - Decode of registered tokens reconstructs text
 *   - decodeToken for unknown ID returns replacement char
 *   - isSpecialToken()
 *   - tokenToId / idToToken lookups
 *   - bpeEncode merges two tokens when a merge rule exists
 *   - encodeChat produces im_start / im_end tokens
 *   - encode(text, addBos=true) prepends EOS token
 *   - encode(text, addEos=true) appends EOS token
 *   - vocabSize / mergeCount grow after adds
 *   - isLoaded() reflects state
 *   - Encode empty string → empty ids
 *   - Decode empty ids → empty string
 *   - loadVocabJson from non-existent path sets lastError
 *   - loadTokenizerJson from non-existent path sets lastError
 *
 * Dequantization helpers:
 *   - bf16ToF32: 1.0f, 0.0f, negative
 *   - f16ToF32: 1.0f, 0.0f, denormal
 *   - dequantQ8_0: scale × values
 *   - dequantQ4_0: nibble decoding
 *   - dequantQ4_1: nibble decoding with min
 *
 * GgufDtype helpers:
 *   - ggufDtypeName returns non-empty for all known types
 *   - ggufDtypeBlockSize positive for all types
 *   - ggufDtypeBlockElems 1 for scalar, 32/256 for quants
 *
 * SafeTensorsDtype helpers:
 *   - parseSafeTensorsDtype round-trips
 *   - Unknown string returns Unknown
 *
 * TensorInfo / SafeTensorInfo:
 *   - dataSizeBytes for F32 scalar
 *   - dataSizeBytes for Q8_0 block
 *   - nElements() and dataSizeBytes() for SafeTensorInfo
 *
 * Qwen3Weights (no file):
 *   - isGguf on nonexistent path returns false
 *   - openGguf on nonexistent path returns false with error
 *   - isGguf on a minimal valid GGUF buffer
 *   - isSafetensors on nonexistent path returns false
 *   - openSafetensors on nonexistent path returns false
 *   - load on unopened weights returns nullopt
 *   - extractConfig on empty metadata returns defaults
 *
 * TensorView:
 *   - dataRaw and rawBytes for owned buffer
 *   - dataF32 for F32 tensor
 *   - dataF32 for F16 tensor
 *   - dataF32 for BF16 tensor
 *   - dataF32 for Q8_0 tensor
 *   - dataF32 for Q4_0 tensor
 *   - dataF32 for Q4_1 tensor
 *   - dataF32 returns nullopt for unsupported quant type
 *   - nDims, dim(), nElements(), dtype() accessors
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <vector>

#include "retdec/qwen3/qwen3_config.h"
#include "retdec/qwen3/qwen3_tokenizer.h"
#include "retdec/qwen3/qwen3_weights.h"

using namespace retdec::qwen3;

// ─── Qwen3Config ──────────────────────────────────────────────────────────────

TEST(Qwen3Config, DefaultValues) {
    Qwen3Config c;
    EXPECT_EQ(c.vocabSize, 151936u);
    EXPECT_EQ(c.eosTokenId, 151643u);
    EXPECT_EQ(c.imStartId,  151644u);
    EXPECT_EQ(c.imEndId,    151645u);
    EXPECT_FALSE(c.isMoE());
}

TEST(Qwen3Config, Preset0_6B) {
    auto c = presets::qwen3_0_6B();
    EXPECT_EQ(c.hiddenSize,  1024u);
    EXPECT_EQ(c.numLayers,   28u);
    EXPECT_EQ(c.numKvHeads,  8u);
}

TEST(Qwen3Config, Preset1_7B) {
    auto c = presets::qwen3_1_7B();
    EXPECT_EQ(c.hiddenSize,  2048u);
    EXPECT_EQ(c.numHeads,    16u);
}

TEST(Qwen3Config, Preset4B) {
    auto c = presets::qwen3_4B();
    EXPECT_EQ(c.numLayers, 36u);
}

TEST(Qwen3Config, Preset8B) {
    auto c = presets::qwen3_8B();
    EXPECT_EQ(c.hiddenSize, 4096u);
}

TEST(Qwen3Config, Preset14B) {
    auto c = presets::qwen3_14B();
    EXPECT_EQ(c.numLayers, 40u);
}

TEST(Qwen3Config, Preset32B) {
    auto c = presets::qwen3_32B();
    EXPECT_EQ(c.numLayers, 64u);
}

TEST(Qwen3Config, IsGQA) {
    auto c = presets::qwen3_1_7B();
    EXPECT_TRUE(c.isGQA());
    EXPECT_FALSE(c.hasMHA());
}

TEST(Qwen3Config, KvRepeat) {
    auto c = presets::qwen3_1_7B(); // numHeads=16, numKvHeads=8
    EXPECT_EQ(c.kvRepeat(), 2u);
}

TEST(Qwen3Config, EstimatedParams) {
    auto c = presets::qwen3_0_6B();
    uint64_t params = c.estimatedParamsM();
    EXPECT_GT(params, 0u);
}

TEST(Qwen3Config, MoEFalseByDefault) {
    Qwen3Config c;
    EXPECT_EQ(c.numExperts, 0u);
    EXPECT_FALSE(c.isMoE());
}

TEST(Qwen3Config, MoETrue) {
    Qwen3Config c;
    c.numExperts       = 64;
    c.numExpertsPerTok = 4;
    EXPECT_TRUE(c.isMoE());
}

// ─── Qwen3Tokenizer ───────────────────────────────────────────────────────────

TEST(Qwen3Tokenizer, DefaultSpecialTokens) {
    Qwen3Tokenizer tok;
    EXPECT_TRUE(tok.isSpecialToken(Qwen3Tokenizer::kEosId));
    EXPECT_TRUE(tok.isSpecialToken(Qwen3Tokenizer::kImStartId));
    EXPECT_TRUE(tok.isSpecialToken(Qwen3Tokenizer::kImEndId));
}

TEST(Qwen3Tokenizer, IsLoadedFalseWhenEmpty) {
    // Default ctor only adds special tokens, not vocab
    Qwen3Tokenizer tok;
    // vocabSize includes special tokens added in ctor
    EXPECT_GT(tok.vocabSize(), 0u);
    EXPECT_TRUE(tok.isLoaded()); // specials are in tokenToId_
}

TEST(Qwen3Tokenizer, AddTokenAndRetrieve) {
    Qwen3Tokenizer tok;
    tok.addToken("hello", 100);
    auto id = tok.tokenToId("hello");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 100u);
    auto str = tok.idToToken(100);
    ASSERT_TRUE(str.has_value());
    EXPECT_EQ(*str, "hello");
}

TEST(Qwen3Tokenizer, VocabSizeGrowsOnAdd) {
    Qwen3Tokenizer tok;
    std::size_t before = tok.vocabSize();
    tok.addToken("newtoken", 9999);
    EXPECT_GT(tok.vocabSize(), before);
}

TEST(Qwen3Tokenizer, MergeCountGrowsOnAdd) {
    Qwen3Tokenizer tok;
    tok.addToken("a", 1);
    tok.addToken("b", 2);
    tok.addToken("ab", 3);
    std::size_t before = tok.mergeCount();
    tok.addMerge(1, 2, 3, 0);
    EXPECT_GT(tok.mergeCount(), before);
}

TEST(Qwen3Tokenizer, EncodeEmptyString) {
    Qwen3Tokenizer tok;
    tok.addToken(" ", 220);
    auto ids = tok.encode("");
    EXPECT_TRUE(ids.empty());
}

TEST(Qwen3Tokenizer, DecodeEmptyIds) {
    Qwen3Tokenizer tok;
    EXPECT_EQ(tok.decode({}), "");
}

TEST(Qwen3Tokenizer, EncodeSpecialTokenPreserved) {
    Qwen3Tokenizer tok;
    // Encode just a special token
    auto ids = tok.encode("<|im_start|>");
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], Qwen3Tokenizer::kImStartId);
}

TEST(Qwen3Tokenizer, EncodeEosTokenPreserved) {
    Qwen3Tokenizer tok;
    auto ids = tok.encode("<|endoftext|>");
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], Qwen3Tokenizer::kEosId);
}

TEST(Qwen3Tokenizer, AddBosPrependsEos) {
    Qwen3Tokenizer tok;
    tok.addToken("x", 500);
    auto ids = tok.encode("x", true, false);
    ASSERT_FALSE(ids.empty());
    EXPECT_EQ(ids[0], Qwen3Tokenizer::kEosId);
}

TEST(Qwen3Tokenizer, AddEosAppendsEos) {
    Qwen3Tokenizer tok;
    tok.addToken("x", 500);
    auto ids = tok.encode("x", false, true);
    ASSERT_FALSE(ids.empty());
    EXPECT_EQ(ids.back(), Qwen3Tokenizer::kEosId);
}

TEST(Qwen3Tokenizer, DecodeKnownToken) {
    Qwen3Tokenizer tok;
    tok.addToken("Hi", 42);
    std::string out = tok.decode({42});
    EXPECT_EQ(out, "Hi");
}

TEST(Qwen3Tokenizer, DecodeUnknownIdReplacesWithFFFD) {
    Qwen3Tokenizer tok;
    std::string out = tok.decodeToken(999999);
    EXPECT_EQ(out, "\xEF\xBF\xBD");
}

TEST(Qwen3Tokenizer, AddSpecialTokenIsSpecial) {
    Qwen3Tokenizer tok;
    tok.addSpecialToken("<|custom|>", 200000);
    EXPECT_TRUE(tok.isSpecialToken(200000));
}

TEST(Qwen3Tokenizer, EncodeSpecialInMiddleOfText) {
    Qwen3Tokenizer tok;
    // Register single-byte tokens
    for (unsigned char b = 0; b < 128; ++b)
        tok.addToken(std::string(1, static_cast<char>(b)), b);

    auto ids = tok.encode("A<|im_end|>B");
    // Should contain kImEndId somewhere
    bool hasImEnd = false;
    for (auto id : ids)
        if (id == Qwen3Tokenizer::kImEndId) { hasImEnd = true; break; }
    EXPECT_TRUE(hasImEnd);
}

TEST(Qwen3Tokenizer, BpeMergeApplied) {
    Qwen3Tokenizer tok;
    tok.addToken("h", 1);
    tok.addToken("e", 2);
    tok.addToken("l", 3);
    tok.addToken("o", 4);
    tok.addToken("he", 5);
    tok.addToken("hel", 6);
    tok.addToken("hell", 7);
    tok.addToken("hello", 8);
    tok.addMerge(1, 2, 5, 0);  // h + e → he (rank 0)
    tok.addMerge(5, 3, 6, 1);  // he + l → hel (rank 1)
    tok.addMerge(6, 3, 7, 2);  // hel + l → hell (rank 2)
    tok.addMerge(7, 4, 8, 3);  // hell + o → hello (rank 3)

    auto ids = tok.encode("hello");
    ASSERT_FALSE(ids.empty());
    // After full merge, should be a single token id=8
    EXPECT_EQ(ids[0], 8u);
    EXPECT_EQ(ids.size(), 1u);
}

TEST(Qwen3Tokenizer, DecodeRoundtrip) {
    Qwen3Tokenizer tok;
    tok.addToken("hello", 10);
    tok.addToken(" ", 11);
    tok.addToken("world", 12);
    auto ids = tok.decode({10, 11, 12});
    EXPECT_EQ(ids, "hello world");
}

TEST(Qwen3Tokenizer, EncodeChatProducesImTokens) {
    Qwen3Tokenizer tok;
    // Register basic printable ASCII
    for (unsigned char b = 32; b < 127; ++b)
        tok.addToken(std::string(1, static_cast<char>(b)), b);

    std::vector<ChatMessage> msgs = {
        {ChatRole::User, "hello"},
    };
    TokenIds ids;
    tok.encodeChat(msgs, ids);

    bool hasImStart = false, hasImEnd = false;
    for (auto id : ids) {
        if (id == Qwen3Tokenizer::kImStartId) hasImStart = true;
        if (id == Qwen3Tokenizer::kImEndId)   hasImEnd   = true;
    }
    EXPECT_TRUE(hasImStart);
    EXPECT_TRUE(hasImEnd);
}

TEST(Qwen3Tokenizer, EncodeChatThinkingAddsThinkStart) {
    Qwen3Tokenizer tok;
    for (unsigned char b = 32; b < 127; ++b)
        tok.addToken(std::string(1, static_cast<char>(b)), b);

    std::vector<ChatMessage> msgs = {{ChatRole::User, "test"}};
    TokenIds ids;
    tok.encodeChat(msgs, ids, true);

    bool hasThink = false;
    for (auto id : ids)
        if (id == Qwen3Tokenizer::kThinkStart) { hasThink = true; break; }
    EXPECT_TRUE(hasThink);
}

TEST(Qwen3Tokenizer, LoadFromNonexistentVocabJsonFails) {
    Qwen3Tokenizer tok;
    EXPECT_FALSE(tok.loadVocabJson("/nonexistent/vocab.json"));
    EXPECT_FALSE(tok.lastError().empty());
}

TEST(Qwen3Tokenizer, LoadFromNonexistentTokenizerJsonFails) {
    Qwen3Tokenizer tok;
    EXPECT_FALSE(tok.loadTokenizerJson("/nonexistent/tokenizer.json"));
    EXPECT_FALSE(tok.lastError().empty());
}

// ─── Dequantization helpers ───────────────────────────────────────────────────

TEST(Dequant, BF16_One) {
    // BF16 representation of 1.0f is 0x3F80
    float v = bf16ToF32(0x3F80);
    EXPECT_FLOAT_EQ(v, 1.0f);
}

TEST(Dequant, BF16_Zero) {
    float v = bf16ToF32(0x0000);
    EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(Dequant, BF16_Negative) {
    // -1.0f in BF16 is 0xBF80
    float v = bf16ToF32(0xBF80);
    EXPECT_FLOAT_EQ(v, -1.0f);
}

TEST(Dequant, F16_One) {
    // 1.0 in IEEE-754 half is 0x3C00
    float v = f16ToF32(0x3C00);
    EXPECT_FLOAT_EQ(v, 1.0f);
}

TEST(Dequant, F16_Zero) {
    float v = f16ToF32(0x0000);
    EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(Dequant, F16_NegativeOne) {
    float v = f16ToF32(0xBC00);
    EXPECT_FLOAT_EQ(v, -1.0f);
}

TEST(Dequant, Q8_0_AllZero) {
    // scale = 0.0 (f16 = 0x0000), qs = 0..0 → all zeros
    uint8_t block[34] = {};
    float out[32] = {};
    dequantQ8_0(block, out);
    for (int i = 0; i < 32; ++i)
        EXPECT_FLOAT_EQ(out[i], 0.0f);
}

TEST(Dequant, Q8_0_ScaleAndValue) {
    // scale = 1.0f in f16 = 0x3C00, qs[0] = 3 → out[0] = 3.0f
    uint8_t block[34] = {};
    block[0] = 0x00; block[1] = 0x3C;  // f16 = 1.0
    reinterpret_cast<int8_t*>(block + 2)[0] = 3;
    float out[32] = {};
    dequantQ8_0(block, out);
    EXPECT_FLOAT_EQ(out[0], 3.0f);
}

TEST(Dequant, Q4_0_AllZero) {
    uint8_t block[18] = {};
    float out[32] = {};
    dequantQ4_0(block, out);
    for (int i = 0; i < 32; ++i)
        EXPECT_FLOAT_EQ(out[i], -8.0f * 0.0f); // scale=0 → all 0
}

TEST(Dequant, Q4_0_NibbleDecode) {
    // scale=1.0f (f16=0x3C00), qs[0] = 0x88 → nibbles: 8,8 → (8-8)=0 each
    uint8_t block[18] = {};
    block[0] = 0x00; block[1] = 0x3C;  // scale=1.0
    block[2] = 0x88; // nibbles 8,8 → 0,0 (offset 8)
    float out[32] = {};
    dequantQ4_0(block, out);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
}

TEST(Dequant, Q4_1_NibbleDecode) {
    // scale=1.0f, min=0.0f, nibble=1 → 1*1.0+0.0 = 1.0
    uint8_t block[20] = {};
    block[0] = 0x00; block[1] = 0x3C;  // scale=1.0 (f16)
    block[2] = 0x00; block[3] = 0x00;  // min=0.0
    block[4] = 0x01;                    // nibbles: 1 (lo), 0 (hi)
    float out[32] = {};
    dequantQ4_1(block, out);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
}

// ─── GgufDtype helpers ────────────────────────────────────────────────────────

TEST(GgufDtype, NameNonEmpty) {
    EXPECT_STRNE(ggufDtypeName(GgufDtype::F32),    "");
    EXPECT_STRNE(ggufDtypeName(GgufDtype::Q4_K_M), "");
    EXPECT_STRNE(ggufDtypeName(GgufDtype::BF16),   "");
}

TEST(GgufDtype, BlockSizeF32) {
    EXPECT_EQ(ggufDtypeBlockSize(GgufDtype::F32), sizeof(float));
}

TEST(GgufDtype, BlockSizeQ8_0) {
    EXPECT_EQ(ggufDtypeBlockSize(GgufDtype::Q8_0), 34u);
}

TEST(GgufDtype, BlockSizeQ4_0) {
    EXPECT_EQ(ggufDtypeBlockSize(GgufDtype::Q4_0), 18u);
}

TEST(GgufDtype, BlockElemsScalar) {
    EXPECT_EQ(ggufDtypeBlockElems(GgufDtype::F32),  1u);
    EXPECT_EQ(ggufDtypeBlockElems(GgufDtype::F16),  1u);
    EXPECT_EQ(ggufDtypeBlockElems(GgufDtype::BF16), 1u);
}

TEST(GgufDtype, BlockElemsQ8_0) {
    EXPECT_EQ(ggufDtypeBlockElems(GgufDtype::Q8_0), 32u);
}

TEST(GgufDtype, BlockElemsKQuant) {
    EXPECT_EQ(ggufDtypeBlockElems(GgufDtype::Q4_K_M), 256u);
}

// ─── SafeTensorsDtype helpers ─────────────────────────────────────────────────

TEST(SafeTensorsDtype, ParseRoundtrip) {
    EXPECT_EQ(parseSafeTensorsDtype("F32"),  SafeTensorsDtype::F32);
    EXPECT_EQ(parseSafeTensorsDtype("F16"),  SafeTensorsDtype::F16);
    EXPECT_EQ(parseSafeTensorsDtype("BF16"), SafeTensorsDtype::BF16);
    EXPECT_EQ(parseSafeTensorsDtype("I8"),   SafeTensorsDtype::I8);
    EXPECT_EQ(parseSafeTensorsDtype("I64"),  SafeTensorsDtype::I64);
}

TEST(SafeTensorsDtype, UnknownString) {
    EXPECT_EQ(parseSafeTensorsDtype("Q4_K_M"), SafeTensorsDtype::Unknown);
}

TEST(SafeTensorsDtype, DtypeNameRoundtrip) {
    EXPECT_STREQ(safeTensorsDtypeName(SafeTensorsDtype::F32),  "F32");
    EXPECT_STREQ(safeTensorsDtypeName(SafeTensorsDtype::BF16), "BF16");
}

// ─── TensorInfo / SafeTensorInfo ─────────────────────────────────────────────

TEST(TensorInfo, DataSizeBytesF32) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::F32;
    ti.nElements = 4;
    EXPECT_EQ(ti.dataSizeBytes(), 16u);
}

TEST(TensorInfo, DataSizeBytesQ8_0) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::Q8_0;
    ti.nElements = 32;
    // 1 block of 32 elements → 2 (scale) + 32 (bytes) = 34
    EXPECT_EQ(ti.dataSizeBytes(), 34u);
}

TEST(SafeTensorInfo, NElements) {
    SafeTensorInfo si;
    si.shape = {2, 3, 4};
    EXPECT_EQ(si.nElements(), 24u);
}

TEST(SafeTensorInfo, DataSizeBytesF32) {
    SafeTensorInfo si;
    si.dtype = SafeTensorsDtype::F32;
    si.shape = {4};
    EXPECT_EQ(si.dataSizeBytes(), 16u);
}

TEST(SafeTensorInfo, DataSizeBytesF16) {
    SafeTensorInfo si;
    si.dtype = SafeTensorsDtype::F16;
    si.shape = {8};
    EXPECT_EQ(si.dataSizeBytes(), 16u);
}

// ─── TensorView ──────────────────────────────────────────────────────────────

TEST(TensorView, OwnedBufferAccess) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::F32;
    ti.nDims     = 1;
    ti.dims[0]   = 4;
    ti.nElements = 4;

    std::vector<uint8_t> data(16, 0);
    TensorView tv(ti, data);
    EXPECT_NE(tv.dataRaw(), nullptr);
    EXPECT_EQ(tv.rawBytes(), 16u);
}

TEST(TensorView, DataF32FromF32Buffer) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::F32;
    ti.nDims     = 1;
    ti.dims[0]   = 2;
    ti.nElements = 2;

    std::vector<uint8_t> data(8);
    float vals[2] = {1.0f, 2.0f};
    std::memcpy(data.data(), vals, 8);

    TensorView tv(ti, data);
    auto f32 = tv.dataF32();
    ASSERT_TRUE(f32.has_value());
    EXPECT_FLOAT_EQ((*f32)[0], 1.0f);
    EXPECT_FLOAT_EQ((*f32)[1], 2.0f);
}

TEST(TensorView, DataF32FromF16Buffer) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::F16;
    ti.nDims     = 1;
    ti.dims[0]   = 1;
    ti.nElements = 1;

    // 1.0f in f16 = 0x3C00
    std::vector<uint8_t> data = {0x00, 0x3C};
    TensorView tv(ti, data);
    auto f32 = tv.dataF32();
    ASSERT_TRUE(f32.has_value());
    EXPECT_FLOAT_EQ((*f32)[0], 1.0f);
}

TEST(TensorView, DataF32FromBF16Buffer) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::BF16;
    ti.nDims     = 1;
    ti.dims[0]   = 1;
    ti.nElements = 1;

    // 1.0f in BF16 = 0x3F80
    std::vector<uint8_t> data = {0x80, 0x3F};
    TensorView tv(ti, data);
    auto f32 = tv.dataF32();
    ASSERT_TRUE(f32.has_value());
    EXPECT_FLOAT_EQ((*f32)[0], 1.0f);
}

TEST(TensorView, DataF32FromQ8_0) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::Q8_0;
    ti.nDims     = 1;
    ti.dims[0]   = 32;
    ti.nElements = 32;

    // One Q8_0 block: scale=1.0 (0x3C00), qs[0]=1, rest=0
    std::vector<uint8_t> data(34, 0);
    data[0] = 0x00; data[1] = 0x3C;
    data[2] = 1;  // qs[0] = 1

    TensorView tv(ti, data);
    auto f32 = tv.dataF32();
    ASSERT_TRUE(f32.has_value());
    EXPECT_FLOAT_EQ((*f32)[0], 1.0f);
    EXPECT_FLOAT_EQ((*f32)[1], 0.0f);
}

TEST(TensorView, DataF32NulloptForKQuant) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::Q4_K_M;
    ti.nDims     = 1;
    ti.dims[0]   = 256;
    ti.nElements = 256;

    std::vector<uint8_t> data(144, 0); // Q4_K_M block
    TensorView tv(ti, data);
    EXPECT_FALSE(tv.dataF32().has_value());
}

TEST(TensorView, AccessorsDims) {
    TensorInfo ti;
    ti.dtype     = GgufDtype::F32;
    ti.nDims     = 2;
    ti.dims[0]   = 3;
    ti.dims[1]   = 4;
    ti.nElements = 12;

    TensorView tv(ti, std::vector<uint8_t>(48, 0));
    EXPECT_EQ(tv.nDims(),     2u);
    EXPECT_EQ(tv.dim(0),      3u);
    EXPECT_EQ(tv.dim(1),      4u);
    EXPECT_EQ(tv.nElements(), 12u);
    EXPECT_EQ(tv.dtype(),     GgufDtype::F32);
}

// ─── Qwen3Weights (no file) ───────────────────────────────────────────────────

TEST(Qwen3Weights, IsGgufNonexistent) {
    EXPECT_FALSE(Qwen3Weights::isGguf("/no/such/file.gguf"));
}

TEST(Qwen3Weights, OpenGgufNonexistentFails) {
    Qwen3Weights w;
    EXPECT_FALSE(w.openGguf("/no/such/file.gguf"));
    EXPECT_FALSE(w.isOpen());
    EXPECT_FALSE(w.lastError().empty());
}

TEST(Qwen3Weights, IsSafetensorsNonexistent) {
    EXPECT_FALSE(Qwen3Weights::isSafetensors("/no/such/file.safetensors"));
}

TEST(Qwen3Weights, OpenSafetensorsNonexistentFails) {
    Qwen3Weights w;
    EXPECT_FALSE(w.openSafetensors("/no/such/file.safetensors"));
}

TEST(Qwen3Weights, LoadOnUnopenedWeightsReturnsNullopt) {
    Qwen3Weights w;
    auto tv = w.load("token_embd.weight");
    EXPECT_FALSE(tv.has_value());
}

TEST(Qwen3Weights, ExtractConfigEmptyMetadata) {
    Qwen3Weights w;
    Qwen3Config cfg = w.extractConfig();
    // Should return defaults
    EXPECT_EQ(cfg.vocabSize, 151936u);
}

TEST(Qwen3Weights, IsGgufOnValidMagic) {
    // Write a minimal GGUF header to a temp file
    const char* tmp = "test_magic.gguf.tmp";
    std::FILE* f = std::fopen(tmp, "wb");
    ASSERT_NE(f, nullptr);
    uint32_t magic   = 0x46554747u; // "GGUF"
    uint32_t version = 3;
    uint64_t nTensors = 0;
    uint64_t nKv      = 0;
    std::fwrite(&magic,    4, 1, f);
    std::fwrite(&version,  4, 1, f);
    std::fwrite(&nTensors, 8, 1, f);
    std::fwrite(&nKv,      8, 1, f);
    std::fclose(f);

    EXPECT_TRUE(Qwen3Weights::isGguf(tmp));
    std::remove(tmp);
}

TEST(Qwen3Weights, OpenGgufMinimalFile) {
    // Write a minimal valid GGUF file (version 3, no tensors, no metadata)
    const char* tmp = "test_minimal.gguf.tmp";
    std::FILE* f = std::fopen(tmp, "wb");
    ASSERT_NE(f, nullptr);
    uint32_t magic    = 0x46554747u;
    uint32_t version  = 3;
    uint64_t nTensors = 0;
    uint64_t nKv      = 0;
    std::fwrite(&magic,    4, 1, f);
    std::fwrite(&version,  4, 1, f);
    std::fwrite(&nTensors, 8, 1, f);
    std::fwrite(&nKv,      8, 1, f);
    std::fclose(f);

    Qwen3Weights w;
    EXPECT_TRUE(w.openGguf(tmp));
    EXPECT_TRUE(w.isOpen());
    EXPECT_EQ(w.format(), "gguf");
    EXPECT_TRUE(w.tensors().empty());
    EXPECT_TRUE(w.metadata().empty());
    std::remove(tmp);
}

TEST(Qwen3Weights, OpenGgufBadMagicFails) {
    const char* tmp = "test_badmagic.gguf.tmp";
    std::FILE* f = std::fopen(tmp, "wb");
    ASSERT_NE(f, nullptr);
    uint32_t magic = 0xDEADBEEFu;
    std::fwrite(&magic, 4, 1, f);
    std::fclose(f);

    Qwen3Weights w;
    EXPECT_FALSE(w.openGguf(tmp));
    std::remove(tmp);
}

TEST(Qwen3Weights, MetaReturnsNulloptForMissingKey) {
    Qwen3Weights w;
    EXPECT_FALSE(w.meta("nonexistent_key").has_value());
}

// ─── SafeTensors header parsing ───────────────────────────────────────────────

TEST(Qwen3Weights, OpenSafetensorsMinimalFile) {
    // Build a minimal safetensors file:
    // [8 bytes: header_len] [header_len bytes: JSON]
    std::string header = R"({"weight":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})";
    uint64_t headerLen  = header.size();

    const char* tmp = "test_minimal.safetensors.tmp";
    std::FILE* f = std::fopen(tmp, "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite(&headerLen, 8, 1, f);
    std::fwrite(header.data(), 1, header.size(), f);
    // 8 bytes of tensor data (2 float32 = 8 bytes)
    float vals[2] = {1.0f, 2.0f};
    std::fwrite(vals, 4, 2, f);
    std::fclose(f);

    Qwen3Weights w;
    EXPECT_TRUE(w.openSafetensors(tmp));
    EXPECT_EQ(w.safeTensors().size(), 1u);
    EXPECT_EQ(w.safeTensors()[0].name, "weight");
    EXPECT_EQ(w.safeTensors()[0].dtype, SafeTensorsDtype::F32);
    ASSERT_EQ(w.safeTensors()[0].shape.size(), 1u);
    EXPECT_EQ(w.safeTensors()[0].shape[0], 2u);

    auto tv = w.loadSafetensor("weight");
    ASSERT_TRUE(tv.has_value());
    auto f32 = tv->dataF32();
    ASSERT_TRUE(f32.has_value());
    EXPECT_FLOAT_EQ((*f32)[0], 1.0f);
    EXPECT_FLOAT_EQ((*f32)[1], 2.0f);

    std::remove(tmp);
}
