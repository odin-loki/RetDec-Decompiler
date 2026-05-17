/**
 * @file src/qwen3/qwen3_tokenizer.cpp
 * @brief Qwen3 BPE tokenizer implementation.
 *
 * Implements byte-level BPE compatible with tiktoken's approach:
 *   1. Special-token splitting (greedy, longest first)
 *   2. Pre-tokenization via Qwen3's regex (similar to GPT-4's cl100k_base)
 *   3. Per-word BPE encoding using a priority-queue merge loop
 *   4. Decoding via direct ID → bytes lookup + UTF-8 reconstruction
 *
 * File loading supports:
 *   - `vocab.json`      (base64-encoded token bytes → id)
 *   - `tokenizer.json`  (Hugging Face BPE JSON with vocab + merges arrays)
 */

#include "retdec/qwen3/qwen3_tokenizer.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

// ── Minimal JSON parser (no external dependency) ──────────────────────────────
// We implement just enough to read vocab.json / tokenizer.json:
//   - string values
//   - integer values
//   - flat object iteration
//   - flat array iteration

namespace {

// ── Base64 decoder ────────────────────────────────────────────────────────────

static const int8_t kB64Table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

std::string base64Decode(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = 0;
    for (unsigned char c : in) {
        int v = kB64Table[c];
        if (v < 0) continue; // skip padding / whitespace
        val  = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((val >> bits) & 0xFF);
        }
    }
    return out;
}

// ── Minimal recursive-descent JSON parser ─────────────────────────────────────

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : s_(src), pos_(0) {}

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' ||
               s_[pos_] == '\r' || s_[pos_] == '\t'))
            ++pos_;
    }

    char peek() { skipWs(); return pos_ < s_.size() ? s_[pos_] : 0; }
    char next() { skipWs(); return pos_ < s_.size() ? s_[pos_++] : 0; }

    bool expect(char c) {
        if (next() != c) { ok_ = false; return false; }
        return true;
    }

    std::string parseString() {
        expect('"');
        std::string r;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') break;
            if (c == '\\' && pos_ < s_.size()) {
                char e = s_[pos_++];
                switch (e) {
                case 'n':  r += '\n'; break;
                case 'r':  r += '\r'; break;
                case 't':  r += '\t'; break;
                case '\\': r += '\\'; break;
                case '"':  r += '"';  break;
                case '/':  r += '/';  break;
                case 'b':  r += '\b'; break;
                case 'f':  r += '\f'; break;
                case 'u': {
                    // 4 hex digits → basic Unicode (BMP only)
                    if (pos_ + 4 <= s_.size()) {
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        }
                        // Encode cp as UTF-8
                        if (cp < 0x80) {
                            r += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            r += static_cast<char>(0xC0 | (cp >> 6));
                            r += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            r += static_cast<char>(0xE0 | (cp >> 12));
                            r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            r += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: r += e; break;
                }
            } else {
                r += c;
            }
        }
        return r;
    }

    int64_t parseInt() {
        skipWs();
        bool neg = false;
        if (pos_ < s_.size() && s_[pos_] == '-') { neg = true; ++pos_; }
        int64_t v = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
            v = v * 10 + (s_[pos_++] - '0');
        return neg ? -v : v;
    }

    // Iterate top-level object: calls cb(key, parser&) for each key.
    void parseObject(std::function<void(const std::string&, JsonParser&)> cb) {
        expect('{');
        while (ok_) {
            skipWs();
            if (peek() == '}') { next(); break; }
            std::string key = parseString();
            expect(':');
            cb(key, *this);
            skipWs();
            if (peek() == ',') { next(); continue; }
            if (peek() == '}') { next(); break; }
        }
    }

    // Iterate top-level array: calls cb(parser&) for each element.
    void parseArray(std::function<void(JsonParser&)> cb) {
        expect('[');
        while (ok_) {
            skipWs();
            if (peek() == ']') { next(); break; }
            cb(*this);
            skipWs();
            if (peek() == ',') { next(); continue; }
            if (peek() == ']') { next(); break; }
        }
    }

    // Skip any JSON value
    void skipValue() {
        skipWs();
        char c = peek();
        if      (c == '"')  { parseString(); }
        else if (c == '{')  { expect('{'); int d=1; while(pos_<s_.size()&&d>0){char x=s_[pos_++];if(x=='{')d++;else if(x=='}')d--;} }
        else if (c == '[')  { expect('['); int d=1; while(pos_<s_.size()&&d>0){char x=s_[pos_++];if(x=='[')d++;else if(x==']')d--;} }
        else if (c == 't' || c == 'f' || c == 'n') { while(pos_<s_.size()&&s_[pos_]>='a')pos_++; }
        else                { (void)parseInt(); }
    }

    bool ok_ = true;
    const std::string& s_;
    std::size_t pos_;
};

} // anonymous namespace

