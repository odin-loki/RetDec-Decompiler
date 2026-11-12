// UI8 cookers (see cook.hpp).
#include "fuse/ui/doc/cook.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace fuse::ui::doc {

// ---- byte writer / validating reader -------------------------------------------------------------------------------
namespace {
constexpr char kDocMagic[4] = {'F', 'U', 'I', 'C'};
constexpr char kStyleMagic[4] = {'F', 'U', 'S', 'C'};
constexpr u32 kMaxDepth = 128;

struct Writer {
    std::vector<u8> b;
    void raw(const void* p, std::size_t n) { const u8* c = static_cast<const u8*>(p); b.insert(b.end(), c, c + n); }
    void u8_(u8 v) { b.push_back(v); }
    void u32_(u32 v) { raw(&v, 4); }
    void i32_(i32 v) { raw(&v, 4); }
    void f32_(f32 v) { raw(&v, 4); }
    void f64_(double v) { raw(&v, 8); }
    void str(std::string_view s) { u32_(u32(s.size())); raw(s.data(), s.size()); }
};
struct Reader {
    std::span<const u8> b;
    std::size_t at = 0;
    bool ok = true;
    bool take(void* p, std::size_t n) {
        if (!ok || n > b.size() - at) { ok = false; return false; }
        std::memcpy(p, b.data() + at, n);
        at += n;
        return true;
    }
    u8 u8_() { u8 v = 0; take(&v, 1); return v; }
    u32 u32_() { u32 v = 0; take(&v, 4); return v; }
    i32 i32_() { i32 v = 0; take(&v, 4); return v; }
    f32 f32_() { f32 v = 0; take(&v, 4); return v; }
    double f64_() { double v = 0; take(&v, 8); return v; }
    /// A count of items each at least `minBytes` long (rejects counts the remaining bytes cannot hold).
    u32 count(std::size_t minBytes) {
        const u32 n = u32_();
        if (ok && std::size_t(n) * std::max<std::size_t>(minBytes, 1) > b.size() - at) ok = false;
        return ok ? n : 0;
    }
    std::string str() {
        const u32 n = count(1);
        std::string s;
        if (ok) { s.assign(reinterpret_cast<const char*>(b.data() + at), n); at += n; }
        return s;
    }
    bool magic(const char (&m)[4]) {
        char got[4] = {};
        return take(got, 4) && std::memcmp(got, m, 4) == 0 && u32_() == kCookedVersion;
    }
};

// ---- JSON tree --------------------------------------------------------------------------------------------------
void writeJson(Writer& w, const JsonValue& v) {
    w.u8_(u8(v.type));
    switch (v.type) {
    case JsonValue::Type::Null: break;
    case JsonValue::Type::Bool: w.u8_(v.boolean ? 1 : 0); break;
    case JsonValue::Type::Number: w.f64_(v.number); break;
    case JsonValue::Type::String: w.str(v.string); break;
    case JsonValue::Type::Array:
        w.u32_(u32(v.array.size()));
        for (const JsonValue& e : v.array) writeJson(w, e);
        break;
    case JsonValue::Type::Object:
        w.u32_(u32(v.object.size()));
        for (const auto& [k, e] : v.object) { w.str(k); writeJson(w, e); }
        break;
    }
}
bool readJson(Reader& r, JsonValue& v, u32 depth) {
    if (depth > kMaxDepth) return r.ok = false;
    const u8 t = r.u8_();
    if (!r.ok || t > u8(JsonValue::Type::Object)) return r.ok = false;
    v.type = JsonValue::Type(t);
    switch (v.type) {
    case JsonValue::Type::Null: break;
    case JsonValue::Type::Bool: v.boolean = r.u8_() != 0; break;
    case JsonValue::Type::Number: v.number = r.f64_(); break;
    case JsonValue::Type::String: v.string = r.str(); break;
    case JsonValue::Type::Array: {
        const u32 n = r.count(1);
        v.array.resize(n);
        for (u32 i = 0; i < n && r.ok; ++i) readJson(r, v.array[i], depth + 1);
        break;
    }
    case JsonValue::Type::Object: {
        const u32 n = r.count(5);
        v.object.resize(n);
        for (u32 i = 0; i < n && r.ok; ++i) {
            v.object[i].first = r.str();
            readJson(r, v.object[i].second, depth + 1);
        }
        break;
    }
    }
    return r.ok;
}

// Same rule as Document::buildNode: every key except these is a property (bindings live there).
bool structuralKey(std::string_view k) {
    return k == "class" || k == "id" || k == "children" || k == "on" || k == "classes" || k == "style" ||
           k == "items" || k == "tabIndex" || k == "a11yName" || k == "image" || k == "step";
}
bool isBinding(const JsonValue& v) { return v.isString() && v.string.find('{') != std::string::npos; }

struct Walk {
    std::vector<std::string> bindings;
    std::set<std::string> classes;
    u32 nodes = 0;
    bool walk(const JsonValue& n, u32 depth) {
        if (!n.isObject() || depth > kMaxDepth) return false;
        ++nodes;
        const JsonValue* c = n.get("class");
        classes.insert(c && c->isString() ? c->string : std::string("panel"));
        for (const auto& [k, v] : n.object)
            if (!structuralKey(k) && isBinding(v) &&
                std::find(bindings.begin(), bindings.end(), v.string) == bindings.end())
                bindings.push_back(v.string);
        if (const JsonValue* ch = n.get("children"); ch && ch->isArray())
            for (const JsonValue& k : ch->array)
                if (!walk(k, depth + 1)) return false;
        return true;
    }
};

// ---- style sheet ------------------------------------------------------------------------------------------------
void writeLength(Writer& w, const Length& l) { w.f32_(l.value); w.u8_(u8(l.unit)); }
bool readLength(Reader& r, Length& l) {
    l.value = r.f32_();
    const u8 u = r.u8_();
    if (u > u8(Unit::Fr)) return r.ok = false;
    l.unit = Unit(u);
    return r.ok;
}
void writeValue(Writer& w, const PropValue& v) {
    w.u8_(u8(v.type));
    w.u8_(v.b ? 1 : 0);
    w.u8_(v.e);
    writeLength(w, v.len);
    w.f32_(v.num);
    w.f32_(v.color.r); w.f32_(v.color.g); w.f32_(v.color.b); w.f32_(v.color.a);
    w.u8_(v.trackCount);
    for (u8 i = 0; i < v.trackCount; ++i) writeLength(w, v.tracks[i]);
}
bool readValue(Reader& r, PropValue& v) {
    const u8 t = r.u8_();
    if (t > u8(PropType::Tracks)) return r.ok = false;
    v.type = PropType(t);
    v.b = r.u8_() != 0;
    v.e = r.u8_();
    readLength(r, v.len);
    v.num = r.f32_();
    v.color.r = r.f32_(); v.color.g = r.f32_(); v.color.b = r.f32_(); v.color.a = r.f32_();
    v.trackCount = r.u8_();
    if (v.trackCount > kMaxGridTracks) return r.ok = false;
    for (u8 i = 0; i < v.trackCount && r.ok; ++i) readLength(r, v.tracks[i]);
    return r.ok;
}
void writeSheet(Writer& w, const StyleSheet& s) {
    w.raw(kStyleMagic, 4);
    w.u32_(kCookedVersion);
    w.u32_(u32(s.rules().size()));
    for (const StyleRule& rule : s.rules()) {
        w.u32_(rule.specificity);
        w.u32_(rule.order);
        w.u8_(rule.root ? 1 : 0);
        w.u32_(u32(rule.chain.size()));
        for (const SimpleSelector& sel : rule.chain) {
            w.u32_(sel.typeHash);
            w.u32_(sel.idHash);
            w.u8_(sel.classCount);
            for (u8 i = 0; i < sel.classCount; ++i) w.u32_(sel.classes[i]);
            w.u32_(sel.states);
            w.u8_(u8(sel.combinator));
        }
        w.u32_(u32(rule.decls.size()));
        for (const Declaration& d : rule.decls) {
            w.u32_(u32(d.prop));
            w.u32_(d.customHash);
            writeValue(w, d.value);
            w.str(d.raw);
        }
    }
    w.u32_(u32(s.tokens().size()));
    for (const auto& [h, text] : s.tokens()) { w.u32_(h); w.str(text); }
}
std::expected<StyleSheet, std::string> readSheet(Reader& r) {
    if (!r.magic(kStyleMagic)) return std::unexpected("not a cooked style (bad magic / version)");
    std::vector<StyleRule> rules(r.count(13));
    for (StyleRule& rule : rules) {
        rule.specificity = r.u32_();
        rule.order = r.u32_();
        rule.root = r.u8_() != 0;
        rule.chain.resize(r.count(14));
        for (SimpleSelector& sel : rule.chain) {
            sel.typeHash = r.u32_();
            sel.idHash = r.u32_();
            sel.classCount = r.u8_();
            if (sel.classCount > sel.classes.size()) return std::unexpected("cooked style: bad selector");
            for (u8 i = 0; i < sel.classCount; ++i) sel.classes[i] = r.u32_();
            sel.states = r.u32_();
            const u8 c = r.u8_();
            if (c > u8(SimpleSelector::Combinator::Child)) return std::unexpected("cooked style: bad combinator");
            sel.combinator = SimpleSelector::Combinator(c);
        }
        if (rule.chain.empty() && !rule.root && r.ok) return std::unexpected("cooked style: empty selector");
        rule.decls.resize(r.count(40));
        for (Declaration& d : rule.decls) {
            const u32 p = r.u32_();
            if (p > u32(Prop::Count)) return std::unexpected("cooked style: bad property");
            d.prop = Prop(p);
            d.customHash = r.u32_();
            readValue(r, d.value);
            d.raw = r.str();
        }
        if (!r.ok) break;
    }
    std::vector<std::pair<u32, std::string>> tokens(r.count(8));
    for (auto& [h, text] : tokens) { h = r.u32_(); text = r.str(); }
    if (!r.ok) return std::unexpected("cooked style: truncated or corrupt");
    return StyleSheet::fromParts(std::move(rules), std::move(tokens));
}
} // namespace

