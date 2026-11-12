#pragma once
// Error model: errc codes, forge::error values, and forge_exception (thrown by raise(errc, fmt, ...)).
// Library code throws forge_exception; the C ABI converts it to forge_last_error(), and forge::os returns result<T> instead.
#include <expected>
#include <format>
#include <stdexcept>
#include <string>

namespace forge {

enum class errc { invalid_argument, shape_mismatch, dtype_mismatch, parse_error, io_error,
                  format_error, unsupported, not_found, integrity_error, out_of_memory };

struct error {
    errc code;
    std::string message;
};

template <class T> using result = std::expected<T, error>;

inline std::unexpected<error> fail(errc c, std::string msg) { return std::unexpected(error{c, std::move(msg)}); }

struct forge_exception : std::runtime_error {
    errc code;
    forge_exception(errc c, const std::string& m) : std::runtime_error(m), code(c) {}
};

template <class... A>
[[noreturn]] inline void raise(errc c, std::format_string<A...> f, A&&... a) {
    throw forge_exception(c, std::format(f, std::forward<A>(a)...));
}

template <class T> T unwrap(result<T> r) {
    if (!r) throw forge_exception(r.error().code, r.error().message);
    return std::move(*r);
}

} // namespace forge