namespace retdec::qwen3 {

// ─── Construction ─────────────────────────────────────────────────────────────

Qwen3Tokenizer::Qwen3Tokenizer() {
    // Register Qwen3 special tokens by default so encode() works even without
    // loading a vocab file (useful for testing).
    addSpecialToken("<|endoftext|>", kEosId);
    addSpecialToken("<|im_start|>", kImStartId);
    addSpecialToken("<|im_end|>", kImEndId);
    addSpecialToken("<|think|>",     kThinkStart);
    addSpecialToken("</|think|>",    kThinkEnd);
}

// ─── addToken / addMerge / addSpecialToken ────────────────────────────────────

void Qwen3Tokenizer::addToken(const std::string& token, TokenId id) {
    tokenToId_[token] = id;
    idToToken_[id]    = token;
}

void Qwen3Tokenizer::addToken(TokenId id, const std::string& token, bool isSpecial) {
    addToken(token, id);
    if (isSpecial) addSpecialToken(token, id);
}

void Qwen3Tokenizer::addMerge(TokenId first, TokenId second,
                               TokenId merged, uint32_t rank) {
    BpePair pair{first, second};
    mergeRank_[pair]   = rank;
    mergeResult_[pair] = merged;
    merges_.push_back({rank, pair, merged});
}

void Qwen3Tokenizer::addMerge(const std::string& piece1,
                               const std::string& piece2,
                               uint32_t rank) {
    auto it1 = tokenToId_.find(piece1);
    auto it2 = tokenToId_.find(piece2);
    if (it1 == tokenToId_.end() || it2 == tokenToId_.end()) return;
    auto merged = tokenToId_.find(piece1 + piece2);
    if (merged == tokenToId_.end()) return;
    addMerge(it1->second, it2->second, merged->second, rank);
}

bool Qwen3Tokenizer::loadFromFile(const std::string& path) {
    if (path.size() >= 5) {
        std::string suf = path.substr(path.size() - 5);
        if (suf == ".gguf" || suf == ".GGUF") {
            lastError_ =
                "GGUF vocabulary must be loaded via loadFromGguf(Qwen3Weights), not loadFromFile()";
            return false;
        }
    }
    // Auto-detect: tokenizer.json if name contains "tokenizer", else vocab.json
    if (path.find("tokenizer") != std::string::npos)
        return loadTokenizerJson(path);
    return loadVocabJson(path);
}

bool Qwen3Tokenizer::loadFromGguf(const Qwen3Weights& w) {
    lastError_.clear();
    tokenToId_.clear();
    idToToken_.clear();
    mergeRank_.clear();
    mergeResult_.clear();
    specials_.clear();
    specialsById_.clear();
    sortedSpecials_.clear();
    merges_.clear();

    std::vector<std::string> tokens = w.metaStringArray("tokenizer.ggml.tokens");
    std::vector<std::string> merges = w.metaStringArray("tokenizer.ggml.merges");
    std::vector<float>       scores = w.metaFloatArray("tokenizer.ggml.scores");
    std::vector<int32_t>     ttypes = w.metaInt32Array("tokenizer.ggml.token_type");

    if (tokens.empty()) {
        lastError_ = "GGUF does not contain embedded vocabulary (tokenizer.ggml.tokens)";
        return false;
    }

    for (int id = 0; id < static_cast<int>(tokens.size()); ++id) {
        int tt = (id < static_cast<int>(ttypes.size()))
                     ? ttypes[static_cast<std::size_t>(id)]
                     : 1;
        bool isSpecial = (tt == 3);
        addToken(static_cast<TokenId>(id), tokens[static_cast<std::size_t>(id)], isSpecial);
    }

    std::vector<std::pair<float, std::string>> scoredMerges;
    scoredMerges.reserve(merges.size());
    for (int i = 0; i < static_cast<int>(merges.size()); ++i) {
        float sc = (i < static_cast<int>(scores.size()))
                       ? scores[static_cast<std::size_t>(i)]
                       : -static_cast<float>(i);
        scoredMerges.emplace_back(sc, merges[static_cast<std::size_t>(i)]);
    }
    std::sort(scoredMerges.begin(), scoredMerges.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    uint32_t rank = 0;
    for (const auto& pr : scoredMerges) {
        const std::string& m = pr.second;
        std::size_t sp       = m.find(' ');
        if (sp == std::string::npos) continue;
        addMerge(m.substr(0, sp), m.substr(sp + 1), rank++);
    }

    return true;
}

void Qwen3Tokenizer::addSpecialToken(const std::string& text, TokenId id) {
    specials_[text]    = id;
    specialsById_[id]  = text;
    tokenToId_[text]   = id;
    idToToken_[id]     = text;

    // Keep sortedSpecials_ sorted by length descending for greedy matching.
    auto it = std::find(sortedSpecials_.begin(), sortedSpecials_.end(), text);
    if (it == sortedSpecials_.end()) {
        sortedSpecials_.push_back(text);
        std::sort(sortedSpecials_.begin(), sortedSpecials_.end(),
                  [](const std::string& a, const std::string& b) {
                      return a.size() > b.size(); // longest first
                  });
    }
}

// ─── loadVocabJson ────────────────────────────────────────────────────────────

bool Qwen3Tokenizer::loadVocabJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        lastError_ = "Cannot open: " + path;
        return false;
    }
    std::string src((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    JsonParser p(src);
    p.parseObject([this](const std::string& key, JsonParser& vp) {
        // Key is base64-encoded token bytes; value is integer token id.
        int64_t id = vp.parseInt();
        std::string bytes = base64Decode(key);
        addToken(bytes, static_cast<TokenId>(id));
    });
    if (!p.ok_) {
        lastError_ = "JSON parse error in vocab.json";
        return false;
    }
    return true;
}

// ─── loadTokenizerJson ───────────────────────────────────────────────────────

bool Qwen3Tokenizer::loadTokenizerJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        lastError_ = "Cannot open: " + path;
        return false;
    }
    std::string src((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    JsonParser root(src);

    // tokenizer.json schema:
    // {
    //   "model": {
    //     "vocab": { "<token>": <id>, ... },
    //     "merges": ["<left> <right>", ...]
    //   },
    //   "added_tokens": [ { "id": <id>, "content": "<special>", ... }, ... ]
    // }

    root.parseObject([this](const std::string& key, JsonParser& vp) {
        if (key == "model") {
            vp.parseObject([this](const std::string& mkey, JsonParser& mvp) {
                if (mkey == "vocab") {
                    mvp.parseObject([this](const std::string& token, JsonParser& tvp) {
                        int64_t id = tvp.parseInt();
                        addToken(token, static_cast<TokenId>(id));
                    });
                } else if (mkey == "merges") {
                    uint32_t rank = 0;
                    mvp.parseArray([this, &rank](JsonParser& ap) {
                        std::string merge = ap.parseString();
                        // Format: "left right" (space-separated token strings)
                        std::size_t sp = merge.find(' ');
                        if (sp != std::string::npos) {
                            std::string left  = merge.substr(0, sp);
                            std::string right = merge.substr(sp + 1);
                            auto itL = tokenToId_.find(left);
                            auto itR = tokenToId_.find(right);
                            // Merged token = left + right
                            std::string merged = left + right;
                            auto itM = tokenToId_.find(merged);
                            if (itL != tokenToId_.end() &&
                                itR != tokenToId_.end() &&
                                itM != tokenToId_.end()) {
                                addMerge(itL->second, itR->second,
                                         itM->second, rank);
                            }
                        }
                        ++rank;
                    });
                } else {
                    mvp.skipValue();
                }
            });
        } else if (key == "added_tokens") {
            vp.parseArray([this](JsonParser& ap) {
                std::string content;
                int64_t     id = -1;
                ap.parseObject([&](const std::string& ak, JsonParser& avp) {
                    if      (ak == "id")      id      = avp.parseInt();
                    else if (ak == "content") content = avp.parseString();
                    else                      avp.skipValue();
                });
                if (id >= 0 && !content.empty())
                    addSpecialToken(content, static_cast<TokenId>(id));
            });
        } else {
            vp.skipValue();
        }
    });

    if (!root.ok_) {
        lastError_ = "JSON parse error in tokenizer.json";
        return false;
    }
    return true;
}

// ─── BPE encoding ─────────────────────────────────────────────────────────────

// Encode a single word (byte string) using BPE merges.
TokenIds Qwen3Tokenizer::bpeEncode(const std::string& word) const {
    // Initialise: one token per byte
    std::vector<TokenId> ids;
    ids.reserve(word.size());
    for (unsigned char b : word) {
        std::string byteStr(1, static_cast<char>(b));
        auto it = tokenToId_.find(byteStr);
        if (it != tokenToId_.end()) {
            ids.push_back(it->second);
        } else {
            // Unknown byte — map to its raw unsigned value as a fallback
            ids.push_back(static_cast<TokenId>(b));
        }
    }

    if (ids.size() < 2) return ids;

    // Iteratively apply the highest-priority merge rule.
    // O(n^2) worst case but fine for typical word lengths (<= ~50 tokens).
    while (true) {
        uint32_t bestRank   = std::numeric_limits<uint32_t>::max();
        int      bestIdx    = -1;
        TokenId  bestResult = 0;

        for (int i = 0; i + 1 < static_cast<int>(ids.size()); ++i) {
            BpePair pair{ids[static_cast<std::size_t>(i)],
                         ids[static_cast<std::size_t>(i) + 1]};
            auto it = mergeRank_.find(pair);
            if (it != mergeRank_.end() && it->second < bestRank) {
                bestRank   = it->second;
                bestIdx    = i;
                bestResult = mergeResult_.at(pair);
            }
        }

        if (bestIdx < 0) break; // No more merges possible

        // Apply merge
        auto si = static_cast<std::size_t>(bestIdx);
        ids[si] = bestResult;
        ids.erase(ids.begin() + bestIdx + 1);
    }

    return ids;
}

// ─── Special-token splitting ──────────────────────────────────────────────────

void Qwen3Tokenizer::splitOnSpecials(
        std::string_view text,
        std::vector<std::pair<std::string_view, bool>>& out) const {

    if (sortedSpecials_.empty()) {
        out.push_back({text, false});
        return;
    }

    std::size_t pos = 0;
    while (pos < text.size()) {
        // Try to match any special token at current position
        bool found = false;
        for (const auto& sp : sortedSpecials_) {
            if (text.size() - pos >= sp.size() &&
                text.substr(pos, sp.size()) == sp) {
                if (pos > 0) {} // don't push empty prefix
                out.push_back({text.substr(pos, sp.size()), true});
                pos += sp.size();
                found = true;
                break;
            }
        }
        if (!found) {
            // Find the start of the next special token
            std::size_t nextSpecial = text.size();
            for (const auto& sp : sortedSpecials_) {
                std::size_t f = text.find(sp, pos);
                if (f != std::string_view::npos && f < nextSpecial)
                    nextSpecial = f;
            }
            out.push_back({text.substr(pos, nextSpecial - pos), false});
            pos = nextSpecial;
        }
    }
}

// ─── Pre-tokenization ────────────────────────────────────────────────────────

// Qwen3 uses a simplified regex pattern similar to GPT-4's cl100k_base:
//   - Contractions / apostrophe sequences
//   - Letters + optional digits
//   - Numeric sequences
//   - Whitespace sequences
//   - Any single non-whitespace character
// We implement this as a simple character-class state machine (no regex dep).

std::vector<std::string> Qwen3Tokenizer::preTokenize(std::string_view text) const {
    std::vector<std::string> words;
    std::size_t i = 0;
    const std::size_t n = text.size();

    auto isAlpha = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c > 0x7F;
    };
    auto isDigit = [](unsigned char c) {
        return c >= '0' && c <= '9';
    };
    auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t start = i;

        if (isSpace(c)) {
            // Include leading space in the next word (tiktoken behaviour)
            ++i;
            // Consume more trailing spaces
            // (tiktoken actually splits per word including leading space)
        }

        // Contraction: 's  're  've  'll  'd  'm  't (preceded by letter)
        if (!words.empty() && (c == '\'' || c == '\u2019')) {
            ++i;
            if (i < n) {
                unsigned char c2 = static_cast<unsigned char>(text[i]);
                if (c2 == 's' || c2 == 't' || c2 == 'm' || c2 == 'd') {
                    ++i;
                } else if (c2 == 'r' || c2 == 'v') {
                    if (i + 1 < n && (text[i+1] == 'e')) i += 2;
                } else if (c2 == 'l') {
                    if (i + 1 < n && (text[i+1] == 'l')) i += 2;
                }
            }
        } else if (isAlpha(c)) {
            while (i < n && isAlpha(static_cast<unsigned char>(text[i]))) ++i;
        } else if (isDigit(c)) {
            while (i < n && isDigit(static_cast<unsigned char>(text[i]))) ++i;
        } else {
            ++i; // single punctuation / symbol
        }

        if (i > start)
            words.emplace_back(text.substr(start, i - start));
    }
    return words;
}

