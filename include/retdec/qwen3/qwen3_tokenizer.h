/**
 * @file include/retdec/qwen3/qwen3_tokenizer.h
 * @brief Qwen3 BPE tokenizer — tiktoken-compatible implementation.
 *
 * ## Token format
 *
 * Qwen3 uses a byte-level BPE tokenizer derived from tiktoken's `cl100k_base`
 * extended with Qwen-specific special tokens.  The vocabulary is stored in
 * `vocab.json` (a map of base64-encoded UTF-8 bytes → integer token id) or
 * in `tokenizer.json` (Hugging Face format).
 *
 * ## Loading
 *
 *   Qwen3Tokenizer tok;
 *   tok.loadVocabJson("path/to/vocab.json");
 *   // — or —
 *   tok.loadTokenizerJson("path/to/tokenizer.json");
 *
 * ## Encoding
 *
 *   auto ids = tok.encode("Hello, world!");
 *   // ids → {9906, 11, 1917, 0}  (example values)
 *
 * ## Decoding
 *
 *   std::string text = tok.decode({9906, 11, 1917});
 *
 * ## Chat template
 *
 *   tok.encodeChat({{"user","What is main()?"}, {"assistant",""}}, ids);
 *
 * ## Special tokens
 *
 *   tok.addSpecialToken("<|im_start|>", 151644);
 *   tok.addSpecialToken("<|im_end|>",   151645);
 */

#ifndef RETDEC_QWEN3_TOKENIZER_H
#define RETDEC_QWEN3_TOKENIZER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace retdec::qwen3 {

class Qwen3Weights;

// ─── Token types ──────────────────────────────────────────────────────────────

using TokenId  = uint32_t;
using TokenIds = std::vector<TokenId>;

/// Chat role — system | user | assistant
enum class ChatRole { System, User, Assistant };

struct ChatMessage {
    ChatRole    role;
    std::string content;
};

// ─── BpePair ─────────────────────────────────────────────────────────────────

struct BpePair {
    uint32_t first;
    uint32_t second;
    bool operator==(const BpePair& o) const noexcept {
        return first == o.first && second == o.second;
    }
};

struct BpePairHash {
    std::size_t operator()(const BpePair& p) const noexcept {
        return std::hash<uint64_t>{}(
            (static_cast<uint64_t>(p.first) << 32) | p.second);
    }
};

// ─── Qwen3Tokenizer ───────────────────────────────────────────────────────────

class Qwen3Tokenizer {
public:
    Qwen3Tokenizer();
    ~Qwen3Tokenizer() = default;

    // ── Loading ───────────────────────────────────────────────────────────────

    /**
     * @brief Load from tiktoken-style `vocab.json`.
     *
     * Format: `{ "<base64-bytes>": <token_id>, ... }`
     * Where keys are base64-encoded byte sequences.
     *
     * @param path  Path to `vocab.json`.
     * @return true on success.
     */
    bool loadVocabJson(const std::string& path);

    /**
     * @brief Load from Hugging Face `tokenizer.json`.
     *
     * Parses the `model.vocab` and `model.merges` fields.
     *
     * @param path  Path to `tokenizer.json`.
     * @return true on success.
     */
    bool loadTokenizerJson(const std::string& path);

    /**
     * @brief Load from a file — auto-detects vocab.json vs tokenizer.json.
     */
    bool loadFromFile(const std::string& path);

    /**
     * @brief Load BPE vocabulary and merges from GGUF metadata (tokenizer.ggml.*).
     *
     * Replaces any existing vocabulary. Used by Qwen3Pipeline when opening a .gguf file.
     */
    bool loadFromGguf(const Qwen3Weights& w);

    /**
     * @brief Programmatically add vocabulary entries (for testing / embedding).
     *
     * @param token Raw byte string for this token.
     * @param id    Token ID to assign.
     */
    void addToken(const std::string& token, TokenId id);

    /**
     * @brief Overload: add by (id, token, isSpecial) — used when loading from GGUF.
     */
    void addToken(TokenId id, const std::string& token, bool isSpecial);

    /**
     * @brief Add a BPE merge rule: (first, second) → merged token.
     *
     * @param first  TokenId of the left symbol.
     * @param second TokenId of the right symbol.
     * @param merged TokenId of the merged result.
     * @param rank   Priority rank (lower = applied first).
     */
    void addMerge(TokenId first, TokenId second, TokenId merged, uint32_t rank);

