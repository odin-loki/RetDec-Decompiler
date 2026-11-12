#pragma once
// JSON Schema -> GBNF grammar (compact JSON output, no whitespace). Supported: type (object, array, string,
// number, integer, boolean, null, or a list of types), properties (emitted in declaration order; properties
// not in `required` are optional), items, minItems/maxItems (small bounds), enum, const, anyOf/oneOf, $ref to
// #/$defs/* or #/definitions/*. An empty schema accepts any JSON value.
#include <forge/core/error.hpp>
#include <forge/json/json.hpp>
#include <map>
#include <set>
#include <string>

namespace forge::inference {
namespace detail_schema {
inline std::string lit(const std::string& s) {      // GBNF literal for raw bytes s
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c >= 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\x%02X", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
struct conv {
    const json::value& root; std::string out; std::map<std::string, std::string> refs; int n = 0;
    std::string fresh(const std::string& base) { return base + std::to_string(n++); }
    void def(const std::string& name, const std::string& body) { out += name + " ::= " + body + "\n"; }
    std::string visit(const json::value& s) {
        if (s.is_bool()) return s.b() ? "value" : throw_bad("false schema");
        if (!s.is_obj()) return throw_bad("schema must be an object");
        if (auto* r = s.find("$ref")) {
            auto& ref = r->str(); if (auto it = refs.find(ref); it != refs.end()) return it->second;
            std::string key; for (auto pre : {"#/$defs/", "#/definitions/"}) if (ref.rfind(pre, 0) == 0) key = ref.substr(std::string(pre).size());
            const json::value* defs = root.find("$defs"); if (!defs) defs = root.find("definitions");
            const json::value* d = defs ? defs->find(key) : nullptr; if (!d) return throw_bad("unresolved $ref " + ref);
            std::string nm = fresh("ref"); refs[ref] = nm; def(nm, visit(*d)); return nm;
        }
        if (auto* c = s.find("const")) return lit(json::dump(*c));
        if (auto* e = s.find("enum")) { std::string b; for (auto& v : e->arr()) b += (b.empty() ? "" : " | ") + lit(json::dump(v)); return "(" + b + ")"; }
        for (auto k : {"anyOf", "oneOf"}) if (auto* a = s.find(k)) { std::string b; for (auto& v : a->arr()) b += (b.empty() ? "" : " | ") + visit(v); return "(" + b + ")"; }
        auto* t = s.find("type");
        if (!t) return s.find("properties") ? object(s) : "value";
        if (t->is_arr()) { std::string b; for (auto& v : t->arr()) { json::object o = s.obj(); for (auto& [k, x] : o) if (k == "type") x = v; b += (b.empty() ? "" : " | ") + visit(json::value(o)); } return "(" + b + ")"; }
        auto& ty = t->str();
        if (ty == "string") return "string";
        if (ty == "number") return "number";
        if (ty == "integer") return "integer";
        if (ty == "boolean") return "boolean";
        if (ty == "null") return "null";
        if (ty == "object") return object(s);
        if (ty == "array") {
            std::string it = s.find("items") ? visit(s["items"]) : "value";
            std::string nm = fresh("arr"); long lo = long(s.num_or("minItems", 0)), hi = long(s.num_or("maxItems", -1));
            if (lo > 64 || hi > 64) return throw_bad("minItems/maxItems above 64");
            std::string b = "\"[\"";
            if (hi == 0) { def(nm, "\"[]\""); return nm; }
            std::string req; for (long i = 0; i < lo; ++i) req += (i ? " \",\" " : " ") + it;
            if (hi < 0) b += lo ? req + " (\",\" " + it + ")*" : " (" + it + " (\",\" " + it + ")*)?";
            else {
                auto more = [&](long k) { std::string t; for (long i = 0; i < k; ++i) t = "(\",\" " + it + (t.empty() ? "" : " " + t) + ")?"; return t; };
                if (lo == 0) b += " (" + it + (hi > 1 ? " " + more(hi - 1) : "") + ")?";
                else b += req + (hi > lo ? " " + more(hi - lo) : "");
            }
            def(nm, b + " \"]\""); return nm;
        }
        return throw_bad("unsupported type " + ty);
    }
    std::string object(const json::value& s) {
        std::set<std::string> req; if (auto* r = s.find("required")) for (auto& x : r->arr()) req.insert(x.str());
        std::vector<std::pair<std::string, bool>> props; std::string nm = fresh("obj");
        if (auto* p = s.find("properties")) for (auto& [k, v] : p->obj()) { std::string kv = fresh("kv"); def(kv, lit(json::dump(json::value(k))) + " \":\" " + visit(v)); props.push_back({kv, req.count(k) > 0}); }
        if (props.empty()) { def(nm, "\"{\" (string \":\" value (\",\" string \":\" value)*)? \"}\""); return nm; }
        // Required properties are mandatory; optional ones may be skipped. A comma precedes every property
        // after the first one actually emitted, tracked by splitting on whether anything was emitted yet.
        std::string none = "\"}\"", some = "\"}\"";
        for (std::size_t i = props.size(); i-- > 0;) {
            auto& [kv, r] = props[i];
            std::string a = fresh("o"), b = fresh("o");
            def(b, r ? "\",\" " + kv + " " + some : "\",\" " + kv + " " + some + " | " + some);
            def(a, r ? kv + " " + some : kv + " " + some + " | " + none);
            none = a; some = b;
        }
        def(nm, "\"{\" " + none); return nm;
    }
    [[noreturn]] std::string throw_bad(const std::string& m) { raise(errc::invalid_argument, "json schema: {}", m); }
};
}
inline std::string json_schema_to_gbnf(const json::value& schema) {
    detail_schema::conv c{schema, {}, {}, 0};
    std::string r = c.visit(schema);
    return "root ::= " + r + "\n" + c.out +
        "value ::= object | array | string | number | boolean | null\n"
        "object ::= \"{\" (string \":\" value (\",\" string \":\" value)*)? \"}\"\n"
        "array ::= \"[\" (value (\",\" value)*)? \"]\"\n"
        "string ::= \"\\\"\" ([^\"\\\\\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]))* \"\\\"\"\n"
        "integer ::= \"-\"? (\"0\" | [1-9] [0-9]*)\n"
        "number ::= integer (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)?\n"
        "boolean ::= \"true\" | \"false\"\n"
        "null ::= \"null\"\n";
}
}