// ---- templates / bytecode -------------------------------------------------------------------------------------------
struct CookAccess {
    static void writeExpr(Writer& w, const Expr& e, const Model& m, std::vector<std::string>& names) {
        auto nameIndex = [&](u32 slot) {
            const std::string_view n = m.nameAt(slot);
            for (u32 i = 0; i < names.size(); ++i)
                if (names[i] == n) return i;
            names.emplace_back(n);
            return u32(names.size() - 1);
        };
        w.u32_(u32(e.m_code.size()));
        for (const Expr::Ins& in : e.m_code) {
            w.u8_(u8(in.op));
            if (in.op == Expr::Op::Var) w.u32_(nameIndex(in.slot));
            else if (in.op == Expr::Op::Num) w.f64_(in.num);
        }
    }
    static bool readExpr(Reader& r, Expr& e, const std::vector<u32>& slots) {
        const u32 n = r.count(1);
        if (n == 0 || n > 4096) return r.ok = false;
        e.m_code.resize(n);
        i32 depth = 0, maxDepth = 0;
        for (Expr::Ins& in : e.m_code) {
            const u8 op = r.u8_();
            if (op > u8(Expr::Op::Or)) return r.ok = false;
            in.op = Expr::Op(op);
            if (in.op == Expr::Op::Var) {
                const u32 k = r.u32_();
                if (k >= slots.size()) return r.ok = false;
                in.slot = slots[k];
                if (std::find(e.m_deps.begin(), e.m_deps.end(), in.slot) == e.m_deps.end()) e.m_deps.push_back(in.slot);
            } else if (in.op == Expr::Op::Num) {
                in.num = r.f64_();
            }
            const bool push = in.op == Expr::Op::Num || in.op == Expr::Op::Var;
            const bool unary = in.op == Expr::Op::Neg || in.op == Expr::Op::Not;
            if (!push && depth < (unary ? 1 : 2)) return r.ok = false; // stack underflow
            depth += push ? 1 : unary ? 0 : -1;
            maxDepth = std::max(maxDepth, depth);
            if (maxDepth > 32) return r.ok = false;
        }
        if (depth != 1) return r.ok = false;
        e.m_maxStack = u32(maxDepth);
        e.m_single = (n == 1 && e.m_code[0].op == Expr::Op::Var) ? e.m_code[0].slot : ~0u;
        return r.ok;
    }
    static u32 writeTemplate(Writer& w, const Template& t, const Model& m, std::vector<std::string>& names) {
        u32 ins = 0;
        w.u8_(t.m_twoWay != ~0u ? 1 : 0);
        w.u32_(u32(t.m_parts.size()));
        for (const Template::Part& p : t.m_parts) {
            w.u8_(p.lit ? 1 : 0);
            if (p.lit) { w.str(p.text); continue; }
            w.i32_(p.decimals);
            writeExpr(w, p.expr, m, names);
            ins += u32(p.expr.m_code.size());
        }
        return ins;
    }
    static bool readTemplate(Reader& r, Template& t, const std::vector<u32>& slots) {
        const bool twoWay = r.u8_() != 0;
        t.m_parts.resize(r.count(2));
        for (Template::Part& p : t.m_parts) {
            p.lit = r.u8_() != 0;
            if (p.lit) { p.text = r.str(); continue; }
            p.decimals = r.i32_();
            if (p.decimals < -1 || p.decimals > 9) return r.ok = false;
            if (!readExpr(r, p.expr, slots)) return false;
        }
        if (twoWay) {
            if (t.m_parts.size() != 1 || t.m_parts[0].lit || t.m_parts[0].expr.singleVar() == ~0u) return r.ok = false;
            t.m_twoWay = t.m_parts[0].expr.singleVar();
        }
        return r.ok;
    }
    static std::expected<std::unique_ptr<Document>, DocError> load(UiContext& ui, std::span<const u8> bytes,
                                                                   Model* model, NodeHandle parent) {
        Reader r{bytes};
        auto bad = [](std::string m) { return std::unexpected(DocError{{}, std::move(m)}); };
        if (!r.magic(kDocMagic)) return bad("not a cooked document (bad magic / version)");
        JsonValue tree;
        if (!readJson(r, tree, 0)) return bad("cooked document: corrupt tree");
        std::unique_ptr<Document> d(new Document(ui, model, parent.valid() ? parent : ui.root()));
        // variable names -> model slots
        const u32 nNames = r.count(4);
        std::vector<std::string> names(nNames);
        for (std::string& n : names) n = r.str();
        if (!r.ok) return bad("cooked document: corrupt names");
        if (nNames && !model) return bad("cooked document has bindings and needs a Model");
        std::vector<u32> slots;
        for (const std::string& n : names) slots.push_back(model->slot(n));
        const u32 nT = r.count(6);
        for (u32 i = 0; i < nT && r.ok; ++i) {
            std::string src = r.str();
            Template t;
            if (!readTemplate(r, t, slots)) break;
            d->m_cooked.emplace_back(std::move(src), std::move(t));
        }
        if (!r.ok) return bad("cooked document: corrupt binding bytecode");
        if (r.u8_()) {
            const u32 len = r.count(1);
            if (!r.ok) return bad("cooked document: corrupt style");
            Reader sr{bytes.subspan(r.at, len)};
            auto sheet = readSheet(sr);
            if (!sheet) return bad(sheet.error());
            r.at += len;
            d->m_cookedStyle = std::move(*sheet);
        }
        (void)r.count(1); // classes (registration list), not needed to build
        if (!r.ok) return bad("cooked document: truncated");
        if (d->m_cooked.empty()) d->m_cooked.emplace_back(std::string("\x01"), Template{}); // marks "cooked"
        if (auto b = d->build(tree); !b) return std::unexpected(b.error());
        d->m_cooked.clear();
        return d;
    }
};

