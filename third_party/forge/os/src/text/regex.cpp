// forge::os::text: a small backtracking regex engine over code points with Unicode classes.
#include "internal.hpp"
#include <algorithm>
#include <cerrno>

namespace forge::os::text {
using namespace detail;
namespace {
struct cls {
    std::vector<std::pair<cp_t, cp_t>> ranges;
    std::vector<std::pair<std::string, bool>> props;   // name, negated
    std::vector<std::pair<char, bool>> builtins;       // 'd' 'w' 's', negated
    bool neg = false;
    bool test1(cp_t c) const {
        for (auto& [a, b] : ranges) if (c >= a && c <= b) return true;
        for (auto& [n, ng] : props) if (has_property(c, n) != ng) return true;
        for (auto& [k, ng] : builtins) {
            bool v = k == 'd' ? is_digit(c) : k == 'w' ? is_word_char(c) : is_space(c);
            if (v != ng) return true;
        }
        return false;
    }
    bool test(cp_t c, bool icase) const {
        bool v = test1(c);
        if (!v && icase) v = test1(simple_fold(c)) || test1(simple_upper(c)) || test1(simple_lower(c));
        return v != neg;
    }
};
enum op : std::uint8_t { CHAR, ANY, CLASS, SPLIT, JMP, SAVE, BOL, EOL, SBEG, SEND, SENDNL, WB, NWB, GRAPH, BREF, LOOK, MATCH, SETPOS, CHKPOS, FAIL };
struct ins { op o; std::uint32_t a = 0, b = 0; bool neg = false; };

struct node {
    enum kind { ch, any, klass, bol, eol, sbeg, send, sendnl, wb, nwb, graph, group, concat, alt, rep, bref, look, empty } k;
    cp_t c = 0; int idx = -1; int min = 0, max = 0; bool greedy = true, neg = false;
    std::vector<node> kids;
};
}  // namespace

struct regex::impl {
    std::vector<ins> prog; std::vector<cls> classes; int ngroups = 1; std::vector<std::string> names{""};
    bool icase = false, multiline = false, dotall = false;
    int nslots = 0; std::uint64_t step_limit = 10'000'000; mutable bool hit = false;
};

namespace {
struct parser {
    std::u32string p; std::size_t i = 0; bool ext = false; regex::impl& R; std::string err;
    parser(std::u32string s, regex::impl& r, bool x) : p(std::move(s)), ext(x), R(r) {}
    bool eof() { skip_ws(); return i >= p.size(); }
    void skip_ws() {
        if (!ext) return;
        while (i < p.size()) {
            if (is_space(p[i])) ++i;
            else if (p[i] == '#') { while (i < p.size() && p[i] != '\n') ++i; }
            else break;
        }
    }
    cp_t peek() { skip_ws(); return i < p.size() ? p[i] : 0; }
    bool fail(std::string m) { if (err.empty()) err = m + " at position " + std::to_string(i); return false; }
    std::optional<cp_t> hex(std::size_t n) {
        cp_t v = 0; std::size_t k = 0;
        for (; k < n && i < p.size() && std::isxdigit(int(p[i] < 128 ? p[i] : 0)); ++k, ++i) v = v * 16 + cp_t(std::isdigit(int(p[i])) ? p[i] - '0' : (p[i] | 0x20) - 'a' + 10);
        if (!k) return std::nullopt; return v;
    }
    std::string braced() {   // after '{'
        std::string s; while (i < p.size() && p[i] != '}') append_utf8(s, p[i++]);
        if (i < p.size()) ++i; return s;
    }
    // Escapes usable both inside and outside classes. Returns: 0 = literal in `c`, 1 = class appended to `k`.
    int escape(cp_t& c, cls& k) {
        if (i >= p.size()) { fail("trailing backslash"); return -1; }
        cp_t e = p[i++];
        switch (e) {
        case 'n': c = '\n'; return 0; case 't': c = '\t'; return 0; case 'r': c = '\r'; return 0; case 'f': c = '\f'; return 0;
        case 'v': c = '\v'; return 0; case 'a': c = 7; return 0; case 'e': c = 0x1B; return 0; case '0': c = 0; return 0;
        case 'x': if (i < p.size() && p[i] == '{') { ++i; auto v = hex(8); if (i < p.size() && p[i] == '}') ++i; c = v.value_or(0); } else c = hex(2).value_or(0); return 0;
        case 'u': if (i < p.size() && p[i] == '{') { ++i; auto v = hex(8); if (i < p.size() && p[i] == '}') ++i; c = v.value_or(0); } else c = hex(4).value_or(0); return 0;
        case 'U': c = hex(8).value_or(0); return 0;
        case 'd': case 'w': case 's': k.builtins.push_back({char(e), false}); return 1;
        case 'D': case 'W': case 'S': k.builtins.push_back({char(e | 0x20), true}); return 1;
        case 'p': case 'P': {
            std::string name;
            if (i < p.size() && p[i] == '{') { ++i; name = braced(); } else if (i < p.size()) append_utf8(name, p[i++]);
            bool neg = e == 'P'; if (!name.empty() && name[0] == '^') { neg = !neg; name.erase(0, 1); }
            bool known = false; has_property('a', name, &known);
            if (!known) { fail("unknown property \\p{" + name + "}"); return -1; }
            k.props.push_back({name, neg}); return 1; }
        default: c = e; return 0;
        }
    }
    bool parse_class(node& n) {   // after '['
        cls k; if (i < p.size() && p[i] == '^') { k.neg = true; ++i; }
        bool first = true;
        while (i < p.size() && (p[i] != ']' || first)) {
            first = false;
            if (p[i] == '[' && i + 1 < p.size() && p[i + 1] == ':') {   // POSIX [:alpha:]
                auto e = p.find(U":]", i + 2); if (e == std::u32string::npos) return fail("bad POSIX class");
                std::string name = encode_utf8(p.substr(i + 2, e - i - 2)); i = e + 2;
                if (name == "digit") k.builtins.push_back({'d', false}); else if (name == "space") k.builtins.push_back({'s', false});
                else if (name == "alnum") { k.props.push_back({"Alphabetic", false}); k.builtins.push_back({'d', false}); }
                else if (name == "word") k.builtins.push_back({'w', false}); else if (name == "xdigit") k.props.push_back({"Hex_Digit", false});
                else if (name == "alpha" || name == "upper" || name == "lower" || name == "punct" || name == "cntrl")
                    k.props.push_back({name == "alpha" ? "Alphabetic" : name == "upper" ? "Uppercase" : name == "lower" ? "Lowercase" : name == "punct" ? "P" : "Cc", false});
                else return fail("unknown POSIX class " + name);
                continue;
            }
            cp_t lo;
            if (p[i] == '\\') { ++i; int r = escape(lo, k); if (r < 0) return false; if (r == 1) continue; }
            else lo = p[i++];
            cp_t hi = lo;
            if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']') {
                ++i;
                if (p[i] == '\\') { ++i; cls dummy; int r = escape(hi, dummy); if (r != 0) return fail("bad class range"); }
                else hi = p[i++];
                if (hi < lo) return fail("reversed class range");
            }
            k.ranges.push_back({lo, hi});
        }
        if (i >= p.size()) return fail("unterminated [");
        ++i;
        n.k = node::klass; n.idx = int(R.classes.size()); R.classes.push_back(std::move(k));
        return true;
    }
    bool atom(node& n) {
        cp_t c = peek();
        ++i;
        switch (c) {
        case '.': n.k = node::any; return true;
        case '^': n.k = node::bol; return true;
        case '$': n.k = node::eol; return true;
        case '[': return parse_class(n);
        case '(': {
            node g; g.k = node::group;
            if (i < p.size() && p[i] == '?') {
                ++i;
                if (i < p.size() && p[i] == ':') { ++i; g.idx = -1; }
                else if (i < p.size() && (p[i] == '=' || p[i] == '!')) { g.k = node::look; g.neg = p[i] == '!'; ++i; }
                else if (i < p.size() && (p[i] == '<' || p[i] == 'P' || p[i] == '\'')) {
                    if (p[i] == 'P') ++i;
                    cp_t close = p[i] == '\'' ? '\'' : '>'; ++i;
                    std::string name; while (i < p.size() && p[i] != close) append_utf8(name, p[i++]);
                    if (i >= p.size()) return fail("unterminated group name");
                    ++i; g.idx = R.ngroups++; R.names.push_back(name);
                } else return fail("unsupported group syntax");
            } else { g.idx = R.ngroups++; R.names.push_back(""); }
            node body; if (!alternation(body)) return false;
            if (peek() != ')') return fail("missing )");
            ++i; g.kids.push_back(std::move(body)); n = std::move(g); return true; }
        case ')': --i; return fail("unbalanced )");
        case '*': case '+': case '?': return fail("nothing to repeat");
        case '\\': {
            if (i < p.size()) {
                cp_t e = p[i];
                if (e == 'b') { ++i; n.k = node::wb; return true; }
                if (e == 'B') { ++i; n.k = node::nwb; return true; }
                if (e == 'A') { ++i; n.k = node::sbeg; return true; }
                if (e == 'z') { ++i; n.k = node::send; return true; }
                if (e == 'Z') { ++i; n.k = node::sendnl; return true; }
                if (e == 'X') { ++i; n.k = node::graph; return true; }
                if (e >= '1' && e <= '9') { int v = 0; while (i < p.size() && p[i] >= '0' && p[i] <= '9') v = v * 10 + int(p[i++] - '0'); n.k = node::bref; n.idx = v; return true; }
                if (e == 'k' && i + 1 < p.size() && p[i + 1] == '<') {
                    i += 2; std::string name; while (i < p.size() && p[i] != '>') append_utf8(name, p[i++]); ++i;
                    for (std::size_t g = 0; g < R.names.size(); ++g) if (R.names[g] == name) { n.k = node::bref; n.idx = int(g); return true; }
                    return fail("unknown group name " + name);
                }
            }
            cls k; cp_t lit; int r = escape(lit, k); if (r < 0) return false;
            if (r == 1) { n.k = node::klass; n.idx = int(R.classes.size()); R.classes.push_back(std::move(k)); return true; }
            n.k = node::ch; n.c = lit; return true; }
        default: n.k = node::ch; n.c = c; return true;
        }
    }
    bool repeat(node& n) {
        if (!atom(n)) return false;
        while (true) {
            cp_t c = peek(); int mn, mx;
            if (c == '*') { mn = 0; mx = -1; ++i; }
            else if (c == '+') { mn = 1; mx = -1; ++i; }
            else if (c == '?') { mn = 0; mx = 1; ++i; }
            else if (c == '{') {
                std::size_t save = i; ++i; int a = 0, b; bool digits = false;
                while (i < p.size() && p[i] >= '0' && p[i] <= '9') { a = a * 10 + int(p[i++] - '0'); digits = true; }
                if (!digits) { i = save; return true; }
                b = a;
                if (i < p.size() && p[i] == ',') { ++i; if (i < p.size() && p[i] >= '0' && p[i] <= '9') { b = 0; while (i < p.size() && p[i] >= '0' && p[i] <= '9') b = b * 10 + int(p[i++] - '0'); } else b = -1; }
                if (i >= p.size() || p[i] != '}') { i = save; return true; }
                ++i; mn = a; mx = b;
                if (mx >= 0 && mx < mn) return fail("bad repeat range");
                if (mn > 1000 || mx > 1000) return fail("repeat count too large (max 1000)");
            } else return true;
            bool greedy = true;
            if (i < p.size() && p[i] == '?') { greedy = false; ++i; } else if (i < p.size() && p[i] == '+') ++i;   // possessive: treated as greedy
            node r; r.k = node::rep; r.min = mn; r.max = mx; r.greedy = greedy; r.kids.push_back(std::move(n)); n = std::move(r);
        }
    }
    bool concat(node& n) {
        n.k = node::concat;
        while (!eof() && peek() != '|' && peek() != ')') { node a; if (!repeat(a)) return false; n.kids.push_back(std::move(a)); }
        return true;
    }
    bool alternation(node& n) {
        node first; if (!concat(first)) return false;
        if (peek() != '|') { n = std::move(first); return true; }
        n.k = node::alt; n.kids.push_back(std::move(first));
        while (peek() == '|') { ++i; node b; if (!concat(b)) return false; n.kids.push_back(std::move(b)); }
        return true;
    }
};

struct compiler {
    regex::impl& R; std::string err;
    std::size_t emit(ins x) { R.prog.push_back(x); return R.prog.size() - 1; }
    bool gen(const node& n) {
        if (R.prog.size() > 200000) { err = "pattern too large"; return false; }
        switch (n.k) {
        case node::ch: emit({CHAR, std::uint32_t(n.c)}); break;
        case node::any: emit({ANY}); break;
        case node::klass: emit({CLASS, std::uint32_t(n.idx)}); break;
        case node::bol: emit({BOL}); break;
        case node::eol: emit({EOL}); break;
        case node::sbeg: emit({SBEG}); break;
        case node::send: emit({SEND}); break;
        case node::sendnl: emit({SENDNL}); break;
        case node::wb: emit({WB}); break;
        case node::nwb: emit({NWB}); break;
        case node::graph: emit({GRAPH}); break;
        case node::bref: emit({BREF, std::uint32_t(n.idx)}); break;
        case node::empty: break;
        case node::concat: for (auto& k : n.kids) if (!gen(k)) return false; break;
        case node::group:
            if (n.idx >= 0) emit({SAVE, std::uint32_t(2 * n.idx)});
            if (!gen(n.kids[0])) return false;
            if (n.idx >= 0) emit({SAVE, std::uint32_t(2 * n.idx + 1)});
            break;
        case node::look: {
            auto at = emit({LOOK, 0, 0, n.neg});
            if (!gen(n.kids[0])) return false;
            emit({MATCH});
            R.prog[at].a = std::uint32_t(at + 1); R.prog[at].b = std::uint32_t(R.prog.size());
            break; }
        case node::alt: {
            std::vector<std::size_t> jumps;
            for (std::size_t k = 0; k < n.kids.size(); ++k) {
                if (k + 1 < n.kids.size()) {
                    auto sp = emit({SPLIT});
                    R.prog[sp].a = std::uint32_t(sp + 1);
                    if (!gen(n.kids[k])) return false;
                    jumps.push_back(emit({JMP}));
                    R.prog[sp].b = std::uint32_t(R.prog.size());
                } else if (!gen(n.kids[k])) return false;
            }
            for (auto j : jumps) R.prog[j].a = std::uint32_t(R.prog.size());
            break; }
        case node::rep: {
            auto& body = n.kids[0];
            for (int k = 0; k < n.min; ++k) if (!gen(body)) return false;
            if (n.max < 0) {   // star with empty-iteration guard
                int slot = R.nslots++;
                auto sp = emit({SPLIT});
                emit({SETPOS, std::uint32_t(slot)});
                if (!gen(body)) return false;
                emit({CHKPOS, std::uint32_t(slot)});
                emit({JMP, std::uint32_t(sp)});
                auto out = R.prog.size();
                R.prog[sp].a = n.greedy ? std::uint32_t(sp + 1) : std::uint32_t(out);
                R.prog[sp].b = n.greedy ? std::uint32_t(out) : std::uint32_t(sp + 1);
            } else {
                std::vector<std::size_t> splits;
                for (int k = n.min; k < n.max; ++k) { splits.push_back(emit({SPLIT})); if (!gen(body)) return false; }
                auto out = R.prog.size();
                for (auto sp : splits) {
                    R.prog[sp].a = n.greedy ? std::uint32_t(sp + 1) : std::uint32_t(out);
                    R.prog[sp].b = n.greedy ? std::uint32_t(out) : std::uint32_t(sp + 1);
                }
            }
            break; }
        }
        return true;
    }
};

struct vm {
    const regex::impl& R; const std::u32string& s; std::uint64_t steps = 0;
    std::vector<long> caps, pos_slots;
    bool word_at(long i) const { return i >= 0 && std::size_t(i) < s.size() && is_word_char(s[std::size_t(i)]); }
    bool eq(cp_t a, cp_t b) const { return a == b || (R.icase && simple_fold(a) == simple_fold(b)); }
    // Runs from pc at pos; on success sets end and leaves captures.
    bool run(std::size_t pc0, long pos0, long& end) {
        struct frame { std::uint8_t kind; std::uint32_t pc; long pos; };   // kind 0 branch, 1 restore cap, 2 restore slot
        std::vector<frame> st; st.push_back({0, std::uint32_t(pc0), pos0});
        while (!st.empty()) {
            auto f = st.back(); st.pop_back();
            if (f.kind == 1) { caps[f.pc] = f.pos; continue; }
            if (f.kind == 2) { pos_slots[f.pc] = f.pos; continue; }
            std::size_t pc = f.pc; long pos = f.pos;
            while (true) {
                if (++steps > R.step_limit) { R.hit = true; return false; }
                const ins& x = R.prog[pc];
                bool ok = true;
                switch (x.o) {
                case CHAR: ok = std::size_t(pos) < s.size() && eq(s[pos], cp_t(x.a)); if (ok) { ++pos; ++pc; } break;
                case ANY: ok = std::size_t(pos) < s.size() && (R.dotall || s[pos] != '\n'); if (ok) { ++pos; ++pc; } break;
                case CLASS: ok = std::size_t(pos) < s.size() && R.classes[x.a].test(s[pos], R.icase); if (ok) { ++pos; ++pc; } break;
                case BOL: ok = pos == 0 || (R.multiline && s[pos - 1] == '\n'); ++pc; break;
                case EOL: ok = std::size_t(pos) == s.size() || (R.multiline && s[pos] == '\n') || (!R.multiline && std::size_t(pos) + 1 == s.size() && s[pos] == '\n'); ++pc; break;
                case SBEG: ok = pos == 0; ++pc; break;
                case SEND: ok = std::size_t(pos) == s.size(); ++pc; break;
                case SENDNL: ok = std::size_t(pos) == s.size() || (std::size_t(pos) + 1 == s.size() && s[pos] == '\n'); ++pc; break;
                case WB: ok = word_at(pos - 1) != word_at(pos); ++pc; break;
                case NWB: ok = word_at(pos - 1) == word_at(pos); ++pc; break;
                case GRAPH: {
                    ok = std::size_t(pos) < s.size();
                    if (ok) { auto b = grapheme_breaks32(std::u32string_view(s).substr(pos, std::min<std::size_t>(s.size() - pos, 64))); pos += long(b.size() > 1 ? b[1] : 1); }
                    ++pc; break; }
                case BREF: {
                    long a = 2 * long(x.a) < long(caps.size()) ? caps[2 * x.a] : -1, b = 2 * long(x.a) + 1 < long(caps.size()) ? caps[2 * x.a + 1] : -1;
                    if (a < 0 || b < 0) { ++pc; break; }   // unset group matches empty
                    for (long k = a; k < b && ok; ++k, ++pos) ok = std::size_t(pos) < s.size() && eq(s[pos], s[k]);
                    ++pc; break; }
                case SPLIT: st.push_back({0, x.b, pos}); pc = x.a; break;
                case JMP: pc = x.a; break;
                case SAVE: st.push_back({1, x.a, caps[x.a]}); caps[x.a] = pos; ++pc; break;
                case SETPOS: st.push_back({2, x.a, pos_slots[x.a]}); pos_slots[x.a] = pos; ++pc; break;
                case CHKPOS: ok = pos_slots[x.a] != pos; ++pc; break;
                case LOOK: {
                    vm sub{R, s, 0, caps, pos_slots}; long e;
                    bool m = sub.run(x.a, pos, e); steps += sub.steps;
                    if (R.hit) return false;
                    ok = m != x.neg;
                    if (ok && m && !x.neg) caps = sub.caps;
                    pc = x.b; break; }
                case MATCH: end = pos; return true;
                case FAIL: ok = false; break;
                }
                if (!ok) break;
            }
        }
        return false;
    }
};
}  // namespace