// ─── encode ───────────────────────────────────────────────────────────────────

TokenIds Qwen3Tokenizer::encode(std::string_view text) const {
    return encode(text, false, false);
}

TokenIds Qwen3Tokenizer::encode(std::string_view text,
                                 bool addBos, bool addEos) const {
    TokenIds result;
    if (addBos) result.push_back(kEosId);

    // Split on special tokens first
    std::vector<std::pair<std::string_view, bool>> segments;
    splitOnSpecials(text, segments);

    for (const auto& [seg, isSpecial] : segments) {
        if (isSpecial) {
            // Look up the special token id
            auto it = specials_.find(std::string(seg));
            if (it != specials_.end()) {
                result.push_back(it->second);
            }
        } else {
            // Pre-tokenize and BPE-encode
            auto words = preTokenize(seg);
            for (const auto& word : words) {
                if (word.empty()) continue;
                auto ids = bpeEncode(word);
                result.insert(result.end(), ids.begin(), ids.end());
            }
        }
    }

    if (addEos) result.push_back(kEosId);
    return result;
}

// ─── decode ───────────────────────────────────────────────────────────────────

std::string Qwen3Tokenizer::decode(const TokenIds& ids) const {
    std::string bytes;
    bytes.reserve(ids.size() * 3);
    for (TokenId id : ids) {
        auto it = idToToken_.find(id);
        if (it != idToToken_.end()) {
            bytes += it->second;
        } else {
            // U+FFFD replacement character (UTF-8: EF BF BD)
            bytes += "\xEF\xBF\xBD";
        }
    }
    return bytes;
}

