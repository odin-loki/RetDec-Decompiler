#pragma once
// FSS FIO-1 / FIO-2 helpers in namespace forge::os (os/fs.hpp re-exports them in os::fs): private-file checks, private
// directories and exclusive owner-only files. Kept apart from fs.hpp so headers that must not pull os::fs into scope
// (forge::core's memory.hpp: its includers may say `using namespace forge::os` and `namespace fs = std::filesystem`)
// can use them.
#include <forge/os/os.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#if defined(FORGE_OS_WINDOWS)
#include <aclapi.h>   // GetNamedSecurityInfoW (check_private)
#include <sddl.h>     // private DACLs for temp files and directories
#endif

namespace forge::os {

// ---------------------------------------------------------------- private files and directories (FSS FIO-1 / FIO-2)
// check_private(path, kind, want_dir): whether `path` may hold a secret (kind secret) or be trusted as configuration
// (kind trusted). Never follows a symlink. The error says what is wrong.
//   secret   POSIX: a regular file (a directory with want_dir), not a symlink, owned by the effective uid, no group or
//            other permission bits (mode & 077 == 0: 0600 / 0700). Windows: not a reparse point; owned by the current
//            user (or by BUILTIN\Administrators when this token is an elevated member: that is who owns what an
//            elevated process creates); every access-allowed ACE (inherit-only ones skipped) names the user, the
//            owner, OWNER RIGHTS, CREATOR OWNER, SYSTEM or Administrators. A NULL DACL fails.
//   trusted  Configuration that is not secret but must not be modifiable by others (policy files, trusted public
//            keys, the audit file's directory): not a symlink; owned by the effective uid or root (Windows: the
//            user, SYSTEM, Administrators or TrustedInstaller); no group/other write (mode & 022 == 0; Windows: no
//            write-type right granted to any other SID).
enum class private_kind { secret, trusted };

#if defined(FORGE_OS_WINDOWS)
namespace detail_priv {
inline std::vector<unsigned char> token_user() {   // TOKEN_USER buffer; empty on failure
    HANDLE t = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return {};
    DWORD n = 0; GetTokenInformation(t, TokenUser, nullptr, 0, &n);
    std::vector<unsigned char> b(n ? n : 1);
    if (!n || !GetTokenInformation(t, TokenUser, b.data(), n, &n)) b.clear();
    CloseHandle(t); return b;
}
inline PSID user_sid(std::vector<unsigned char>& b) { return b.empty() ? nullptr : reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid; }
inline bool is_wk(PSID s, WELL_KNOWN_SID_TYPE t) {
    BYTE b[SECURITY_MAX_SID_SIZE]; DWORD n = sizeof b;
    return s && CreateWellKnownSid(t, nullptr, b, &n) && EqualSid(s, b);
}
inline bool token_member(WELL_KNOWN_SID_TYPE t) {
    BYTE b[SECURITY_MAX_SID_SIZE]; DWORD n = sizeof b; BOOL m = FALSE;
    return CreateWellKnownSid(t, nullptr, b, &n) && CheckTokenMembership(nullptr, b, &m) && m;
}
inline std::string sid_str(PSID s) {
    LPWSTR w = nullptr; if (!s || !ConvertSidToStringSidW(s, &w)) return "?";
    std::string r = narrow(w); LocalFree(w); return r;
}
inline bool is_trusted_installer(PSID s) { return sid_str(s) == "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"; }
// "O:<user>D:P(A;[OICI];FA;;;<user>)(A;..;FA;;;SY)(A;..;FA;;;BA)": the user plus SYSTEM and Administrators, not
// inherited from the parent (protected). Null when the token cannot be read (the caller then creates with defaults).
struct private_sa {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    explicit private_sa(bool dir) {
        auto tu = token_user(); PSID u = user_sid(tu); if (!u) return;
        LPWSTR us = nullptr; if (!ConvertSidToStringSidW(u, &us)) return;
        std::wstring f = dir ? L"OICI" : L"", ws = us; LocalFree(us);
        std::wstring sddl = L"O:" + ws + L"D:P(A;" + f + L";FA;;;" + ws + L")(A;" + f + L";FA;;;SY)(A;" + f + L";FA;;;BA)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) sa.lpSecurityDescriptor = sd;
    }
    ~private_sa() { if (sd) LocalFree(sd); }
    private_sa(const private_sa&) = delete; private_sa& operator=(const private_sa&) = delete;
    SECURITY_ATTRIBUTES* get() { return sd ? &sa : nullptr; }
};
} // namespace detail_priv
#endif

inline status check_private(const std::string& path, private_kind kind = private_kind::secret, bool want_dir = false) {
    const bool secret = kind == private_kind::secret;
    auto bad = [&](const std::string& why) { return status{{}, {EACCES, path + ": " + why}}; };
#if defined(FORGE_OS_POSIX)
    struct ::stat st{};
    if (::lstat(path.c_str(), &st) != 0) return {{}, last_error(path)};
    if (S_ISLNK(st.st_mode)) return bad("is a symbolic link");
    if (want_dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) return bad(want_dir ? "not a directory" : "not a regular file");
    const uid_t me = ::geteuid();
    if (st.st_uid != me && (secret || st.st_uid != 0))
        return bad("owned by uid " + std::to_string(st.st_uid) + ", not by this user (uid " + std::to_string(me) + ")" + (secret ? "" : " or root"));
    char m[8]; std::snprintf(m, sizeof m, "%04o", unsigned(st.st_mode & 07777));
    if (secret && (st.st_mode & 077)) return bad(std::string("mode ") + m + " gives group/other access (want " + (want_dir ? "0700" : "0600") + ")");
    if (!secret && (st.st_mode & 022)) return bad(std::string("mode ") + m + " is writable by group/other");
    return {};
#else
    auto w = widen(path);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return {{}, last_error(path)};
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return bad("is a symbolic link or junction");
    if (want_dir != bool(a & FILE_ATTRIBUTE_DIRECTORY)) return bad(want_dir ? "not a directory" : "not a regular file");
    PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (DWORD e = GetNamedSecurityInfoW(w.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &sd))
        return {{}, {int(e), path + ": GetNamedSecurityInfo: " + error_text(int(e))}};
    struct sd_free { PSECURITY_DESCRIPTOR p; ~sd_free() { if (p) LocalFree(p); } } sd_guard{sd};
    auto tu = detail_priv::token_user(); PSID me = detail_priv::user_sid(tu);
    if (!me) return bad("cannot read this process's user SID");
    using namespace detail_priv;
    const bool admin_owner = is_wk(owner, WinBuiltinAdministratorsSid);
    bool owner_ok = EqualSid(owner, me) || (admin_owner && token_member(WinBuiltinAdministratorsSid));
    if (!secret) owner_ok = owner_ok || admin_owner || is_wk(owner, WinLocalSystemSid) || is_trusted_installer(owner);
    if (!owner_ok) return bad("owned by " + sid_str(owner) + ", not by this user (" + sid_str(me) + ")");
    if (!dacl) return bad("has a NULL DACL (everyone has full access)");
    const ACCESS_MASK write_rights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER | DELETE | FILE_DELETE_CHILD | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr; if (!GetAce(dacl, i, &ace)) continue;
        auto* h = static_cast<ACE_HEADER*>(ace);
        if (h->AceFlags & INHERIT_ONLY_ACE) continue;
        if (h->AceType == ACCESS_DENIED_ACE_TYPE || h->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE || h->AceType == SYSTEM_AUDIT_ACE_TYPE) continue;
        if (h->AceType != ACCESS_ALLOWED_ACE_TYPE) return bad("has an access-allowed ACE of a kind FORGE does not check (type " + std::to_string(h->AceType) + ")");
        auto* aa = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        PSID s = reinterpret_cast<PSID>(&aa->SidStart);
        bool ok = EqualSid(s, me) || EqualSid(s, owner) || is_wk(s, WinLocalSystemSid) || is_wk(s, WinBuiltinAdministratorsSid) ||
                  is_wk(s, WinCreatorOwnerSid) || is_wk(s, WinCreatorOwnerRightsSid) || (!secret && is_trusted_installer(s));
        if (ok) continue;
        if (secret && aa->Mask) return bad("grants access to " + sid_str(s) + " (want only the owner, SYSTEM and Administrators)");
        if (!secret && (aa->Mask & write_rights)) return bad("grants write access to " + sid_str(s));
    }
    return {};
#endif
}

