#include "fuse/ui/doc/document.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fuse::ui::doc {

// ---- Model ---------------------------------------------------------------------------------------------------------
u32 Model::find(std::string_view name) const {
    for (u32 i = 0; i < m_vars.size(); ++i)
        if (m_vars[i].name == name) return i;
    return 0xFFFFFFFFu;
}
u32 Model::slot(std::string_view name) {
    const u32 f = find(name);
    if (f != 0xFFFFFFFFu) return f;
    m_vars.emplace_back();
    m_vars.back().name = std::string(name);
    return u32(m_vars.size() - 1);
}
void Model::setNumber(u32 s, double v) {
    Var& var = m_vars[s];
    if (!var.isString && var.number == v && var.version) return;
    var.isString = false;
    var.number = v;
    var.version = ++m_version;
}
void Model::setString(u32 s, std::string_view v) {
    Var& var = m_vars[s];
    if (var.isString && var.text == v) return;
    var.isString = true;
    var.text.assign(v);
    double d = 0.0;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), d);
    var.number = r.ec == std::errc{} ? d : 0.0;
    var.version = ++m_version;
}
double Model::number(std::string_view name) const {
    const u32 f = find(name);
    return f == 0xFFFFFFFFu ? 0.0 : m_vars[f].number;
}
std::string_view Model::string(std::string_view name) const {
    const u32 f = find(name);
    return f == 0xFFFFFFFFu ? std::string_view{} : std::string_view(m_vars[f].text);
}