std::expected<std::unique_ptr<Document>, DocError> Document::loadCooked(UiContext& ui, std::span<const u8> cooked,
                                                                        Model* model, NodeHandle parent) {
    return CookAccess::load(ui, cooked, model, parent);
}

std::expected<std::vector<u8>, DocError> cookDocument(std::string_view json, CookInfo* info) {
    auto parsed = parseJson(json);
    if (!parsed) return std::unexpected(DocError{{}, parsed.error().message, parsed.error().line, parsed.error().column});
    const JsonValue& doc = *parsed;
    if (!doc.isObject()) return std::unexpected(DocError{"", "document must be an object"});
    if (const JsonValue* ver = doc.get("fuseui"); !ver || !ver->isNumber() || ver->number != 1.0)
        return std::unexpected(DocError{"", "missing or unsupported \"fuseui\" version (expected 1)"});
    const JsonValue* root = doc.get("root");
    if (!root || !root->isObject()) return std::unexpected(DocError{"", "missing \"root\" object"});
    Walk walk;
    if (!walk.walk(*root, 0)) return std::unexpected(DocError{"root", "node must be an object (or tree too deep)"});

    Writer w;
    w.raw(kDocMagic, 4);
    w.u32_(kCookedVersion);
    writeJson(w, doc);
    Model scratch;
    std::vector<std::string> names;
    Writer tw;
    u32 instructions = 0;
    for (const std::string& src : walk.bindings) {
        auto t = Template::compile(src, scratch);
        if (!t) return std::unexpected(DocError{"binding \"" + src + "\"", t.error()});
        tw.str(src);
        instructions += CookAccess::writeTemplate(tw, *t, scratch, names);
    }
    w.u32_(u32(names.size()));
    for (const std::string& n : names) w.str(n);
    w.u32_(u32(walk.bindings.size()));
    w.raw(tw.b.data(), tw.b.size());
    if (const JsonValue* st = doc.get("style"); st && st->isString()) {
        std::vector<StyleDiagnostic> diags;
        auto sheet = StyleSheet::parse(st->string, &diags);
        if (!sheet) return std::unexpected(DocError{"style", sheet.error().message});
        Writer sw;
        writeSheet(sw, *sheet);
        w.u8_(1);
        w.u32_(u32(sw.b.size()));
        w.raw(sw.b.data(), sw.b.size());
    } else {
        w.u8_(0);
    }
    w.u32_(u32(walk.classes.size()));
    for (const std::string& c : walk.classes) w.str(c);
    if (info) {
        info->nodes = walk.nodes;
        info->bindings = u32(walk.bindings.size());
        info->instructions = instructions;
        info->classes.assign(walk.classes.begin(), walk.classes.end());
    }
    return std::move(w.b);
}