// Create `path` (parents as needed) as a private directory: 0700 / the owner + SYSTEM + Administrators DACL. An
// existing one must be a directory owned by this user and no symlink; POSIX tightens its mode to 0700, Windows refuses
// one that check_private(secret) rejects.
inline status ensure_private_dir(const std::string& path) {
#if defined(FORGE_OS_POSIX)
    struct ::stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno != ENOENT) return {{}, last_error(path)};
        auto parent = std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path();
        if (!parent.empty()) { std::error_code ec; std::filesystem::create_directories(parent, ec); }
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return {{}, last_error("mkdir " + path)};
        if (::lstat(path.c_str(), &st) != 0) return {{}, last_error(path)};
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return {{}, {EACCES, path + ": not a directory (or a symbolic link)"}};
    if (st.st_uid != ::geteuid()) return {{}, {EACCES, path + ": owned by another user"}};
    if ((st.st_mode & 077) && ::chmod(path.c_str(), 0700) != 0) return {{}, last_error("chmod " + path)};
    return {};
#else
    auto w = widen(path);
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) {
        auto parent = std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path();
        if (!parent.empty()) { std::error_code ec; std::filesystem::create_directories(parent, ec); }
        detail_priv::private_sa sa(true);
        if (!CreateDirectoryW(w.c_str(), sa.get()) && GetLastError() != ERROR_ALREADY_EXISTS) return {{}, last_error("mkdir " + path)};
    }
    return check_private(path, private_kind::secret, true);
