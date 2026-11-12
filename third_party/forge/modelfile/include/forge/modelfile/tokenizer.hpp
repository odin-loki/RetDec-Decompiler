#pragma once
// Byte-level BPE tokenizer (GPT-2 / Llama-3 / Qwen family) loaded from a HF tokenizer.json.
#include <forge/json/json.hpp>
#include <climits>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::modelfile {

class bpe_tokenizer {
public:
    bpe_tokenizer() = default;
    explicit bpe_tokenizer(std::string_view tokenizer_json) {
        init_bytes();
        auto j = json::parse(tokenizer_json);
        const auto& m = j["model"];
        for (auto& [tok, id] : m["vocab"].obj()) add(tok, int(id.i()));
        int rank = 0;
        for (auto& mg : m["merges"].arr()) {
            std::string a, b;
            if (mg.is_str()) { auto& s = mg.str(); auto sp = s.find(' '); a = s.substr(0, sp); b = s.substr(sp + 1); }
            else { a = mg[0].str(); b = mg[1].str(); }
            ranks_[a + '\x01' + b] = rank++;
        }
        if (auto* at = j.find("added_tokens"); at && at->is_arr())
            for (auto& t : at->arr()) { add(t["content"].str(), int(t["id"].i())); special_.push_back(t["content"].str()); }
        if (auto* b = j.find("bos_id")) bos_ = int(b->i());
        if (auto* e = j.find("eos_id")) eos_ = int(e->i());
    }
    std::size_t vocab_size() const { return id_to_tok_.size(); }
    int bos() const { return bos_; }
    int eos() const { return eos_; }
    void set_special(int bos, int eos) { bos_ = bos; eos_ = eos; }
    // Whether a prompt starts with BOS (when the model has one): set from the manifest's "add_bos".
    bool add_bos() const { return add_bos_ && bos_ >= 0; }
    void set_add_bos(bool on) { add_bos_ = on; }

    std::vector<int> encode(std::string_view text) const {
        std::vector<int> out;
        std::size_t p = 0;
        while (p < text.size()) {
            // special tokens are matched verbatim
            bool hit = false;
            for (auto& s : special_) if (!s.empty() && text.substr(p, s.size()) == s) { out.push_back(tok_to_id_.at(s)); p += s.size(); hit = true; break; }
            if (hit) continue;
            std::size_t e = next_piece(text, p);
            std::size_t stop = e;
            for (auto& s : special_) { auto f = text.substr(0, e).find(s, p); if (f != std::string_view::npos && f < stop) stop = f; }
            if (stop == p) stop = e;
            bpe(text.substr(p, stop - p), out);
            p = stop;
        }
        return out;
    }
    std::string decode(const std::vector<int>& ids) const {
        std::string s; for (int i : ids) s += decode_one(i); return s;
    }
    std::string decode_one(int id) const {
        if (id < 0 || std::size_t(id) >= id_to_tok_.size()) return "";
        const auto& t = id_to_tok_[std::size_t(id)];
        for (auto& sp : special_) if (sp == t) return t;
        std::string o; std::size_t i = 0;
        while (i < t.size()) {
            std::size_t l = utf8_len(static_cast<unsigned char>(t[i]));
            auto it = unbyte_.find(t.substr(i, l));
            if (it != unbyte_.end()) o += char(it->second); else o += t.substr(i, l);
            i += l;
        }
        return o;
    }
    int token_id(const std::string& t) const { auto it = tok_to_id_.find(t); return it == tok_to_id_.end() ? -1 : it->second; }

private:
    static std::size_t utf8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
        else { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    }
    void init_bytes() {
        // GPT-2 bytes_to_unicode
        int n = 0;
        for (int b = 0; b < 256; ++b) {
            bool keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
            unsigned cp = keep ? unsigned(b) : unsigned(256 + n++);
            std::string s; utf8(s, cp);
            byte_[std::size_t(b)] = s; unbyte_[s] = b;
        }
    }
    void add(const std::string& tok, int id) {
        if (std::size_t(id) >= id_to_tok_.size()) id_to_tok_.resize(std::size_t(id) + 1);
        id_to_tok_[std::size_t(id)] = tok; tok_to_id_[tok] = id;
    }
    static int cls(unsigned char c) {  // 0 space, 1 letter, 2 digit, 3 other
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
        if (std::isalpha(c) || c >= 0x80) return 1;
        if (std::isdigit(c)) return 2;
        return 3;
    }
    // Approximates the GPT-2 pre-tokenizer: 's|'t|... | ?L+| ?N+| ?[^sLN]+|\s+(?!\S)|\s+
    static std::size_t next_piece(std::string_view t, std::size_t p) {
        static const char* contr[] = {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"};
        for (auto c : contr) { std::string_view cv(c); if (t.substr(p, cv.size()) == cv) return p + cv.size(); }
        std::size_t q = p;
        if (t[q] == ' ' && q + 1 < t.size() && cls((unsigned char)t[q + 1]) != 0) ++q;  // " word"
        else if (cls((unsigned char)t[q]) == 0) {
            while (q < t.size() && cls((unsigned char)t[q]) == 0) ++q;
            if (q < t.size() && q - p > 1 && t[q - 1] == ' ') --q;  // \s+(?!\S)
            return q;
        }
        int c = cls((unsigned char)t[q]);
        while (q < t.size() && cls((unsigned char)t[q]) == c) ++q;
        return q;
    }
    void bpe(std::string_view piece, std::vector<int>& out) const {
        std::vector<std::string> sym;
        for (unsigned char c : piece) sym.push_back(byte_[c]);
        while (sym.size() > 1) {
            int best = INT_MAX; std::size_t bi = 0;
            for (std::size_t i = 0; i + 1 < sym.size(); ++i) {
                auto it = ranks_.find(sym[i] + '\x01' + sym[i + 1]);
                if (it != ranks_.end() && it->second < best) { best = it->second; bi = i; }
            }
            if (best == INT_MAX) break;
            sym[bi] += sym[bi + 1]; sym.erase(sym.begin() + std::ptrdiff_t(bi) + 1);
        }
        for (auto& s : sym) {
            auto it = tok_to_id_.find(s);
            if (it != tok_to_id_.end()) out.push_back(it->second);
            else for (unsigned char c : s) { (void)c; }  // unknown symbol: dropped (byte vocab normally covers all)
        }
    }
    std::string byte_[256];
    std::unordered_map<std::string, int> unbyte_;
    std::unordered_map<std::string, int> tok_to_id_;
    std::vector<std::string> id_to_tok_;
    std::unordered_map<std::string, int> ranks_;
    std::vector<std::string> special_;
    int bos_ = -1, eos_ = -1; bool add_bos_ = true;
};

} // namespace forge::modelfile
