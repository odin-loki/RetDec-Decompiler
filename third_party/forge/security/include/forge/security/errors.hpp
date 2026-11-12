#pragma once
// FORGE Security Standard (docs/security-standard.md), FSS-ERR: the security exception family and its os::error codes.
//
//   security_error (an os::os_exception)
//     policy_violation    bad / unreadable policy, a policy that cannot be honoured     code 0x5EC01
//     integrity_error     a signature or hash check failed                              code 0x5EC02
//     permission_denied   enforce() refused a capability, unsafe path                   code 0x5EC03
//     sandbox_error       lockdown() could not apply a required control                 code 0x5EC04
//
// The codes live in the 0x5EC00-0x5EC0F range of os::error::code (like os::unsupported_code, they cannot clash with
// errno or Win32 values). os_exception::kind is permission_denied for permission_denied and sandbox_violation for
// sandbox_error, so os_exception::script_type() keeps giving the existing script classes PermissionError and
// SandboxViolationError; policy_violation and integrity_error report "OSError" there. The script interpreter maps
// them by code to its classes PolicyViolation / IntegrityError (under SecurityError) (fss_class() gives the FSS name).
#include <forge/os/oserror.hpp>
#include <string>

namespace forge::security {

enum class sec_code : int { policy = 0x5EC01, integrity = 0x5EC02, permission = 0x5EC03, sandbox = 0x5EC04 };
inline constexpr int code_first = 0x5EC00, code_last = 0x5EC0F;
inline bool is_security_code(int c) { return c >= code_first && c <= code_last; }
inline const char* fss_class(sec_code c) {
    switch (c) {
    case sec_code::policy: return "PolicyViolation";
    case sec_code::integrity: return "IntegrityError";
    case sec_code::permission: return "PermissionDenied";
    case sec_code::sandbox: return "SandboxError";
    }
    return "SecurityError";
}
inline os::os_errc kind_of(sec_code c) {
    switch (c) {
    case sec_code::permission: return os::os_errc::permission_denied;
    case sec_code::sandbox: return os::os_errc::sandbox_violation;
    default: return os::os_errc::other;
    }
}
// An os::error carrying a security code (for the non-throwing APIs).
inline os::error make_error(sec_code c, std::string what) { return {int(c), std::move(what), os::err_domain::native}; }
// os::classify() does not know these codes (it reports `other`); this one does.
inline os::os_errc classify(const os::error& e) {
    if (is_security_code(e.code)) return kind_of(sec_code(e.code));
    return os::classify(e);
}

struct security_error : os::os_exception {
    sec_code sc;
    std::string control;   // FSS control ID, e.g. "CAP-1"
    security_error(sec_code c, std::string ctl, const std::string& what)
        : os::os_exception(kind_of(c), int(c), what), sc(c), control(std::move(ctl)) { src = source::sandbox; }
    const char* fss_name() const { return fss_class(sc); }
};
struct policy_violation : security_error {
    explicit policy_violation(const std::string& w, std::string ctl = "POL") : security_error(sec_code::policy, std::move(ctl), w) {}
};
struct integrity_error : security_error {
    explicit integrity_error(const std::string& w, std::string ctl = "INT-4") : security_error(sec_code::integrity, std::move(ctl), w) {}
};
struct permission_denied : security_error {
    explicit permission_denied(const std::string& w, std::string ctl = "CAP-1") : security_error(sec_code::permission, std::move(ctl), w) {}
};
struct sandbox_error : security_error {
    explicit sandbox_error(const std::string& w, std::string ctl = "SBX-4") : security_error(sec_code::sandbox, std::move(ctl), w) {}
};

// Throw the matching exception for an os::error with a security code (other codes: os::throw_if).
inline void throw_if(const os::error& e, const std::string& control = {}) {
    if (!e) return;
    if (!is_security_code(e.code)) os::throw_if(e);
    switch (sec_code(e.code)) {
    case sec_code::policy: throw policy_violation(e.what, control.empty() ? "POL" : control);
    case sec_code::integrity: throw integrity_error(e.what, control.empty() ? "INT-4" : control);
    case sec_code::permission: throw permission_denied(e.what, control.empty() ? "CAP-1" : control);
    case sec_code::sandbox: throw sandbox_error(e.what, control.empty() ? "SBX-4" : control);
    }
    throw security_error(sec_code(e.code), control, e.what);
}

} // namespace forge::security