// ---- Expr ----------------------------------------------------------------------------------------------------------
namespace {
struct ExprParser {
    std::string_view s;
    std::size_t i = 0;
    Model& model;
    std::vector<Expr::Ins>& code;
    std::vector<u32>& deps;
    std::string err;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
    bool eat(std::string_view t) {
        ws();
        if (s.substr(i, t.size()) == t) {
            i += t.size();
            return true;
        }
        return false;
    }
    bool fail(std::string m) {
        if (err.empty()) err = std::move(m) + " at offset " + std::to_string(i);
        return false;
    }
    bool primary() {
        ws();
        if (i >= s.size()) return fail("expected a value");
        const char c = s[i];
        if (c == '(') {
            ++i;
            if (!orExpr()) return false;
            if (!eat(")")) return fail("expected ')'");
            return true;
        }
        if (c == '-') { ++i; if (!primary()) return false; code.push_back({Expr::Op::Neg}); return true; }
        if (c == '!') { ++i; if (!primary()) return false; code.push_back({Expr::Op::Not}); return true; }
        if ((c >= '0' && c <= '9') || c == '.') {
            std::size_t j = i;
            while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.' || s[j] == 'e' || s[j] == 'E')) ++j;
            double v = 0;
            const auto r = std::from_chars(s.data() + i, s.data() + j, v);
            if (r.ec != std::errc{}) return fail("bad number");
            i = j;
            code.push_back({Expr::Op::Num, 0, v});
            return true;
        }
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            std::size_t j = i;
            while (j < s.size() && (s[j] == '_' || s[j] == '.' || (s[j] >= 'a' && s[j] <= 'z') ||
                                    (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9')))
                ++j;
            const std::string_view name = s.substr(i, j - i);
            i = j;
            if (name == "true") { code.push_back({Expr::Op::Num, 0, 1.0}); return true; }
            if (name == "false") { code.push_back({Expr::Op::Num, 0, 0.0}); return true; }
            const u32 slot = model.slot(name);
            code.push_back({Expr::Op::Var, slot});
            bool seen = false;
            for (u32 d : deps) seen |= d == slot;
            if (!seen) deps.push_back(slot);
            return true;
        }
        return fail("unexpected character");
    }
    bool mul() {
        if (!primary()) return false;
        while (true) {
            Expr::Op op;
            if (eat("*")) op = Expr::Op::Mul;
            else if (eat("/")) op = Expr::Op::Div;
            else if (eat("%")) op = Expr::Op::Mod;
            else return true;
            if (!primary()) return false;
            code.push_back({op});
        }
    }
    bool add() {
        if (!mul()) return false;
        while (true) {
            Expr::Op op;
            if (eat("+")) op = Expr::Op::Add;
            else if (eat("-")) op = Expr::Op::Sub;
            else return true;
            if (!mul()) return false;
            code.push_back({op});
        }
    }
    bool cmp() {
        if (!add()) return false;
        while (true) {
            Expr::Op op;
            if (eat("<=")) op = Expr::Op::Le;
            else if (eat(">=")) op = Expr::Op::Ge;
            else if (eat("==")) op = Expr::Op::Eq;
            else if (eat("!=")) op = Expr::Op::Ne;
            else if (eat("<")) op = Expr::Op::Lt;
            else if (eat(">")) op = Expr::Op::Gt;
            else return true;
            if (!add()) return false;
            code.push_back({op});
        }
    }
    bool andExpr() {
        if (!cmp()) return false;
        while (eat("&&")) {
            if (!cmp()) return false;
            code.push_back({Expr::Op::And});
        }
        return true;
    }
    bool orExpr() {
        if (!andExpr()) return false;
        while (eat("||")) {
            if (!andExpr()) return false;
            code.push_back({Expr::Op::Or});
        }
        return true;
    }
};
} // namespace

namespace {
std::atomic<u64> g_exprCompiles{0};
}
u64 Expr::compileCount() { return g_exprCompiles.load(); }

std::expected<Expr, std::string> Expr::compile(std::string_view src, Model& model) {
    ++g_exprCompiles;
    Expr e;
    ExprParser p{src, 0, model, e.m_code, e.m_deps, {}};
    if (!p.orExpr()) return std::unexpected(p.err);
    p.ws();
    if (p.i != src.size()) return std::unexpected("trailing characters in '" + std::string(src) + "'");
    if (e.m_code.size() == 1 && e.m_code[0].op == Op::Var) e.m_single = e.m_code[0].slot;
    // Stack depth (bounded: eval uses a fixed array).
    i32 depth = 0, maxDepth = 0;
    for (const Ins& in : e.m_code) {
        depth += (in.op == Op::Num || in.op == Op::Var) ? 1 : (in.op == Op::Neg || in.op == Op::Not) ? 0 : -1;
        maxDepth = std::max(maxDepth, depth);
    }
    if (maxDepth > 32) return std::unexpected("expression too deep");
    e.m_maxStack = u32(maxDepth);
    return e;
}

double Expr::eval(const Model& m) const {
    double st[32];
    u32 sp = 0;
    for (const Ins& in : m_code) {
        switch (in.op) {
        case Op::Num: st[sp++] = in.num; break;
        case Op::Var: st[sp++] = m.numberAt(in.slot); break;
        case Op::Neg: st[sp - 1] = -st[sp - 1]; break;
        case Op::Not: st[sp - 1] = st[sp - 1] == 0.0 ? 1.0 : 0.0; break;
        default: {
            const double b = st[--sp];
            double& a = st[sp - 1];
            switch (in.op) {
            case Op::Add: a += b; break;
            case Op::Sub: a -= b; break;
            case Op::Mul: a *= b; break;
            case Op::Div: a = b == 0.0 ? 0.0 : a / b; break;
            case Op::Mod: a = b == 0.0 ? 0.0 : std::fmod(a, b); break;
            case Op::Lt: a = a < b; break;
            case Op::Le: a = a <= b; break;
            case Op::Gt: a = a > b; break;
            case Op::Ge: a = a >= b; break;
            case Op::Eq: a = a == b; break;
            case Op::Ne: a = a != b; break;
            case Op::And: a = (a != 0.0 && b != 0.0); break;
            case Op::Or: a = (a != 0.0 || b != 0.0); break;
            default: break;
            }
        }
        }
    }
    return sp ? st[0] : 0.0;
}

// ---- Template ------------------------------------------------------------------------------------------------------
std::expected<Template, std::string> Template::compile(std::string_view src, Model& model) {
    Template t;
    std::size_t i = 0;
    std::string lit;
    auto flush = [&] {
        if (!lit.empty()) {
            Part p;
            p.text = std::move(lit);
            t.m_parts.push_back(std::move(p));
            lit.clear();
        }
    };
    while (i < src.size()) {
        const char c = src[i];
        if (c == '{' && i + 1 < src.size() && src[i + 1] == '{') { lit += '{'; i += 2; continue; }
        if (c == '}' && i + 1 < src.size() && src[i + 1] == '}') { lit += '}'; i += 2; continue; }
        if (c != '{') { lit += c; ++i; continue; }
        const auto close = src.find('}', i);
        if (close == std::string_view::npos) return std::unexpected("unterminated '{' in \"" + std::string(src) + "\"");
        std::string_view body = src.substr(i + 1, close - i - 1);
        flush();
        Part p;
        p.lit = false;
        bool twoWay = false;
        if (!body.empty() && body[0] == '=') { twoWay = true; body.remove_prefix(1); }
        if (const auto colon = body.rfind(':'); colon != std::string_view::npos) {
            int d = 0;
            const auto r = std::from_chars(body.data() + colon + 1, body.data() + body.size(), d);
            if (r.ec != std::errc{} || r.ptr != body.data() + body.size() || d < 0 || d > 9)
                return std::unexpected("bad ':decimals' in \"" + std::string(src) + "\"");
            p.decimals = d;
            body = body.substr(0, colon);
        }
        auto e = Expr::compile(body, model);
        if (!e) return std::unexpected(e.error());
        if (twoWay) {
            if (e->singleVar() == ~0u) return std::unexpected("two-way binding needs a single variable: {=" + std::string(body) + "}");
            t.m_twoWay = e->singleVar();
        }
        p.expr = std::move(*e);
        t.m_parts.push_back(std::move(p));
        i = close + 1;
    }
    flush();
    if (t.m_twoWay != ~0u && t.m_parts.size() != 1) return std::unexpected("two-way binding must be the whole value");
    return t;
}

void Template::render(const Model& m, std::string& out) const {
    out.clear();
    char buf[64];
    for (const Part& p : m_parts) {
        if (p.lit) { out += p.text; continue; }
        if (p.decimals < 0 && p.expr.singleVar() != ~0u && m.isString(p.expr.singleVar())) {
            out += m.stringAt(p.expr.singleVar());
            continue;
        }
        const double v = p.expr.eval(m);
        const int n = p.decimals >= 0 ? std::snprintf(buf, sizeof buf, "%.*f", p.decimals, v)
                                      : std::snprintf(buf, sizeof buf, "%g", v);
        out.append(buf, std::size_t(std::max(0, n)));
    }
}

void Template::deps(std::vector<u32>& out) const {
    for (const Part& p : m_parts)
        if (!p.lit)
            for (u32 d : p.expr.deps()) {
                bool seen = false;
                for (u32 o : out) seen |= o == d;
                if (!seen) out.push_back(d);
            }
}

// ---- Document ------------------------------------------------------------------------------------------------------
namespace {
bool isBindingString(const JsonValue& v) { return v.isString() && v.string.find('{') != std::string::npos; }

void appendCss(std::string& css, const JsonValue& v) {
    if (v.isString()) css += v.string;
    else if (v.isNumber()) {
        char b[32];
        std::snprintf(b, sizeof b, "%gpx", v.number);
        css += b;
    } else if (v.isBool()) css += v.boolean ? "true" : "false";
}

NodeHandle makeNode(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id) {
    namespace w = fuse::ui::widgets;
    if (cls == "slider") return w::slider(ui, parent, 0.f, 1.f, 0.f, 0.f, id);
    if (cls == "progressbar") return w::progressBar(ui, parent, 0.f, id);
    if (cls == "dial") return w::dial(ui, parent, 0.f, 1.f, 0.f, id);
    if (cls == "spinbox") return w::spinBox(ui, parent, 0.f, 100.f, 0.f, 1.f, id);
    if (cls == "scrollbar") return w::scrollBar(ui, parent, true, id);
    if (cls == "lineedit") return w::lineEdit(ui, parent, {}, id);
    auto n = ui.create(cls, parent, id);
    return n ? *n : NodeHandle{};
}

void setNodeText(UiContext& ui, NodeHandle n, std::string_view s) {
    ui.setText(n, s);
    if (ui.registry().get(ui.classOf(n)).name == "lineedit") {
        ui.editBuffer(n).setText(s);
        ui.editBuffer(n).end(false);
    }
}

std::expected<SignalKind, std::string> signalOf(std::string_view s) {
    if (s == "clicked") return SignalKind::Clicked;
    if (s == "toggled") return SignalKind::Toggled;
    if (s == "valueChanged") return SignalKind::ValueChanged;
    if (s == "textChanged") return SignalKind::TextChanged;
    if (s == "submitted") return SignalKind::Submitted;
    if (s == "focusChanged") return SignalKind::FocusChanged;
    if (s == "hoverChanged") return SignalKind::HoverChanged;
    return std::unexpected("unknown signal '" + std::string(s) + "'");
}
} // namespace

std::expected<std::unique_ptr<Document>, DocError> Document::load(UiContext& ui, std::string_view json, Model* model,
                                                                  NodeHandle parent) {
    auto parsed = parseJson(json);
    if (!parsed) return std::unexpected(DocError{{}, parsed.error().message, parsed.error().line, parsed.error().column});
    std::unique_ptr<Document> d(new Document(ui, model, parent.valid() ? parent : ui.root()));
    if (auto r = d->build(*parsed); !r) return std::unexpected(r.error());
    return d;
}

Document::~Document() {
    for (ConnectionHandle c : m_connections) m_ui.disconnect(c);
    if (m_root && m_ui.alive(m_root)) m_ui.destroy(m_root);
}

std::expected<void, DocError> Document::build(const JsonValue& doc) {
    if (!doc.isObject()) return std::unexpected(DocError{"", "document must be an object"});
    if (const JsonValue* ver = doc.get("fuseui"); !ver || !ver->isNumber() || ver->number != 1.0)
        return std::unexpected(DocError{"", "missing or unsupported \"fuseui\" version (expected 1)"});
    if (m_cookedStyle) {
        m_ui.addStyleSheet(std::move(*m_cookedStyle));
        m_cookedStyle.reset();
    } else if (const JsonValue* st = doc.get("style")) {
        if (!st->isString()) return std::unexpected(DocError{"style", "style must be a string"});
        std::vector<StyleDiagnostic> diags;
        if (!m_ui.addStyleSheet(st->string, &diags))
            return std::unexpected(DocError{"style", diags.empty() ? "style parse error" : diags[0].message});
    }
    const JsonValue* root = doc.get("root");
    if (!root || !root->isObject()) return std::unexpected(DocError{"", "missing \"root\" object"});
    std::string path = "root";
    auto r = buildNode(*root, m_parent, path);
    if (!r) {
        if (m_root && m_ui.alive(m_root)) m_ui.destroy(m_root);
        m_root = {};
        return std::unexpected(r.error());
    }
    m_root = *r;
    m_seen = 0;
    for (u32 b = 0; b < m_bindings.size(); ++b) apply(b);
    if (m_model) m_seen = m_model->version();
    return {};
}

std::expected<NodeHandle, DocError> Document::buildNode(const JsonValue& n, NodeHandle parent, std::string& path) {
    if (!n.isObject()) return std::unexpected(DocError{path, "node must be an object"});
    const JsonValue* cls = n.get("class");
    const std::string_view className = cls && cls->isString() ? std::string_view(cls->string) : "panel";
    const JsonValue* idv = n.get("id");
    const std::string_view id = idv && idv->isString() ? std::string_view(idv->string) : std::string_view{};
    if (m_ui.registry().find(className) == 0xFFFFFFFFu)
        return std::unexpected(DocError{path, "unknown class '" + std::string(className) + "'"});
    auto made = makeOrReuse(className, parent, id);
    if (!made) return std::unexpected(DocError{path, made.error().message});
    const NodeHandle node = *made;
    if (!m_root) m_root = node; // so a failure part-way destroys what was built

    for (const auto& [key, v] : n.object) {
        if (key == "class" || key == "id" || key == "children" || key == "on") continue;
        if (key == "classes") {
            if (!v.isArray()) return std::unexpected(DocError{path, "classes must be an array"});
            for (const JsonValue& c : v.array)
                if (c.isString()) {
                    m_ui.addClass(node, c.string);
                    m_nodeClasses.emplace_back(node, c.string);
                }
        } else if (key == "style") {
            if (!v.isObject()) return std::unexpected(DocError{path, "style must be an object of CSS properties"});
            for (const auto& [prop, val] : v.object) {
                std::string css;
                appendCss(css, val);
                if (!m_ui.setCss(node, prop, css))
                    return std::unexpected(DocError{path, "bad style " + prop + ": " + css});
            }
        } else if (key == "items") {
            if (!v.isArray()) return std::unexpected(DocError{path, "items must be an array"});
            std::string items;
            for (std::size_t k = 0; k < v.array.size(); ++k) {
                if (k) items += '\n';
                items += v.array[k].isString() ? v.array[k].string : std::string{};
            }
            m_ui.setText(node, items);
            m_ui.setRange(node, 0.f, f32(v.array.empty() ? 0 : v.array.size() - 1), 1.f);
        } else if (key == "tabIndex" && v.isNumber()) {
            m_ui.setTabIndex(node, i32(v.number));
        } else if (key == "a11yName" && v.isString()) {
            m_ui.setA11y(node, m_ui.role(node), v.string);
        } else if (key == "image" && v.isNumber()) {
            m_ui.setUserData(node, u64(v.number));
        } else if (key == "step" && v.isNumber()) {
            m_ui.setRange(node, m_ui.rangeMin(node), m_ui.rangeMax(node), f32(v.number));
        } else {
            if (auto r = bindProp(node, key, v, path); !r) return std::unexpected(r.error());
        }
    }
    if (const JsonValue* on = n.get("on")) {
        if (!on->isObject()) return std::unexpected(DocError{path, "on must be an object of signal: action"});
        for (const auto& [sig, act] : on->object) {
            auto kind = signalOf(sig);
            if (!kind) return std::unexpected(DocError{path, kind.error()});
            if (!act.isString()) return std::unexpected(DocError{path, "action must be a string"});
            m_actions.push_back(act.string);
            const u32 idx = u32(m_actions.size() - 1);
            Document* self = this;
            m_connections.push_back(m_ui.connect(node, *kind, [self, idx](UiContext&, NodeHandle h, const SignalArgs& a) {
                if (self->m_action) self->m_action(self->m_actions[idx], h, a);
            }));
        }
    }
    if (const JsonValue* ch = n.get("children")) {
        if (!ch->isArray()) return std::unexpected(DocError{path, "children must be an array"});
        for (std::size_t k = 0; k < ch->array.size(); ++k) {
            const std::size_t len = path.size();
            path += "/children[" + std::to_string(k) + "]";
            auto c = buildNode(ch->array[k], node, path);
            if (!c) return c;
            path.resize(len);
        }
    }
    return node;
}

std::expected<void, DocError> Document::bindProp(NodeHandle node, std::string_view prop, const JsonValue& v,
                                                 const std::string& path) {
    Target target;
    if (prop == "text") target = Target::Text;
    else if (prop == "value" || prop == "selected") target = Target::Value;
    else if (prop == "checked") target = Target::Checked;
    else if (prop == "disabled") target = Target::Disabled;
    else if (prop == "visible") target = Target::Visible;
    else if (prop == "min") target = Target::Min;
    else if (prop == "max") target = Target::Max;
    else return std::unexpected(DocError{path, "unknown property '" + std::string(prop) + "'"});

    if (!isBindingString(v)) { // constant
        switch (target) {
        case Target::Text:
            if (!v.isString()) return std::unexpected(DocError{path, "text must be a string"});
            setNodeText(m_ui, node, v.string);
            break;
        case Target::Value:
            if (!v.isNumber()) return std::unexpected(DocError{path, std::string(prop) + " must be a number"});
            m_ui.setValue(node, f32(v.number));
            break;
        case Target::Min: case Target::Max:
            if (!v.isNumber()) return std::unexpected(DocError{path, std::string(prop) + " must be a number"});
            if (target == Target::Min) m_ui.setRange(node, f32(v.number), m_ui.rangeMax(node), m_ui.rangeStep(node));
            else m_ui.setRange(node, m_ui.rangeMin(node), f32(v.number), m_ui.rangeStep(node));
            break;
        case Target::Checked: case Target::Disabled: case Target::Visible:
            if (!v.isBool()) return std::unexpected(DocError{path, std::string(prop) + " must be a boolean"});
            if (target == Target::Checked) m_ui.setChecked(node, v.boolean);
            else if (target == Target::Disabled) m_ui.setDisabled(node, v.boolean);
            else m_ui.setCss(node, "display", v.boolean ? "flex" : "none");
            break;
        }
        return {};
    }
    if (!m_model) return std::unexpected(DocError{path, "binding '" + v.string + "' needs a Model"});
    std::expected<Template, std::string> t = std::unexpected(std::string("binding missing from the cooked document"));
    if (!m_cooked.empty()) {
        for (const auto& [src, tm] : m_cooked)
            if (src == v.string) { t = tm; break; }
    } else {
        t = Template::compile(v.string, *m_model);
    }
    if (!t) return std::unexpected(DocError{path, t.error()});
    if (target != Target::Text && !t->onlyExpr())
        return std::unexpected(DocError{path, std::string(prop) + " binding must be a single {expr}"});
    Binding b{node, target, std::move(*t), {}};
    b.tmpl.deps(b.deps);
    if (const u32 slot = b.tmpl.twoWay(); slot != ~0u) {
        Document* self = this;
        SignalKind kind = SignalKind::ValueChanged;
        if (target == Target::Checked) kind = SignalKind::Toggled;
        else if (target == Target::Text) kind = SignalKind::TextChanged;
        else if (target != Target::Value) return std::unexpected(DocError{path, "two-way binding only on text, value, checked"});
        m_connections.push_back(m_ui.connect(node, kind, [self, slot, kind](UiContext&, NodeHandle, const SignalArgs& a) {
            if (self->m_applying || !self->m_model) return;
            if (kind == SignalKind::ValueChanged) self->m_model->setNumber(slot, a.value);
            else if (kind == SignalKind::Toggled) self->m_model->setNumber(slot, a.flag ? 1.0 : 0.0);
            else self->m_model->setString(slot, a.text);
        }));
    }
    m_bindings.push_back(std::move(b));
    return {};
}

void Document::apply(u32 bi) {
    const Binding& b = m_bindings[bi];
    if (!m_ui.alive(b.node) || !m_model) return;
    m_applying = true;
    if (b.target == Target::Text) {
        b.tmpl.render(*m_model, m_scratch);
        if (m_ui.text(b.node) != m_scratch) setNodeText(m_ui, b.node, m_scratch);
    } else {
        const double v = b.tmpl.onlyExpr()->eval(*m_model);
        switch (b.target) {
        case Target::Value: m_ui.setValue(b.node, f32(v)); break;
        case Target::Min: m_ui.setRange(b.node, f32(v), m_ui.rangeMax(b.node), m_ui.rangeStep(b.node)); break;
        case Target::Max: m_ui.setRange(b.node, m_ui.rangeMin(b.node), f32(v), m_ui.rangeStep(b.node)); break;
        case Target::Checked: m_ui.setChecked(b.node, v != 0.0); break;
        case Target::Disabled: m_ui.setDisabled(b.node, v != 0.0); break;
        case Target::Visible: m_ui.setCss(b.node, "display", v != 0.0 ? "flex" : "none"); break;
        default: break;
        }
    }
    m_applying = false;
}

u32 Document::refresh() {
    if (!m_model || m_model->version() == m_seen) return 0;
    u32 applied = 0;
    for (u32 i = 0; i < m_bindings.size(); ++i) {
        bool dirty = false;
        for (u32 d : m_bindings[i].deps) dirty |= m_model->versionAt(d) > m_seen;
        if (dirty) {
            apply(i);
            ++applied;
        }
    }
    m_seen = m_model->version();
    return applied;
}

std::expected<NodeHandle, DocError> Document::makeOrReuse(std::string_view cls, NodeHandle parent, std::string_view id) {
    if (!id.empty()) {
        for (NodeHandle& h : m_reuse) {
            if (!h || !m_ui.alive(h) || m_ui.idOf(h) != id) continue;
            if (m_ui.registry().get(m_ui.classOf(h)).name != cls) break; // class changed: rebuild
            const NodeHandle n = h;
            h = {};
            m_ui.moveTo(n, parent);
            // reset what the old document set; the new one re-applies its properties
            m_ui.clearInline(n);
            for (auto it = m_nodeClasses.begin(); it != m_nodeClasses.end();) {
                if (it->first == n) { m_ui.removeClass(n, it->second); it = m_nodeClasses.erase(it); }
                else ++it;
            }
            m_ui.setDisabled(n, false);
            ++m_reused;
            return n;
        }
    }
    const NodeHandle node = makeNode(m_ui, cls, parent, id);
    if (!node) return std::unexpected(DocError{{}, "could not create '" + std::string(cls) + "'"});
    return node;
}

std::expected<void, DocError> Document::reload(std::string_view json) {
    auto parsed = parseJson(json);
    if (!parsed) return std::unexpected(DocError{{}, parsed.error().message, parsed.error().line, parsed.error().column});
    // Save per-id state (restored on unbound rebuilt / reused nodes after the new properties are applied).
    struct Saved {
        std::string id, text;
        f32 value;
        bool checked;
    };
    std::vector<Saved> saved;
    std::string focusId;
    m_reuse.clear();
    m_reused = 0;
    if (m_root && m_ui.alive(m_root)) {
        std::vector<NodeHandle> stack{m_root};
        while (!stack.empty()) {
            const NodeHandle h = stack.back();
            stack.pop_back();
            if (const auto id = m_ui.idOf(h); !id.empty()) {
                saved.push_back({std::string(id), std::string(m_ui.text(h)), m_ui.value(h), m_ui.checked(h)});
                if (m_ui.focused() == h) focusId = id;
                m_reuse.push_back(h);
            }
            for (NodeHandle c = m_ui.firstChild(h); c; c = m_ui.nextSibling(c)) stack.push_back(c);
        }
    }
    for (ConnectionHandle c : m_connections) m_ui.disconnect(c);
    m_connections.clear();
    m_bindings.clear();
    m_actions.clear();
    // Park the id nodes under a hidden holder, drop the rest of the old tree.
    NodeHandle holder;
    if (!m_reuse.empty()) {
        holder = makeNode(m_ui, "panel", m_parent, {});
        m_ui.setCss(holder, "display", "none");
        for (NodeHandle h : m_reuse) m_ui.moveTo(h, holder);
        for (NodeHandle h : m_reuse) // anonymous children of kept nodes are rebuilt
            for (NodeHandle c = m_ui.firstChild(h); c;) {
                const NodeHandle next = m_ui.nextSibling(c);
                m_ui.destroy(c);
                c = next;
            }
    }
    bool rootKept = false;
    for (NodeHandle h : m_reuse) rootKept |= h == m_root;
    if (!rootKept && m_root && m_ui.alive(m_root)) m_ui.destroy(m_root);
    m_root = {};
    auto r = build(*parsed);
    if (holder) m_ui.destroy(holder); // ids that left the document
    m_reuse.clear();
    for (auto it = m_nodeClasses.begin(); it != m_nodeClasses.end();)
        it = m_ui.alive(it->first) ? it + 1 : m_nodeClasses.erase(it);
    if (!r) return r;
    for (const Saved& s : saved) {
        const NodeHandle h = m_ui.find(s.id);
        if (!h) continue;
        bool bound = false;
        for (const Binding& b : m_bindings) bound |= b.node == h;
        if (bound) continue; // the model owns bound state
        // user-edited text survives; display text (labels, buttons) takes the new markup
        const std::string_view cls = m_ui.registry().get(m_ui.classOf(h)).name;
        if (cls == "lineedit" || cls == "textedit") setNodeText(m_ui, h, s.text);
        m_ui.setValue(h, s.value);
        m_ui.setChecked(h, s.checked);
    }
    if (!focusId.empty())
        if (const NodeHandle h = m_ui.find(focusId); h && m_ui.focused() != h) m_ui.setFocus(h);
    return {};
}

bool readTextFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace fuse::ui::doc
