#pragma once
// FSS-CAP-2 / CAP-3: untrusted paths.
//
//   check_relative(rel)            validate an untrusted relative path; returns its normalised segments or an error
//   safe_join(root, rel)           root + rel, refusing anything that could leave root (throws permission_denied)
//   try_safe_join(root, rel)       same, as os::result<std::string>
//   canonical_within(path, roots)  the canonical path (symlinks resolved, `..` folded) if it lies inside one of the
//                                  roots, else std::nullopt
//
// The same rules apply on every OS (P5), so a path accepted on Linux is also safe on Windows:
//   * both '/' and '\' separate segments; NUL and control characters are refused;
//   * absolute paths are refused: a leading separator, a drive letter (C:, C:\), UNC (\\server) and the device
//     namespaces \\?\ and \\.\ ;
//   * ':' anywhere is refused (NTFS alternate data streams "file:stream", drive-relative "C:x");
//   * Windows reserved device names in any segment (CON, PRN, AUX, NUL, COM0-9, LPT0-9, the superscript-digit forms
//     COM¹/LPT², CONIN$, CONOUT$, CLOCK$), with or without an extension, any case;
//   * segments ending in '.' or ' ' (Windows strips them, so "a." would alias "a"), and segments of three or more dots;
//   * ".." may only fold inside the path: "a/../b" is "b", "../x" and "a/../../x" are refused.
// safe_join also resolves symlinks (canonical_within of the result against root), so a link inside root that points
// outside is refused. This is a check, not a handle: a file system that changes between the check and the open (a
// link swapped in by another process) is outside what a path check can stop; the OS sandbox (lockdown) backs it up.
#include <forge/security/errors.hpp>
#include <forge/os/fs.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::security {

namespace detail {
inline bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
// Windows reserved device name, judged on the part before the first '.' with trailing spaces removed.
inline bool reserved_device(std::string_view seg) {
    auto base = seg.substr(0, seg.find('.'));
    while (!base.empty() && base.back() == ' ') base.remove_suffix(1);
    for (const char* n : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"}) if (ieq(base, n)) return true;
    if (base.size() >= 4 && (ieq(base.substr(0, 3), "COM") || ieq(base.substr(0, 3), "LPT"))) {
        auto d = base.substr(3);
        if (d.size() == 1 && d[0] >= '0' && d[0] <= '9') return true;
        if (d == "\xC2\xB9" || d == "\xC2\xB2" || d == "\xC2\xB3") return true;   // ¹ ² ³ (UTF-8)
    }
    return false;
}
inline std::string native_join(std::string root, const std::vector<std::string>& segs) {
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    for (auto& s : segs) {
        if (!root.empty() && root.back() != '/' && root.back() != '\\') root += sep;
        root += s;
    }
    return root;
}
inline std::string lower_if_windows(std::string s) {
#if defined(_WIN32)
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
#endif
    return s;
}
} // namespace detail

// Validate an untrusted relative path; on success returns the normalised segments (possibly empty for "." or "a/..").
inline os::result<std::vector<std::string>> check_relative(std::string_view rel) {
    auto bad = [&](std::string why) -> os::result<std::vector<std::string>> {
        return {{}, make_error(sec_code::permission, "unsafe relative path: " + why)};
    };
    if (rel.empty()) return bad("empty");
    for (unsigned char c : rel) {
        if (c == 0) return bad("NUL byte");
        if (c < 0x20 || c == 0x7f) return bad("control character");
    }
    if (rel.front() == '/' || rel.front() == '\\') return bad("absolute, UNC or device path");
    if (rel.find(':') != rel.npos) return bad("':' (drive letter or alternate data stream)");
    std::vector<std::string> out;
    std::size_t b = 0;
    while (b <= rel.size()) {
        auto e = rel.find_first_of("/\\", b); if (e == rel.npos) e = rel.size();
        std::string_view seg = rel.substr(b, e - b);
        b = e + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (out.empty()) return bad("'..' leaves the root");
            out.pop_back(); continue;
        }
        if (seg.find_first_not_of('.') == seg.npos) return bad("dot-only segment");
        if (seg.back() == '.' || seg.back() == ' ') return bad("segment ends in '.' or ' '");
        if (detail::reserved_device(seg)) return bad("reserved device name '" + std::string(seg) + "'");
        out.emplace_back(seg);
    }
    return {std::move(out), {}};
}

// The canonical form of `path` (absolute, symlinks of the existing prefix resolved, `.`/`..` folded) if it lies inside
// one of `roots` (also canonicalised), else nullopt. Refuses NUL and the \\?\ / \\.\ device namespaces outright.
inline std::optional<std::string> canonical_within(std::string_view path, const std::vector<std::string>& roots) {
    namespace sfs = std::filesystem;
    if (path.empty() || path.find('\0') != path.npos) return std::nullopt;
    auto device_ns = [](std::string_view p) {
        return p.size() >= 4 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') && (p[2] == '?' || p[2] == '.') && (p[3] == '\\' || p[3] == '/');
    };
    if (device_ns(path)) return std::nullopt;
    auto canon = [](std::string_view p) -> std::optional<sfs::path> {
        std::error_code ec;
        auto a = sfs::absolute(os::fs::u8path(std::string(p)), ec); if (ec) return std::nullopt;
        auto c = sfs::weakly_canonical(a, ec); if (ec) return std::nullopt;
        return c.lexically_normal();
    };
    auto cp = canon(path); if (!cp) return std::nullopt;
    auto parts = [](const sfs::path& p) {
        std::vector<std::string> v;
        for (const auto& x : p) { auto s = detail::lower_if_windows(os::fs::u8str(x)); if (!s.empty()) v.push_back(s); }
        return v;
    };
    auto pp = parts(*cp);
    for (auto& r : roots) {
        if (r.empty() || r.find('\0') != r.npos || device_ns(r)) continue;
        auto cr = canon(r); if (!cr) continue;
        auto rp = parts(*cr);
        // a trailing separator element ("" or "/") is dropped by parts(); compare whole components
        while (!rp.empty() && (rp.back() == "/" || rp.back() == "\\")) rp.pop_back();
        if (rp.size() <= pp.size() && std::equal(rp.begin(), rp.end(), pp.begin())) return os::fs::u8str(*cp);
    }
    return std::nullopt;
}

inline os::result<std::string> try_safe_join(std::string_view root, std::string_view rel) {
    auto segs = check_relative(rel);
    if (!segs) return {{}, segs.err};
    auto joined = detail::native_join(std::string(root), segs.value);
    auto c = canonical_within(joined, {std::string(root)});
    if (!c) return {{}, make_error(sec_code::permission, "unsafe relative path: '" + std::string(rel) + "' resolves outside the root")};
    return {*c, {}};
}
// Throws permission_denied (control CAP-3) when rel is unsafe.
inline std::string safe_join(std::string_view root, std::string_view rel) {
    auto r = try_safe_join(root, rel);
    if (!r) throw permission_denied(r.err.what, "CAP-3");
    return std::move(r.value);
}

} // namespace forge::security