std::string Qwen3Tokenizer::decodeToken(TokenId id) const {
    auto it = idToToken_.find(id);
    return it != idToToken_.end() ? it->second : "\xEF\xBF\xBD";
}

// ─── Chat template ────────────────────────────────────────────────────────────

void Qwen3Tokenizer::encodeChat(const std::vector<ChatMessage>& messages,
                                 TokenIds& out,
                                 bool enableThinking) const {
    // Qwen3 ChatML template:
    //   <|im_start|>role\ncontent<|im_end|>\n
    auto roleStr = [](ChatRole r) -> std::string {
        switch (r) {
        case ChatRole::System:    return "system";
        case ChatRole::User:      return "user";
        case ChatRole::Assistant: return "assistant";
        }
        return "user";
    };

    for (const auto& msg : messages) {
        // <|im_start|>role\n
        out.push_back(kImStartId);
        auto roleIds = encode(roleStr(msg.role) + "\n");
        out.insert(out.end(), roleIds.begin(), roleIds.end());

        // content
        auto contentIds = encode(msg.content);
        out.insert(out.end(), contentIds.begin(), contentIds.end());

        // <|im_end|>\n
        out.push_back(kImEndId);
        auto nlIds = encode("\n");
        out.insert(out.end(), nlIds.begin(), nlIds.end());
    }

    // Begin assistant turn
    out.push_back(kImStartId);
    auto assistantIds = encode("assistant\n");
    out.insert(out.end(), assistantIds.begin(), assistantIds.end());

    if (enableThinking)
        out.push_back(kThinkStart);
}

// ─── Queries ─────────────────────────────────────────────────────────────────

std::optional<TokenId> Qwen3Tokenizer::tokenToId(const std::string& token) const {
    auto it = tokenToId_.find(token);
    if (it == tokenToId_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> Qwen3Tokenizer::idToToken(TokenId id) const {
    auto it = idToToken_.find(id);
    if (it == idToToken_.end()) return std::nullopt;
    return it->second;
}

bool Qwen3Tokenizer::isSpecialToken(TokenId id) const {
    return specialsById_.count(id) > 0;
}

} // namespace retdec::qwen3