regex::regex() : p_(std::make_shared<impl>()) {}
regex::~regex() = default;
regex::regex(regex&&) noexcept = default;
regex& regex::operator=(regex&&) noexcept = default;
regex::regex(const regex&) = default;
regex& regex::operator=(const regex&) = default;
std::size_t regex::group_count() const { return std::size_t(p_->ngroups - 1); }
void regex::set_step_limit(std::uint64_t n) { auto c = std::make_shared<impl>(*p_); c->step_limit = n; p_ = std::move(c); }
bool regex::step_limit_hit() const { return p_->hit; }

result<regex> regex::compile(std::string_view pattern, std::string_view flags) {
    result<regex> r; auto im = std::make_shared<impl>();
    bool ext = false;
    for (char f : flags) {
        if (f == 'i') im->icase = true; else if (f == 'm') im->multiline = true; else if (f == 's') im->dotall = true; else if (f == 'x') ext = true;
        else if (f == 'g' || f == 'u') {}
        else { r.err = {EINVAL, std::string("regex: unknown flag '") + f + "'"}; return r; }
    }
    parser ps(to32(pattern), *im, ext);
    node root;
    if (!ps.alternation(root) || (!ps.eof() && ps.err.empty() && ps.fail("unexpected )"))) {}
    if (!ps.err.empty()) { r.err = {EINVAL, "regex: " + ps.err}; return r; }
    compiler c{*im};
    if (!c.gen(root)) { r.err = {EINVAL, "regex: " + c.err}; return r; }
    im->prog.push_back({MATCH});
    r.value.p_ = std::move(im);
    return r;
}