std::expected<std::vector<u8>, std::string> cookStyle(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    auto sheet = StyleSheet::parse(css, diags);
    if (!sheet) return std::unexpected("line " + std::to_string(sheet.error().line) + ": " + sheet.error().message);
    Writer w;
    writeSheet(w, *sheet);
    return std::move(w.b);
}

std::expected<StyleSheet, std::string> loadCookedStyle(std::span<const u8> cooked) {
    Reader r{cooked};
    auto s = readSheet(r);
    if (s && r.at != cooked.size()) return std::unexpected("cooked style: trailing bytes");
    return s;
}

std::expected<std::vector<std::string>, std::string> cookedClasses(std::span<const u8> cooked) {
    Reader r{cooked};
    if (!r.magic(kDocMagic)) return std::unexpected("not a cooked document");
    JsonValue tree;
    if (!readJson(r, tree, 0)) return std::unexpected("corrupt tree");
    for (u32 i = 0, n = r.count(4); i < n && r.ok; ++i) (void)r.str();
    // skip templates: re-read them against dummy slots
    std::vector<u32> slots(4096, 0);
    for (u32 i = 0, n = r.count(6); i < n && r.ok; ++i) {
        (void)r.str();
        Template t;
        CookAccess::readTemplate(r, t, slots);
    }
    if (r.u8_()) { const u32 len = r.count(1); r.at += r.ok ? len : 0; }
    std::vector<std::string> out(r.count(4));
    for (std::string& c : out) c = r.str();
    if (!r.ok) return std::unexpected("truncated");
    return out;
}

