#pragma once
// forge::os::registry: Windows registry read / write / enumerate / delete for HKCU, HKLM, HKCR, HKU.
// Keys are "HKCU\\Software\\Vendor\\App". Values: std::string (REG_SZ / REG_EXPAND_SZ, read unexpanded),
// std::int64_t (REG_DWORD / REG_QWORD), std::vector<std::string> (REG_MULTI_SZ), std::vector<std::uint8_t>
// (REG_BINARY and any other raw type). set() keeps the existing type where it can (REG_EXPAND_SZ stays
// expandable, REG_DWORD stays DWORD while the number fits); set_as() writes an explicit type. Every call takes an
// optional view: native, or the 64-/32-bit registry view (KEY_WOW64_64KEY / KEY_WOW64_32KEY) for WOW64 redirection.
// Every call returns unsupported() on other platforms. Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>
#include <variant>

namespace forge::os::registry {
using value = std::variant<std::string, std::int64_t, std::vector<std::string>, std::vector<std::uint8_t>>;
enum class type { none, sz, expand_sz, multi_sz, binary, dword, qword, other };
enum class view { native, wow64_64, wow64_32 };
#if defined(FORGE_OS_WINDOWS)
inline std::wstring w_(const std::string& s) { if (s.empty()) return {}; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0); std::wstring w(std::size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w; }
inline std::string u_(const wchar_t* w, int len = -1) { int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr); std::string s(std::size_t(n > 0 ? n : 0), '\0'); WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr); if (len < 0 && !s.empty()) s.pop_back(); return s; }
inline bool split_(const std::string& key, HKEY& root, std::wstring& sub) {
    auto p = key.find('\\'); std::string r = key.substr(0, p); sub = p == std::string::npos ? L"" : w_(key.substr(p + 1));
    if (r == "HKCU" || r == "HKEY_CURRENT_USER") root = HKEY_CURRENT_USER;
    else if (r == "HKLM" || r == "HKEY_LOCAL_MACHINE") root = HKEY_LOCAL_MACHINE;
    else if (r == "HKCR" || r == "HKEY_CLASSES_ROOT") root = HKEY_CLASSES_ROOT;
    else if (r == "HKU" || r == "HKEY_USERS") root = HKEY_USERS;
    else return false;
    return true;
}
inline error rerr_(LSTATUS e, const std::string& what) { return {int(e), what + ": " + error_text(int(e))}; }
inline REGSAM sam_(view v) { return v == view::wow64_64 ? KEY_WOW64_64KEY : v == view::wow64_32 ? KEY_WOW64_32KEY : 0; }
inline type type_of_(DWORD t) {
    switch (t) { case REG_SZ: return type::sz; case REG_EXPAND_SZ: return type::expand_sz; case REG_MULTI_SZ: return type::multi_sz; case REG_BINARY: return type::binary;
                 case REG_DWORD: return type::dword; case REG_QWORD: return type::qword; default: return type::other; }
}
struct key_ {   // RAII HKEY
    HKEY h = nullptr; ~key_() { if (h) RegCloseKey(h); }
};
inline LSTATUS open_(const std::string& key, view v, REGSAM access, bool create, key_& out, std::string& err) {
    HKEY root; std::wstring sub; if (!split_(key, root, sub)) { err = "registry: bad root in " + key; return ERROR_INVALID_PARAMETER; }
    if (create) return RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, access | sam_(v), nullptr, &out.h, nullptr);
    return RegOpenKeyExW(root, sub.c_str(), 0, access | sam_(v), &out.h);
}
// Raw value bytes and type.
inline LSTATUS query_(HKEY h, const std::wstring& name, DWORD& t, std::vector<std::uint8_t>& buf) {
    for (int i = 0; i < 8; ++i) {
        DWORD size = 0; LSTATUS e = RegQueryValueExW(h, name.c_str(), nullptr, &t, nullptr, &size); if (e != ERROR_SUCCESS) return e;
        buf.assign(std::size_t(size) + 4, 0);   // room for missing terminators
        e = RegQueryValueExW(h, name.c_str(), nullptr, &t, buf.data(), &size);
        if (e == ERROR_MORE_DATA) continue;   // grew between the calls
        if (e == ERROR_SUCCESS) buf.resize(size);
        return e;
    }
    return ERROR_MORE_DATA;
}
inline std::wstring wstr_(const std::vector<std::uint8_t>& b) {   // REG_SZ bytes -> string without trailing NULs
    std::wstring w(b.size() / sizeof(wchar_t), L'\0'); std::memcpy(w.data(), b.data(), w.size() * sizeof(wchar_t));
    auto z = w.find(L'\0'); if (z != std::wstring::npos) w.resize(z); return w;
}
#endif
inline result<value> get(const std::string& key, const std::string& name, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    key_ k; std::string bad; LSTATUS e = open_(key, v, KEY_QUERY_VALUE, false, k, bad);
    if (!bad.empty()) return {{}, {EINVAL, bad}};
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry get " + key + "\\" + name)};
    DWORD t = 0; std::vector<std::uint8_t> b; e = query_(k.h, w_(name), t, b);
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry get " + key + "\\" + name)};
    if (t == REG_DWORD && b.size() >= 4) { DWORD x; std::memcpy(&x, b.data(), 4); return {value(std::int64_t(x)), {}}; }
    if (t == REG_QWORD && b.size() >= 8) { std::uint64_t x; std::memcpy(&x, b.data(), 8); return {value(std::int64_t(x)), {}}; }
    if (t == REG_SZ || t == REG_EXPAND_SZ) { auto w = wstr_(b); return {value(u_(w.c_str(), int(w.size()))), {}}; }
    if (t == REG_MULTI_SZ) {
        std::wstring w(b.size() / sizeof(wchar_t), L'\0'); std::memcpy(w.data(), b.data(), w.size() * sizeof(wchar_t));
        std::vector<std::string> out; std::size_t i = 0;
        while (i < w.size()) { auto z = w.find(L'\0', i); if (z == std::wstring::npos) z = w.size(); if (z == i) break; out.push_back(u_(w.data() + i, int(z - i))); i = z + 1; }
        return {value(std::move(out)), {}};
    }
    return {value(std::move(b)), {}};   // REG_BINARY and other raw types
#else
    (void)key; (void)name; (void)v; return {{}, unsupported("registry")};
#endif
}
// Type of an existing value (type::none when the value does not exist).
inline result<type> value_type(const std::string& key, const std::string& name, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    key_ k; std::string bad; LSTATUS e = open_(key, v, KEY_QUERY_VALUE, false, k, bad);
    if (!bad.empty()) return {{}, {EINVAL, bad}};
    if (e == ERROR_FILE_NOT_FOUND) return {type::none, {}};
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry type " + key)};
    DWORD t = 0; e = RegQueryValueExW(k.h, w_(name).c_str(), nullptr, &t, nullptr, nullptr);
    if (e == ERROR_FILE_NOT_FOUND) return {type::none, {}};
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry type " + key + "\\" + name)};
    return {type_of_(t), {}};
#else
    (void)key; (void)name; (void)v; return {{}, unsupported("registry")};
#endif
}
// Write with an explicit type: sz/expand_sz take a string, multi_sz a string list (no empty items), binary bytes,
// dword/qword an integer (dword must fit in 32 bits unsigned or signed).
inline status set_as(const std::string& key, const std::string& name, const value& val, type t, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    auto mismatch = [&] { return status{{}, {EINVAL, "registry set " + key + "\\" + name + ": value does not match the requested type"}}; };
    std::vector<std::uint8_t> data; DWORD rt = 0;
    if (t == type::sz || t == type::expand_sz) {
        auto* s = std::get_if<std::string>(&val); if (!s) return mismatch();
        auto w = w_(*s); data.resize((w.size() + 1) * sizeof(wchar_t)); std::memcpy(data.data(), w.c_str(), data.size()); rt = t == type::sz ? REG_SZ : REG_EXPAND_SZ;
    } else if (t == type::multi_sz) {
        auto* l = std::get_if<std::vector<std::string>>(&val); if (!l) return mismatch();
        std::wstring w; for (auto& s : *l) { if (s.empty()) return {{}, {EINVAL, "registry set " + key + "\\" + name + ": REG_MULTI_SZ cannot hold an empty string"}}; w += w_(s); w += L'\0'; }
        w += L'\0'; data.resize(w.size() * sizeof(wchar_t)); std::memcpy(data.data(), w.data(), data.size()); rt = REG_MULTI_SZ;
    } else if (t == type::binary) {
        auto* b = std::get_if<std::vector<std::uint8_t>>(&val); if (!b) return mismatch(); data = *b; rt = REG_BINARY;
    } else if (t == type::dword || t == type::qword) {
        auto* n = std::get_if<std::int64_t>(&val); if (!n) return mismatch();
        if (t == type::dword) {
            if (*n < INT32_MIN || *n > std::int64_t(UINT32_MAX)) return {{}, {ERANGE, "registry set " + key + "\\" + name + ": value does not fit REG_DWORD"}};
            DWORD d = DWORD(std::uint32_t(*n)); data.resize(4); std::memcpy(data.data(), &d, 4); rt = REG_DWORD;
        } else { std::uint64_t q = std::uint64_t(*n); data.resize(8); std::memcpy(data.data(), &q, 8); rt = REG_QWORD; }
    } else return {{}, {EINVAL, "registry set " + key + "\\" + name + ": unsupported type"}};
    key_ k; std::string bad; LSTATUS e = open_(key, v, KEY_SET_VALUE, true, k, bad);
    if (!bad.empty()) return {{}, {EINVAL, bad}};
    if (e == ERROR_SUCCESS) e = RegSetValueExW(k.h, w_(name).c_str(), 0, rt, data.empty() ? nullptr : data.data(), DWORD(data.size()));
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry set " + key + "\\" + name)};
    return {};
#else
    (void)key; (void)name; (void)val; (void)t; (void)v; return {{}, unsupported("registry")};
#endif
}
// Write, keeping the existing value's type where the new value allows it.
inline status set(const std::string& key, const std::string& name, const value& val, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    auto cur = value_type(key, name, v); type old = cur ? cur.value : type::none;
    type t = type::binary;
    if (std::holds_alternative<std::string>(val)) t = old == type::expand_sz ? type::expand_sz : type::sz;
    else if (auto* n = std::get_if<std::int64_t>(&val)) t = old == type::dword && *n >= INT32_MIN && *n <= std::int64_t(UINT32_MAX) ? type::dword : type::qword;
    else if (std::holds_alternative<std::vector<std::string>>(val)) t = type::multi_sz;
    return set_as(key, name, val, t, v);
#else
    (void)key; (void)name; (void)val; (void)v; return {{}, unsupported("registry")};
#endif
}
inline status remove_value(const std::string& key, const std::string& name, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    key_ k; std::string bad; LSTATUS e = open_(key, v, KEY_SET_VALUE, false, k, bad);
    if (!bad.empty()) return {{}, {EINVAL, bad}};
    if (e == ERROR_SUCCESS) e = RegDeleteValueW(k.h, w_(name).c_str());
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry delete " + key + "\\" + name)};
    return {};
#else
    (void)key; (void)name; (void)v; return {{}, unsupported("registry")};
#endif
}
inline status remove_key(const std::string& key, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    HKEY root; std::wstring sub; if (!split_(key, root, sub) || sub.empty()) return {{}, {EINVAL, "registry: bad key " + key}};
    key_ k; LSTATUS e = RegOpenKeyExW(root, sub.c_str(), 0, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | sam_(v), &k.h);
    if (e == ERROR_FILE_NOT_FOUND) return {};
    if (e == ERROR_SUCCESS) e = RegDeleteTreeW(k.h, nullptr);
    if (e == ERROR_SUCCESS) e = RegDeleteKeyExW(root, sub.c_str(), sam_(v), 0);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return {{}, rerr_(e, "registry delete " + key)};
    return {};
#else
    (void)key; (void)v; return {{}, unsupported("registry")};
#endif
}
struct listing { std::vector<std::string> subkeys, values; };
inline result<listing> list(const std::string& key, view v = view::native) {
#if defined(FORGE_OS_WINDOWS)
    key_ k; std::string bad; LSTATUS e = open_(key, v, KEY_READ, false, k, bad);
    if (!bad.empty()) return {{}, {EINVAL, bad}};
    if (e != ERROR_SUCCESS) return {{}, rerr_(e, "registry open " + key)};
    listing l; std::vector<wchar_t> name(16384);
    for (DWORD i = 0;; ++i) { DWORD n = 16384; e = RegEnumKeyExW(k.h, i, name.data(), &n, nullptr, nullptr, nullptr, nullptr); if (e != ERROR_SUCCESS) break; l.subkeys.push_back(u_(name.data(), int(n))); }
    if (e != ERROR_NO_MORE_ITEMS) return {{}, rerr_(e, "registry list " + key)};
    for (DWORD i = 0;; ++i) { DWORD n = 16384; e = RegEnumValueW(k.h, i, name.data(), &n, nullptr, nullptr, nullptr, nullptr); if (e != ERROR_SUCCESS) break; l.values.push_back(u_(name.data(), int(n))); }
    if (e != ERROR_NO_MORE_ITEMS) return {{}, rerr_(e, "registry list " + key)};
    return {std::move(l), {}};
#else
    (void)key; (void)v; return {{}, unsupported("registry")};
#endif
}
} // namespace forge::os::registry
