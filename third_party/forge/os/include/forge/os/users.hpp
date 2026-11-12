#pragma once
// forge::os users and standard directories: the current user, user lookup by name or id, elevation, and the
// per-user config / cache / data / state / runtime directories (XDG base dirs and user-dirs.dirs on Linux/FreeBSD,
// ~/Library on Apple, Known Folders via SHGetKnownFolderPath on Windows, so OneDrive/GPO redirection is followed). Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>
#if defined(FORGE_OS_POSIX)
#include <grp.h>
#include <pwd.h>
#else
#include <lm.h>     // types only: netapi32 is loaded at run time
#include <sddl.h>
#endif

namespace forge::os {
// Windows has no numeric ids: uid is the RID (last sub-authority) of the account SID, gid the RID of its primary
// group (the account's, usually 513 "None" / "Domain Users", from lookups; the process token's from current_user,
// like getgid), and sid the full SID string ("" on POSIX). lookup_uid(rid) resolves the RID in the current user's
// domain, then among this machine's local accounts.
struct user_info { std::string name, home, shell, full_name; long uid = -1, gid = -1; std::vector<std::string> groups; std::string sid; };

#if defined(FORGE_OS_POSIX)
inline user_info user_from_passwd_(const passwd& p) {
    user_info u; u.name = p.pw_name ? p.pw_name : ""; u.home = p.pw_dir ? p.pw_dir : ""; u.shell = p.pw_shell ? p.pw_shell : "";
    if (p.pw_gecos) { u.full_name = p.pw_gecos; auto c = u.full_name.find(','); if (c != std::string::npos) u.full_name.resize(c); }
    u.uid = long(p.pw_uid); u.gid = long(p.pw_gid);
#if !defined(FORGE_OS_ANDROID)
    std::vector<gid_t> g;
#if defined(__APPLE__)
    int n = 64; std::vector<int> gi(64);
    if (::getgrouplist(p.pw_name, int(p.pw_gid), gi.data(), &n) < 0) { gi.resize(std::size_t(n > 64 ? n : 256)); n = int(gi.size()); ::getgrouplist(p.pw_name, int(p.pw_gid), gi.data(), &n); }
    for (int i = 0; i < n && std::size_t(i) < gi.size(); ++i) g.push_back(gid_t(gi[std::size_t(i)]));
#else
    int n = 64; g.resize(64);
    if (::getgrouplist(p.pw_name, p.pw_gid, g.data(), &n) < 0) { g.resize(std::size_t(n > 64 ? n : 256)); n = int(g.size()); ::getgrouplist(p.pw_name, p.pw_gid, g.data(), &n); }
    g.resize(std::size_t(n) < g.size() ? std::size_t(n) : g.size());
#endif
    for (auto id : g) { std::vector<char> b(4096); group gr{}, *r = nullptr; if (::getgrgid_r(id, &gr, b.data(), b.size(), &r) == 0 && r) u.groups.push_back(r->gr_name); }
#endif
    return u;
}
#else
inline long sid_rid_(PSID s) { UCHAR n = *GetSidSubAuthorityCount(s); return n ? long(*GetSidSubAuthority(s, n - 1u)) : -1; }
inline std::string sid_string_(PSID s) { wchar_t* w = nullptr; if (!ConvertSidToStringSidW(s, &w)) return {}; std::string r = narrow(w); LocalFree(w); return r; }
// The token's user or primary-group SID (cheap: no account-database lookup).
inline std::vector<BYTE> token_sid_(TOKEN_INFORMATION_CLASS c) {
    HANDLE t = nullptr; std::vector<BYTE> out; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return out;
    DWORD n = 0; GetTokenInformation(t, c, nullptr, 0, &n); std::vector<BYTE> b(n ? n : 1);
    if (n && GetTokenInformation(t, c, b.data(), n, &n)) {
        PSID s = c == TokenUser ? reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid : reinterpret_cast<TOKEN_PRIMARY_GROUP*>(b.data())->PrimaryGroup;
        out.assign(static_cast<BYTE*>(s), static_cast<BYTE*>(s) + GetLengthSid(s));
    }
    CloseHandle(t); return out;
}
// Full name, primary group and local groups from netapi32 (loaded at run time, no link library). Filled for
// local accounts; left empty for domain accounts, whose database lives on a domain controller.
inline void user_netinfo_(user_info& u) {
    static HMODULE h = LoadLibraryExW(L"netapi32.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); if (!h) return;
    auto get = reinterpret_cast<decltype(&NetUserGetInfo)>(reinterpret_cast<void*>(GetProcAddress(h, "NetUserGetInfo")));
    auto grp = reinterpret_cast<decltype(&NetUserGetLocalGroups)>(reinterpret_cast<void*>(GetProcAddress(h, "NetUserGetLocalGroups")));
    auto fre = reinterpret_cast<decltype(&NetApiBufferFree)>(reinterpret_cast<void*>(GetProcAddress(h, "NetApiBufferFree")));
    if (!get || !grp || !fre) return;
    std::wstring w = widen(u.name); LPBYTE b = nullptr;
    if (get(nullptr, w.c_str(), 3, &b) == NERR_Success && b) { auto* i = reinterpret_cast<USER_INFO_3*>(b); if (i->usri3_full_name) u.full_name = narrow(i->usri3_full_name); u.gid = long(i->usri3_primary_group_id); }
    else if (b) { fre(b); b = nullptr; }
    if (!b && get(nullptr, w.c_str(), 10, &b) == NERR_Success && b) { auto* i = reinterpret_cast<USER_INFO_10*>(b); if (i->usri10_full_name) u.full_name = narrow(i->usri10_full_name); }   // level 10: any caller
    if (b) fre(b);
    b = nullptr; DWORD n = 0, tot = 0;
    if (grp(nullptr, w.c_str(), 0, 0, &b, MAX_PREFERRED_LENGTH, &n, &tot) == NERR_Success && b) { auto* g = reinterpret_cast<LOCALGROUP_USERS_INFO_0*>(b); for (DWORD i = 0; i < n; ++i) u.groups.push_back(narrow(g[i].lgrui0_name)); }
    if (b) fre(b);
}
// The process token's groups (enabled and deny-only; not the logon session or integrity label), local and domain.
inline std::vector<std::string> token_groups_() {
    std::vector<std::string> out; HANDLE t = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return out;
    DWORD n = 0; GetTokenInformation(t, TokenGroups, nullptr, 0, &n); std::vector<BYTE> b(n ? n : 1);
    if (n && GetTokenInformation(t, TokenGroups, b.data(), n, &n)) {
        auto* g = reinterpret_cast<TOKEN_GROUPS*>(b.data());
        for (DWORD i = 0; i < g->GroupCount; ++i) {
            if ((g->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) continue;
            wchar_t nm[256], dom[256]; DWORD nn = 256, dn = 256; SID_NAME_USE use{};
            if (!LookupAccountSidW(nullptr, g->Groups[i].Sid, nm, &nn, dom, &dn, &use)) continue;
            if (use != SidTypeGroup && use != SidTypeAlias && use != SidTypeWellKnownGroup) continue;
            auto s = narrow(nm); bool dup = false; for (auto& x : out) dup |= x == s; if (!dup) out.push_back(s);
        }
    }
    CloseHandle(t); return out;
}
// Display name of the logged-on user (domain and Microsoft accounts too; secur32 loaded at run time).
inline std::string display_name_() {
    using fn = BOOLEAN(WINAPI*)(int, LPWSTR, PULONG);
    static HMODULE h = LoadLibraryExW(L"secur32.dll", nullptr, 0x00000800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/);
    static fn f = h ? reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(h, "GetUserNameExW"))) : nullptr;
    wchar_t b[512]; ULONG n = 512; return f && f(3 /*NameDisplay*/, b, &n) ? narrow(b) : std::string();
}
// The user account a SID names. home comes from the ProfileList registry key (empty if it never logged on).
inline result<user_info> user_from_sid_(PSID sid, const std::string& what) {
    wchar_t nm[256], dom[256]; DWORD nn = 256, dn = 256; SID_NAME_USE use{};
    if (!LookupAccountSidW(nullptr, sid, nm, &nn, dom, &dn, &use)) return {{}, error{int(GetLastError()), what + ": not found"}};
    if (use != SidTypeUser) return {{}, error{ERROR_NONE_MAPPED, what + ": not a user account"}};
    user_info u; u.name = narrow(nm); u.sid = sid_string_(sid); u.uid = sid_rid_(sid); u.shell = getenv("COMSPEC", "cmd.exe");
    wchar_t home[1024]; DWORD hn = sizeof home;
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + widen(u.sid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ProfileImagePath", RRF_RT_REG_SZ, nullptr, home, &hn) == ERROR_SUCCESS) u.home = narrow(home);
    user_netinfo_(u);
    return {std::move(u), {}};
}
#endif

inline result<user_info> lookup_user(const std::string& name) {
#if defined(FORGE_OS_POSIX)
    std::vector<char> b(16384); passwd p{}, *r = nullptr;
    int rc = ::getpwnam_r(name.c_str(), &p, b.data(), b.size(), &r);
    if (rc || !r) return {{}, error{rc ? rc : ENOENT, "user " + name + ": not found"}};
    return {user_from_passwd_(p), {}};
#else   // "name" or "DOMAIN\name"
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD sn = sizeof sid; wchar_t dom[256]; DWORD dn = 256; SID_NAME_USE use{};
    if (!LookupAccountNameW(nullptr, widen(name).c_str(), sid, &sn, dom, &dn, &use) || use != SidTypeUser) return {{}, error{ERROR_NONE_MAPPED, "user " + name + ": not found"}};
    return user_from_sid_(sid, "user " + name);
#endif
}
inline result<user_info> lookup_uid(long uid) {
#if defined(FORGE_OS_POSIX)
    std::vector<char> b(16384); passwd p{}, *r = nullptr;
    int rc = ::getpwuid_r(uid_t(uid), &p, b.data(), b.size(), &r);
    if (rc || !r) return {{}, error{rc ? rc : ENOENT, "uid " + std::to_string(uid) + ": not found"}};
    return {user_from_passwd_(p), {}};
#else   // uid = RID, under the current user's domain SID, then this machine's account-domain SID
    auto none = [&] { return result<user_info>{{}, error{ERROR_NONE_MAPPED, "uid " + std::to_string(uid) + ": not found"}}; };
    if (uid < 0) return none();
    std::vector<std::pair<std::vector<BYTE>, UCHAR>> doms;   // (SID, sub-authorities of it to keep)
    if (auto me = token_sid_(TokenUser); !me.empty()) { UCHAR n = *GetSidSubAuthorityCount(me.data()); if (n > 1) doms.push_back({me, UCHAR(n - 1)}); }
    wchar_t cn[MAX_COMPUTERNAME_LENGTH + 1]; DWORD cl = MAX_COMPUTERNAME_LENGTH + 1; BYTE d[SECURITY_MAX_SID_SIZE]; DWORD dl = sizeof d; wchar_t dd[256]; DWORD ddl = 256; SID_NAME_USE use{};
    if (GetComputerNameW(cn, &cl) && LookupAccountNameW(nullptr, cn, d, &dl, dd, &ddl, &use) && use == SidTypeDomain)
        doms.push_back({std::vector<BYTE>(d, d + GetLengthSid(d)), *GetSidSubAuthorityCount(d)});
    for (auto& [p, keep] : doms) {
        if (keep >= SID_MAX_SUB_AUTHORITIES) continue;
        BYTE s[SECURITY_MAX_SID_SIZE];
        if (!InitializeSid(s, GetSidIdentifierAuthority(p.data()), BYTE(keep + 1))) continue;
        for (UCHAR i = 0; i < keep; ++i) *GetSidSubAuthority(s, i) = *GetSidSubAuthority(p.data(), i);
        *GetSidSubAuthority(s, keep) = DWORD(uid);
        if (auto r = user_from_sid_(s, "uid " + std::to_string(uid))) return r;
    }
    return none();
#endif
}
inline user_info current_user() {
#if defined(FORGE_OS_POSIX)
    if (auto r = lookup_uid(long(::getuid()))) return r.value;
    user_info u; u.uid = long(::getuid()); u.gid = long(::getgid()); u.name = getenv("USER"); u.home = home_dir(); return u;
#else
    user_info u; wchar_t b[257]; DWORD n = 257;
    if (GetUserNameW(b, &n)) u.name = narrow(b); else u.name = getenv("USERNAME");
    u.home = home_dir(); u.shell = getenv("COMSPEC");
    if (auto s = token_sid_(TokenUser); !s.empty()) { u.uid = sid_rid_(s.data()); u.sid = sid_string_(s.data()); }
    if (auto g = token_sid_(TokenPrimaryGroup); !g.empty()) u.gid = sid_rid_(g.data());
    { user_info a; a.name = u.name; user_netinfo_(a); u.full_name = a.full_name; }
    if (u.full_name.empty()) u.full_name = display_name_();
    u.groups = token_groups_();
    return u;
#endif
}
// Root on POSIX (effective uid 0); an elevated token on Windows.
inline bool is_elevated() {
#if defined(FORGE_OS_POSIX)
    return ::geteuid() == 0;
#else
    HANDLE t = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e{}; DWORD n = 0; bool r = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t); return r;
#endif
}

// ---------------------------------------------------------------- standard per-user directories
enum class user_dir { config, cache, data, state, runtime, documents, downloads, desktop };
#if defined(FORGE_OS_WINDOWS)
// SHGetKnownFolderPath (shell32/ole32 loaded at run time): follows folder redirection (OneDrive, Group Policy).
inline std::string known_folder_(const GUID& id) {
    using get_fn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*); using free_fn = void(WINAPI*)(LPVOID);
    static HMODULE sh = LoadLibraryExW(L"shell32.dll", nullptr, 0x00000800), ole = LoadLibraryExW(L"ole32.dll", nullptr, 0x00000800);
    static get_fn get = sh ? reinterpret_cast<get_fn>(reinterpret_cast<void*>(GetProcAddress(sh, "SHGetKnownFolderPath"))) : nullptr;
    static free_fn fre = ole ? reinterpret_cast<free_fn>(reinterpret_cast<void*>(GetProcAddress(ole, "CoTaskMemFree"))) : nullptr;
    if (!get || !fre) return {};
    PWSTR p = nullptr; HRESULT hr = get(id, 0, nullptr, &p);
    std::string r; if (SUCCEEDED(hr) && p) r = narrow(p); if (p) fre(p); return r;
}
#elif !defined(__APPLE__)
// XDG user directory from user-dirs.dirs (lines like XDG_DESKTOP_DIR="$HOME/Desktop"); "" when not set there.
inline std::string xdg_user_dir_(const std::string& key, const std::string& home, const std::string& config) {
    auto f = read_file(config + "/user-dirs.dirs"); if (!f) return {};
    std::string out; std::size_t i = 0; const std::string& t = f.value;
    while (i < t.size()) {
        auto e = t.find('\n', i); std::string line = t.substr(i, e == std::string::npos ? std::string::npos : e - i); i = e == std::string::npos ? t.size() : e + 1;
        std::size_t k = 0; while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        if (line.compare(k, key.size() + 2, key + "=\"") != 0) continue;
        std::string v; bool closed = false;
        for (std::size_t j = k + key.size() + 2; j < line.size(); ++j) {
            if (line[j] == '\\' && j + 1 < line.size()) { v += line[++j]; continue; }
            if (line[j] == '"') { closed = true; break; }
            v += line[j];
        }
        if (!closed) continue;
        if (v.rfind("$HOME", 0) == 0 && (v.size() == 5 || v[5] == '/')) out = home + v.substr(5);
        else if (!v.empty() && v[0] == '/') out = v;
    }
    return out;
}
#endif
inline std::string user_directory(user_dir d) {
    std::string h = home_dir();
#if defined(FORGE_OS_WINDOWS)
    auto kf = [&](const GUID& id, const std::string& dflt) { auto p = known_folder_(id); return p.empty() ? dflt : p; };
    static const GUID roaming{0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}}, local{0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}},
        docs{0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}}, downloads{0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}},
        desktop{0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}};
    auto ad = [&] { return kf(roaming, getenv("APPDATA", h + "\\AppData\\Roaming")); };
    auto lad = [&] { return kf(local, getenv("LOCALAPPDATA", h + "\\AppData\\Local")); };
    switch (d) {
        case user_dir::config: case user_dir::data: return ad();
        case user_dir::cache: return lad() + "\\Cache";
        case user_dir::state: return lad();
        case user_dir::runtime: return temp_dir();
        case user_dir::documents: return kf(docs, h + "\\Documents");
        case user_dir::downloads: return kf(downloads, h + "\\Downloads");
        case user_dir::desktop: return kf(desktop, h + "\\Desktop");
    }