std::string_view regex_match::str(std::string_view s, std::size_t g) const {
    if (g >= groups.size() || groups[g].first == std::string_view::npos) return {};
    return s.substr(groups[g].first, groups[g].second - groups[g].first);
}
std::optional<std::size_t> regex_match::group_index(std::string_view n) const {
    for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == n) return i;
    return std::nullopt;
}

namespace {
std::optional<regex_match> exec(const regex::impl& R, std::string_view s8, const std::u32string& s, const std::vector<std::size_t>& off, std::size_t from_cp, bool anchored_full) {
    R.hit = false;
    vm v{R, s, 0, {}, {}};
    for (std::size_t start = from_cp; start <= s.size(); ++start) {
        v.caps.assign(std::size_t(2 * R.ngroups), -1); v.pos_slots.assign(std::size_t(R.nslots), -1);
        long end;
        if (v.run(0, long(start), end) && (!anchored_full || std::size_t(end) == s.size())) {
            regex_match m; m.names = R.names; m.groups.resize(std::size_t(R.ngroups), {std::string_view::npos, std::string_view::npos});
            m.groups[0] = {off[start], off[std::size_t(end)]};
            for (int g = 1; g < R.ngroups; ++g) if (v.caps[2 * g] >= 0 && v.caps[2 * g + 1] >= 0) m.groups[g] = {off[v.caps[2 * g]], off[v.caps[2 * g + 1]]};
            return m;
        }
        if (R.hit || anchored_full) break;
    }
    (void)s8;
    return std::nullopt;
}
}  // namespace