bool isCookedDocument(std::span<const u8> b) { return b.size() >= 8 && std::memcmp(b.data(), kDocMagic, 4) == 0; }
bool isCookedStyle(std::span<const u8> b) { return b.size() >= 8 && std::memcmp(b.data(), kStyleMagic, 4) == 0; }

std::vector<std::string> registrationList(const std::vector<std::vector<std::string>>& perDoc) {
    static const std::pair<std::string_view, std::string_view> kDeps[] = {
        {"combobox", "popup"}, {"combobox", "menu"}, {"combobox", "menuitem"},
        {"menubutton", "popup"}, {"menubutton", "menu"}, {"menubutton", "menuitem"},
        {"menubar", "menubutton"}, {"menubar", "popup"}, {"menubar", "menu"}, {"menubar", "menuitem"},
        {"menu", "menuitem"}, {"menu", "popup"},
    };
    std::set<std::string> out;
    for (const auto& d : perDoc) out.insert(d.begin(), d.end());
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [from, to] : kDeps)
            if (out.count(std::string(from)) && out.insert(std::string(to)).second) grew = true;
    }
    return {out.begin(), out.end()};
}

std::string registrationSource(const std::vector<std::string>& classes, std::string_view fn) {
    std::string s = "// Generated by fuse_ui_cook: the widget classes the cooked documents use. Do not edit.\n"
                    "#include \"fuse/ui/widgets/widgets.hpp\"\n\n#include <string_view>\n\nnamespace {\n"
                    "constexpr std::string_view kCookedClasses[] = {";
    for (std::size_t i = 0; i < classes.size(); ++i) s += (i ? ", \"" : "\"") + classes[i] + "\"";
    if (classes.empty()) s += "\"panel\"";
    s += "};\n} // namespace\n\n";
    s += "extern const std::size_t " + std::string(fn) + "_count = " + std::to_string(classes.empty() ? 1 : classes.size()) + ";\n";
    s += "const std::string_view* " + std::string(fn) + "_classes() { return kCookedClasses; }\n";
    s += "fuse::ui::u32 " + std::string(fn) + "(fuse::ui::UiContext& ui) {\n"
         "    return fuse::ui::widgets::installClasses(ui, kCookedClasses);\n}\n";
    return s;
}

} // namespace fuse::ui::doc