    /**
     * @brief String-pair merge: looks up IDs for piece1 and piece2,
     *        then looks up the merged form (piece1 + piece2) to find the result ID.
     *
     * Silently ignored if any token is not found in the vocabulary.
     * Used when loading merge pairs from GGUF metadata.
     *
     * @param piece1  Left merge piece (raw string, not base64).
     * @param piece2  Right merge piece.
     * @param rank    Priority rank.
     */
    void addMerge(const std::string& piece1, const std::string& piece2,
                  uint32_t rank = 0);

    /**
     * @brief Register a special token (bypasses BPE splitting).
     *
     * @param text Special token string, e.g. "<|im_start|>".
     * @param id   Token ID.
     */
    void addSpecialToken(const std::string& text, TokenId id);

    // ── Core encode / decode ─────────────────────────────────────────────────

    /**
     * @brief Encode a UTF-8 string to token IDs.
     *
     * Special tokens are recognized verbatim (not split by BPE).
     * The text is first matched against special tokens, then split
     * into words by the pre-tokenization regex, and finally each
     * word is BPE-encoded.
     */
    TokenIds encode(std::string_view text) const;

    /**
     * @brief Encode with optional BOS/EOS wrapping.
     */
    TokenIds encode(std::string_view text,
                    bool addBos, bool addEos = false) const;

    /**
     * @brief Decode token IDs back to a UTF-8 string.
     *
     * Unknown token IDs are substituted with the Unicode replacement
     * character U+FFFD.
     */
    std::string decode(const TokenIds& ids) const;

    /**
     * @brief Decode a single token to its byte representation.
     */
    std::string decodeToken(TokenId id) const;

    // ── Chat template ─────────────────────────────────────────────────────────

    /**
     * @brief Encode a multi-turn chat conversation using the Qwen3 ChatML
     *        template:
     *
     *        <|im_start|>role\ncontent<|im_end|>\n
     *
     * @param messages  Ordered list of chat messages.
     * @param out       Appended token IDs.
     * @param enableThinking  If true prepend <|think|> after the last
     *                        assistant turn (for reasoning models).
     */
    void encodeChat(const std::vector<ChatMessage>& messages,
                    TokenIds& out,
                    bool enableThinking = false) const;

    // ── Vocabulary queries ────────────────────────────────────────────────────

    std::size_t vocabSize()       const { return tokenToId_.size(); }
    std::size_t mergeCount()      const { return merges_.size(); }
    bool        isLoaded()        const { return !tokenToId_.empty(); }

    std::optional<TokenId>   tokenToId(const std::string& token) const;
    std::optional<std::string> idToToken(TokenId id)             const;
    bool isSpecialToken(TokenId id)                               const;

    // ── Well-known token IDs ──────────────────────────────────────────────────

    static constexpr TokenId kEosId        = 151643; ///< <|endoftext|>
    static constexpr TokenId kImStartId   = 151644; ///< <|im_start|>
    static constexpr TokenId kImEndId     = 151645; ///< <|im_end|>
    static constexpr TokenId kThinkStart  = 151646; ///< <|think|>
    static constexpr TokenId kThinkEnd    = 151647; ///< </|think|>

    // Aliases used by model code
    static constexpr TokenId EOS_TOKEN_ID   = kEosId;
    static constexpr TokenId IM_END_TOKEN_ID = kImEndId;

    // ── Error state ───────────────────────────────────────────────────────────

    const std::string& lastError() const { return lastError_; }

private:
    // BPE encoding of a single pre-tokenized word (as a byte sequence)
    TokenIds bpeEncode(const std::string& word) const;

    // Split text on special-token boundaries
    void splitOnSpecials(std::string_view text,
                         std::vector<std::pair<std::string_view, bool>>& out) const;

    // Pre-tokenization: split into words by Qwen3's regex pattern
    std::vector<std::string> preTokenize(std::string_view text) const;

    // Decode raw bytes, replacing unmappable IDs with U+FFFD
    std::string bytesToUtf8(const std::string& bytes) const;

    // ── Data ─────────────────────────────────────────────────────────────────

    std::unordered_map<std::string, TokenId> tokenToId_;
    std::unordered_map<TokenId, std::string> idToToken_;
    std::unordered_map<BpePair, uint32_t, BpePairHash> mergeRank_;
    std::unordered_map<BpePair, TokenId,  BpePairHash> mergeResult_;
    std::unordered_map<std::string, TokenId> specials_;
    std::unordered_map<TokenId, std::string> specialsById_;

    // Sorted list of special-token strings (longest first for greedy matching)
    std::vector<std::string> sortedSpecials_;

    // Merge rules in rank order (for BPE iteration)
    struct MergeRule {
        uint32_t rank;
        BpePair  pair;
        TokenId  result;
    };
    std::vector<MergeRule> merges_;

    mutable std::string lastError_;
};

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_TOKENIZER_H