std::optional<regex_match> regex::match(std::string_view s) const {
    auto v = to32(s); auto off = cp_byte_offsets(s);
    // Whole-string match: anchor by trying with an end check; patterns with alternation need the full search, so
    // compile-time anchoring is emulated by wrapping: try start 0 and require end == size.
    impl tmp = *p_; tmp.prog.clear();
    // Rebuild with \z before MATCH: copy program and replace terminal MATCH (last instruction) with SEND, MATCH.
    tmp.prog = p_->prog; tmp.prog.back() = {SEND}; tmp.prog.push_back({MATCH});
    auto r = exec(tmp, s, v, off, 0, true); p_->hit = tmp.hit; return r;
}
std::optional<regex_match> regex::search(std::string_view s, std::size_t from) const {
    auto v = to32(s); auto off = cp_byte_offsets(s);
    std::size_t cp = std::lower_bound(off.begin(), off.end(), from) - off.begin();
    return exec(*p_, s, v, off, cp, false);
}
std::vector<regex_match> regex::find_all(std::string_view s) const {
    std::vector<regex_match> out; auto v = to32(s); auto off = cp_byte_offsets(s);
    std::size_t cp = 0;
    while (cp <= v.size()) {
        auto m = exec(*p_, s, v, off, cp, false); if (!m) break;
        std::size_t e = std::lower_bound(off.begin(), off.end(), m->end()) - off.begin();
        std::size_t b = std::lower_bound(off.begin(), off.end(), m->begin()) - off.begin();
        out.push_back(std::move(*m));
        cp = e > b ? e : e + 1;
    }
    return out;
}
std::string regex::replace(std::string_view s, std::string_view repl, bool all) const {
    auto ms = all ? find_all(s) : std::vector<regex_match>{};
    if (!all) if (auto m = search(s)) ms.push_back(*m);
    std::string o; std::size_t last = 0;
    for (auto& m : ms) {
        o.append(s.substr(last, m.begin() - last));
        for (std::size_t i = 0; i < repl.size(); ++i) {
            if (repl[i] != '$' || i + 1 >= repl.size()) { o += repl[i]; continue; }
            char n = repl[i + 1];
            if (n == '$') { o += '$'; ++i; }
            else if (n >= '0' && n <= '9') { o.append(m.str(s, std::size_t(n - '0'))); ++i; }
            else if (n == '{') {
                auto e = repl.find('}', i + 2); if (e == std::string_view::npos) { o += repl[i]; continue; }
                auto name = repl.substr(i + 2, e - i - 2);
                if (auto g = m.group_index(name)) o.append(m.str(s, *g));
                else if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) o.append(m.str(s, std::size_t(std::stoul(std::string(name)))));
                i = e;
            } else o += repl[i];
        }
        last = m.end();
    }
    o.append(s.substr(last));
    return o;
}
std::vector<std::string> regex::split(std::string_view s) const {
    std::vector<std::string> r; std::size_t last = 0;
    for (auto& m : find_all(s)) { if (m.end() == m.begin() && (m.begin() == 0 || m.begin() == s.size())) continue; r.emplace_back(s.substr(last, m.begin() - last)); last = m.end(); }
    r.emplace_back(s.substr(last));
    return r;
}
}  // namespace forge::os::text