#elif defined(__APPLE__)
    switch (d) {
        case user_dir::config: case user_dir::data: case user_dir::state: return h + "/Library/Application Support";
        case user_dir::cache: return h + "/Library/Caches";
        case user_dir::runtime: return temp_dir();
        case user_dir::documents: return h + "/Documents";
        case user_dir::downloads: return h + "/Downloads";
        case user_dir::desktop: return h + "/Desktop";
    }
#else
    auto x = [&](const char* var, const std::string& dflt) { auto v = getenv(var); return v.empty() || v[0] != '/' ? dflt : v; };
    // user-dirs.dirs (what xdg-user-dir reads), then the environment variable, then the default
    auto u = [&](const char* var, const std::string& dflt) { auto v = xdg_user_dir_(var, h, x("XDG_CONFIG_HOME", h + "/.config")); return v.empty() ? x(var, dflt) : v; };
    switch (d) {
        case user_dir::config: return x("XDG_CONFIG_HOME", h + "/.config");
        case user_dir::cache: return x("XDG_CACHE_HOME", h + "/.cache");
        case user_dir::data: return x("XDG_DATA_HOME", h + "/.local/share");
        case user_dir::state: return x("XDG_STATE_HOME", h + "/.local/state");
        case user_dir::runtime: return x("XDG_RUNTIME_DIR", temp_dir());
        case user_dir::documents: return u("XDG_DOCUMENTS_DIR", h + "/Documents");
        case user_dir::downloads: return u("XDG_DOWNLOAD_DIR", h + "/Downloads");
        case user_dir::desktop: return u("XDG_DESKTOP_DIR", h + "/Desktop");
    }
#endif
    return h;
}
inline std::string config_dir() { return user_directory(user_dir::config); }
inline std::string cache_dir() { return user_directory(user_dir::cache); }
inline std::string data_dir() { return user_directory(user_dir::data); }
} // namespace forge::os
