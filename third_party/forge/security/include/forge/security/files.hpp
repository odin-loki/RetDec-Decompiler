#pragma once
// FSS-FIO-2: owner-only permission checks on keys, credentials, policies and audit directories.
//
//   if (auto s = security::check_private_file(path); !s) ...                     // secret: 0600, ours, regular, no link
//   security::check_private_file(policy_path, security::file_kind::trusted);    // not writable by others; ours or root
//   security::check_private_dir(dir);                                            // directory, trusted rules
//   security::require_private_file("--token-file", path);                        // per the active policy (below)
//
// The rules are os::fs::check_private (os/fs.hpp):
//   secret   POSIX: regular file, not a symlink, owned by the effective uid, mode & 077 == 0. Windows: not a reparse
//            point, owned by the user (or Administrators for an elevated member), and the DACL allows only the
//            user / owner, SYSTEM and Administrators.
//   trusted  not a symlink, owned by the user or root (Windows: or SYSTEM, Administrators, TrustedInstaller), no
//            write access for group/other (Windows: for any other SID). For files that are not secret but steer
//            security decisions: policy files, trusted public keys, authorized_keys, the audit directory.
//
// require_private_file / require_private_dir apply the rule according to a policy (default: the active one):
//   strict             a failing check throws permission_denied (control FIO-2), audited as deny
//   standard/permissive/none   the file is used; the failure is audited as weaken (a warning in the log)
// Behaviour under profile none is unchanged apart from that warning.
#include <forge/security/audit.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/errors.hpp>
#include <forge/security/policy.hpp>
#include <forge/os/fs.hpp>
#include <string>
#include <string_view>

namespace forge::security {

enum class file_kind { secret, trusted };

inline os::status check_private_file(const std::string& path, file_kind k = file_kind::secret) {
    return os::fs::check_private(path, k == file_kind::secret ? os::fs::private_kind::secret : os::fs::private_kind::trusted, false);
}
inline os::status check_private_dir(const std::string& path, file_kind k = file_kind::trusted) {
    return os::fs::check_private(path, k == file_kind::secret ? os::fs::private_kind::secret : os::fs::private_kind::trusted, true);
}

namespace detail {
inline void require_private_(const policy& p, std::string_view what, const std::string& path, file_kind k, bool dir) {
    auto s = dir ? check_private_dir(path, k) : check_private_file(path, k);
    if (s) return;
    std::string msg = std::string(what) + ": " + s.err.what;
    if (p.prof == profile::strict) {
        audit("fs", decision::deny, "FIO-2", path, msg, name(p.prof));
        throw permission_denied(msg + " (FIO-2: profile strict requires " + (k == file_kind::secret ? "owner-only" : "owner-writable") + " " +
                                (dir ? "directories" : "files") + " here)", "FIO-2");
    }
    audit("fs", decision::weaken, "FIO-2", path, msg + "; used anyway (profile " + name(p.prof) + ")", name(p.prof));
}
} // namespace detail

// Check `path` (a file `what` names, e.g. "--tls-key") per `p`; see the header comment. A missing file is left to
// the caller's own open (which reports it).
inline void require_private_file(const policy& p, std::string_view what, const std::string& path, file_kind k = file_kind::secret) {
    if (path.empty() || !os::exists(path)) return;
    detail::require_private_(p, what, path, k, false);
}
inline void require_private_file(std::string_view what, const std::string& path, file_kind k = file_kind::secret) {
    require_private_file(*active_policy(), what, path, k);
}
inline void require_private_dir(const policy& p, std::string_view what, const std::string& path, file_kind k = file_kind::trusted) {
    if (path.empty() || !os::exists(path)) return;
    detail::require_private_(p, what, path, k, true);
}
inline void require_private_dir(std::string_view what, const std::string& path, file_kind k = file_kind::trusted) {
    require_private_dir(*active_policy(), what, path, k);
}

} // namespace forge::security