#endif
}

// Create a new file exclusively (never an existing file, never through a symlink) with owner-only access
// (0600 / the private DACL) and write `data` to it. The file is removed again if the write fails.
inline status write_new_private_file(const std::string& path, std::string_view data) {
#if defined(FORGE_OS_POSIX)
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return {{}, last_error("create " + path)};
    for (std::size_t off = 0; off < data.size();) {
        auto n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { auto e = last_error("write " + path); ::close(fd); ::unlink(path.c_str()); return {{}, e}; }
        off += std::size_t(n);
    }
    if (::close(fd) != 0) { auto e = last_error("close " + path); ::unlink(path.c_str()); return {{}, e}; }
    return {};
#else
    detail_priv::private_sa sa(false);
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, sa.get(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {{}, last_error("create " + path)};
    for (std::size_t off = 0; off < data.size();) {
        DWORD put = 0;
        if (!WriteFile(h, data.data() + off, file::chunk(data.size() - off), &put, nullptr) || !put) {
            auto e = last_error("write " + path); CloseHandle(h); DeleteFileW(widen(path).c_str()); return {{}, e};
        }
        off += put;
    }
    CloseHandle(h); return {};
#endif
}

// A new directory `prefix` + random characters under `base`, created exclusively and private (0700 / the private
// DACL), like os::fs::make_temp_dir(prefix, base).
inline result<std::string> make_private_dir_in(const std::string& base, const std::string& prefix) {
    static const char a[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 16; ++i) {
        unsigned char r[10]; if (!entropy(r, sizeof r)) return {{}, {EIO, "make_private_dir_in: no entropy"}};
        std::string name = prefix; for (unsigned char c : r) name += a[c % (sizeof a - 1)];
        auto p = join(base, name);
#if defined(FORGE_OS_POSIX)
        if (::mkdir(p.c_str(), 0700) == 0) return {p, {}};
        if (errno != EEXIST) return {{}, last_error("mkdir " + p)};
#else
        detail_priv::private_sa sa(true);
        if (CreateDirectoryW(widen(p).c_str(), sa.get())) return {p, {}};
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {{}, last_error("mkdir " + p)};
#endif
    }
    return {{}, error{-1, "make_private_dir_in: could not create a unique directory"}};
}

} // namespace forge::os
