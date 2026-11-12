#pragma once
// forge::os networking beyond os::socket: DNS resolution to address strings, network interfaces, TCP port
// probing, URL parsing and a small blocking HTTP/1.1 client for http:// and https://. TLS comes from the OS,
// loaded at run time (the os layer links nothing): Schannel on Windows, Network.framework on Apple, the system
// OpenSSL (libssl.so.3 / .so.1.1 / FreeBSD base) elsewhere. Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>
#include <climits>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#if defined(FORGE_OS_POSIX) && (!defined(FORGE_OS_ANDROID) || __ANDROID_API__ >= 24)   // bionic: getifaddrs from API 24
#define FORGE_NET_IFADDRS 1
#include <ifaddrs.h>
#include <net/if.h>
#elif defined(FORGE_OS_ANDROID)
#include <net/if.h>
#include <sys/ioctl.h>
#elif defined(FORGE_OS_WINDOWS)
#include <iphlpapi.h>   // types only: iphlpapi.dll is loaded at run time
#include <wincrypt.h>   // types only: secur32.dll and crypt32.dll are loaded at run time
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <schannel.h>
#endif
#if defined(__APPLE__)
#include <forge/os/detail.hpp>
#endif

namespace forge::os::net {
struct address { std::string ip; int family = 4; };
// All addresses for host (IPv4 and IPv6, de-duplicated, resolver order).
inline result<std::vector<address>> resolve(const std::string& host) {
    net_init();
    addrinfo hints{}, *r = nullptr; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (int e = ::getaddrinfo(host.c_str(), nullptr, &hints, &r); e) return {{}, {e, "resolve " + host + ": " + ::gai_strerror(e)}};
    std::vector<address> out;
    for (auto* p = r; p; p = p->ai_next) {
        char b[INET6_ADDRSTRLEN] = {};
        const void* src = p->ai_family == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr) : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr);
        if (!::inet_ntop(p->ai_family, src, b, sizeof b)) continue;
        address a{b, p->ai_family == AF_INET ? 4 : 6};
        bool dup = false; for (auto& x : out) dup |= x.ip == a.ip; if (!dup) out.push_back(a);
    }
    ::freeaddrinfo(r);
    return {std::move(out), {}};
}
// Windows names an interface by its friendly name ("Ethernet", "Wi-Fi"); old Android (API < 24) lists IPv4 only.
struct interface_info { std::string name; std::vector<address> addresses; bool up = false, loopback = false; };
inline result<std::vector<interface_info>> interfaces() {
#if defined(FORGE_NET_IFADDRS)
    ifaddrs* ifs = nullptr; if (::getifaddrs(&ifs)) return {{}, last_error("getifaddrs")};
    std::vector<interface_info> out;
    for (auto* p = ifs; p; p = p->ifa_next) {
        interface_info* it = nullptr; for (auto& x : out) if (x.name == p->ifa_name) it = &x;
        if (!it) { out.push_back({p->ifa_name, {}, (p->ifa_flags & IFF_UP) != 0, (p->ifa_flags & IFF_LOOPBACK) != 0}); it = &out.back(); }
        if (!p->ifa_addr || (p->ifa_addr->sa_family != AF_INET && p->ifa_addr->sa_family != AF_INET6)) continue;
        char b[INET6_ADDRSTRLEN] = {}; int fam = p->ifa_addr->sa_family;
        const void* src = fam == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr) : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(p->ifa_addr)->sin6_addr);
        if (::inet_ntop(fam, src, b, sizeof b)) it->addresses.push_back({b, fam == AF_INET ? 4 : 6});
    }
    ::freeifaddrs(ifs);
    return {std::move(out), {}};
#elif defined(FORGE_OS_ANDROID)   // SIOCGIFCONF: interfaces with an IPv4 address
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return {{}, last_error("socket")};
    std::vector<ifreq> rq(64); ifconf ic{};
    for (;;) {
        ic.ifc_len = int(rq.size() * sizeof(ifreq)); ic.ifc_req = rq.data();
        if (::ioctl(fd, SIOCGIFCONF, &ic) < 0) { auto e = last_error("SIOCGIFCONF"); ::close(fd); return {{}, e}; }
        if (std::size_t(ic.ifc_len) < rq.size() * sizeof(ifreq)) break;
        rq.resize(rq.size() * 2);
    }
    std::vector<interface_info> out;
    for (std::size_t i = 0; i < std::size_t(ic.ifc_len) / sizeof(ifreq); ++i) {
        ifreq& r = rq[i]; interface_info* it = nullptr; for (auto& x : out) if (x.name == r.ifr_name) it = &x;
        if (!it) {
            ifreq f{}; std::memcpy(f.ifr_name, r.ifr_name, IFNAMSIZ); short fl = ::ioctl(fd, SIOCGIFFLAGS, &f) == 0 ? f.ifr_flags : 0;
            out.push_back({r.ifr_name, {}, (fl & IFF_UP) != 0, (fl & IFF_LOOPBACK) != 0}); it = &out.back();
        }
        char b[INET_ADDRSTRLEN] = {};
        if (r.ifr_addr.sa_family == AF_INET && ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&r.ifr_addr)->sin_addr, b, sizeof b)) it->addresses.push_back({b, 4});
    }
    ::close(fd);
    return {std::move(out), {}};
#elif defined(FORGE_OS_WINDOWS)
    net_init();
    static HMODULE h = LoadLibraryExW(L"iphlpapi.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); if (!h) return {{}, last_error("iphlpapi.dll")};
    auto gaa = reinterpret_cast<decltype(&GetAdaptersAddresses)>(reinterpret_cast<void*>(GetProcAddress(h, "GetAdaptersAddresses")));
    if (!gaa) return {{}, unsupported("GetAdaptersAddresses")};
    ULONG n = 16384, rc = 0; std::vector<unsigned char> buf;
    for (int k = 0; k < 4; ++k) {   // the list can grow between the size query and the call
        buf.assign(n, 0);
        rc = gaa(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &n);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    std::vector<interface_info> out;
    if (rc == ERROR_NO_DATA) return {std::move(out), {}};
    if (rc != ERROR_SUCCESS) return {{}, error{int(rc), "GetAdaptersAddresses failed"}};
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        interface_info it{a->FriendlyName ? narrow(a->FriendlyName) : std::string(a->AdapterName), {}, a->OperStatus == IfOperStatusUp, a->IfType == IF_TYPE_SOFTWARE_LOOPBACK};
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = u->Address.lpSockaddr; if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
            char b[INET6_ADDRSTRLEN] = {}; int fam = sa->sa_family;
            const void* src = fam == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(sa)->sin_addr) : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr);
            if (::inet_ntop(fam, src, b, sizeof b)) it.addresses.push_back({b, fam == AF_INET ? 4 : 6});
        }
        out.push_back(std::move(it));
    }
    return {std::move(out), {}};
#else
    return {{}, unsupported("network interfaces")};
#endif
}
// True when a TCP connection to host:port succeeds within timeout_ms.
inline bool port_open(const std::string& host, int port, int timeout_ms = 1000) { auto s = socket::connect(host, port, timeout_ms); return bool(s); }

struct url { std::string scheme, host, path = "/", query; int port = 0; };
namespace detail_net {
inline bool alnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline int hexv(unsigned char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
// RFC 9110 token (methods, header names) and field value (no CR, LF or NUL: request splitting / header injection).
inline bool token(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!alnum(c) && !(c && std::strchr("!#$%&'*+-.^_`|~", c))) return false;
    return true;
}
inline bool field_value(std::string_view s) { for (unsigned char c : s) if (c == '\r' || c == '\n' || c == 0) return false; return true; }
// Percent-encode what may not appear raw in a request target (controls, space, non-ASCII, "<>\"\\^`{|}[]#");
// valid %XX escapes are kept, a stray '%' becomes %25.
inline std::string encode_target(std::string_view s, bool query) {
    std::string o; o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool ok = alnum(c) || std::strchr("-._~!$&'()*+,;=:@/", c) || (query && c == '?');
        if (c == '%' && i + 2 < s.size() && hexv(static_cast<unsigned char>(s[i + 1])) >= 0 && hexv(static_cast<unsigned char>(s[i + 2])) >= 0) ok = true;
        if (ok && c) { o += char(c); continue; }
        char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b;
    }
    return o;
}
// RFC 3986 5.2.4.
inline std::string remove_dot_segments(std::string in) {
    std::string out;
    while (!in.empty()) {
        if (in.rfind("../", 0) == 0) in.erase(0, 3);
        else if (in.rfind("./", 0) == 0) in.erase(0, 2);
        else if (in.rfind("/./", 0) == 0) in.replace(0, 3, "/");
        else if (in == "/.") in = "/";
        else if (in.rfind("/../", 0) == 0 || in == "/..") { in = in.size() == 3 ? "/" : in.substr(3); auto k = out.rfind('/'); out.resize(k == std::string::npos ? 0 : k); }
        else if (in == "." || in == "..") in.clear();
        else { auto k = in.find('/', in[0] == '/' ? 1 : 0); out += in.substr(0, k); in = k == std::string::npos ? std::string() : in.substr(k); }
    }
    return out;
}
inline bool valid_host(const std::string& h) {   // reg-name / IPv4 / IPv6 (with %zone): nothing that could split a request line
    if (h.empty()) return false;
    for (unsigned char c : h) if (!alnum(c) && !(c && std::strchr("-._~%:", c))) return false;
    return true;
}
}
// "host", "host:port" or "[v6]:port" (port omitted when it is the scheme default), as a Host header wants it.
inline std::string host_port(const url& u) {
    std::string h = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    bool dflt = (u.scheme == "http" && u.port == 80) || (u.scheme == "https" && u.port == 443);
    return dflt || !u.port ? h : h + ":" + std::to_string(u.port);
}
inline std::string to_string(const url& u) { return u.scheme + "://" + host_port(u) + (u.path.empty() ? "/" : u.path) + (u.query.empty() ? "" : "?" + u.query); }
inline result<url> parse_url(const std::string& s) {
    url u; auto k = s.find("://"); if (k == std::string::npos) return {{}, {EINVAL, "bad url '" + s + "'"}};
    u.scheme = detail_net::lower(s.substr(0, k));
    auto rest = s.substr(k + 3); auto slash = rest.find_first_of("/?#");
    std::string hostport = rest.substr(0, slash); if (slash != std::string::npos) { auto p = rest.substr(slash); auto h = p.find('#'); if (h != std::string::npos) p.resize(h); auto q = p.find('?'); if (q != std::string::npos) { u.query = p.substr(q + 1); p.resize(q); } u.path = p.empty() ? "/" : p; }
    if (auto at = hostport.rfind('@'); at != std::string::npos) hostport = hostport.substr(at + 1);
    std::string port;
    if (!hostport.empty() && hostport[0] == '[') { auto e = hostport.find(']'); if (e == std::string::npos) return {{}, {EINVAL, "bad url '" + s + "'"}}; u.host = hostport.substr(1, e - 1); if (e + 1 < hostport.size()) { if (hostport[e + 1] != ':') return {{}, {EINVAL, "bad url '" + s + "'"}}; port = hostport.substr(e + 2); } }
    else { auto c = hostport.rfind(':'); if (c != std::string::npos) { u.host = hostport.substr(0, c); port = hostport.substr(c + 1); } else u.host = hostport; }
    for (char c : port) if (c < '0' || c > '9') return {{}, {EINVAL, "bad url '" + s + "' (port)"}};
    if (!port.empty()) { u.port = port.size() > 5 ? 0 : std::atoi(port.c_str()); if (u.port <= 0 || u.port > 65535) return {{}, {EINVAL, "bad url '" + s + "' (port)"}}; }
    if (!u.port) u.port = u.scheme == "https" ? 443 : u.scheme == "http" ? 80 : 0;
    if (u.host.empty()) return {{}, {EINVAL, "bad url '" + s + "' (no host)"}};
    if (!detail_net::valid_host(u.host)) return {{}, {EINVAL, "bad url '" + s + "' (host)"}};
    return {u, {}};
}
// Resolve a reference (a redirect Location) against base, RFC 3986 section 5.2: absolute URLs, "//host/p",
// "/p", "p", "../p", "?q".
inline result<url> resolve_reference(const url& base, std::string ref) {
    if (auto h = ref.find('#'); h != std::string::npos) ref.resize(h);
    while (!ref.empty() && (ref.back() == ' ' || ref.back() == '\t')) ref.pop_back();
    auto colon = ref.find(':'), stop = ref.find_first_of("/?");
    bool has_scheme = colon != std::string::npos && colon > 0 && (stop == std::string::npos || colon < stop) && std::isalpha(static_cast<unsigned char>(ref[0]));
    for (std::size_t i = 0; has_scheme && i < colon; ++i) has_scheme = detail_net::alnum(static_cast<unsigned char>(ref[i])) || std::strchr("+-.", ref[i]);
    if (has_scheme) return parse_url(ref);
    if (ref.rfind("//", 0) == 0) return parse_url(base.scheme + ":" + ref);
    url u = base; auto q = ref.find('?'); std::string path = ref.substr(0, q);
    if (path.empty()) { if (q != std::string::npos) u.query = ref.substr(q + 1); return {u, {}}; }
    u.query = q == std::string::npos ? std::string() : ref.substr(q + 1);
    if (path[0] == '/') u.path = detail_net::remove_dot_segments(path);
    else { auto d = base.path.rfind('/'); u.path = detail_net::remove_dot_segments((d == std::string::npos ? "/" : base.path.substr(0, d + 1)) + path); }
    if (u.path.empty() || u.path[0] != '/') u.path = "/" + u.path;
    return {u, {}};
}
struct http_response {
    int status = 0; std::string reason, body;
    // Lower-case names. A repeated header is joined with ", ", except Set-Cookie (cookies contain commas): "\n".
    std::map<std::string, std::string> headers;
    std::vector<std::pair<std::string, std::string>> header_list;   // every header line, in order (lower-case names)
    std::string url;                                                // the URL that answered (after redirects)
    std::vector<std::string> values(const std::string& lower_name) const { std::vector<std::string> v; for (auto& [k, x] : header_list) if (k == lower_name) v.push_back(x); return v; }
};
// timeout_ms bounds the connect, the TLS handshake and each read/write; deadline_ms the whole exchange including
// redirects (<= 0: none). Method, header names and values with CR/LF/NUL or other non-token bytes are rejected
// (EINVAL). Credentials (Authorization, Proxy-Authorization, Cookie) are not forwarded when a redirect leaves the
// origin (scheme, host or port), so an https -> http redirect never carries them in clear text.
// https: the certificate chain and the host name are always verified (TLS 1.2 or later, SNI sent). A failure is
// EPROTO with "certificate verification failed" in the message.
struct http_request {
    std::string method = "GET", url, body; std::map<std::string, std::string> headers; int timeout_ms = 10000, deadline_ms = 300000; std::size_t max_body = 64u << 20; int max_redirects = 5;
    // https: trust these CA certificates (PEM, one or more) INSTEAD of the system store, e.g. a private CA.
    std::string ca_file, ca_pem;
    // TESTS ONLY: skip certificate and host-name verification. The connection is encrypted but not
    // authenticated, so anyone on the path can read and change it. Never set this in production code.
    bool insecure_skip_verify = false;
};
namespace detail_net {
// Decode a chunked body: 1 complete, 0 needs more bytes, -1 malformed, -2 larger than max.
inline int dechunk(const std::string& in, std::size_t i, std::string& out, std::size_t max) {
    out.clear();
    for (;;) {
        auto e = in.find("\r\n", i); if (e == std::string::npos) return in.size() - i > 1024 ? -1 : 0;
        std::string_view line(in.data() + i, e - i); if (auto semi = line.find(';'); semi != std::string_view::npos) line = line.substr(0, semi);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.empty() || line.size() > 15) return -1;
        std::uint64_t n = 0; for (char c : line) { int v = hexv(static_cast<unsigned char>(c)); if (v < 0) return -1; n = n * 16 + std::uint64_t(v); }
        i = e + 2;
        if (!n) { for (;;) { auto t = in.find("\r\n", i); if (t == std::string::npos) return 0; if (t == i) return 1; i = t + 2; } }   // trailers
        if (n > max || out.size() + n > max) return -2;
        if (in.size() < i + n + 2) return 0;
        if (in.compare(i + std::size_t(n), 2, "\r\n")) return -1;
        out.append(in, i, std::size_t(n)); i += std::size_t(n) + 2;
    }
}
inline bool timed_out(int e) {
#if defined(FORGE_OS_WINDOWS)
    return would_block(e) || e == WSAETIMEDOUT;
#else
    return would_block(e) || e == ETIMEDOUT;
#endif
}
// ---------------------------------------------------------------- TLS
// DER bytes of every CERTIFICATE block in a PEM text.
inline std::vector<std::string> pem_certs(std::string_view pem) {
    std::vector<std::string> out; const std::string_view b = "-----BEGIN CERTIFICATE-----", e = "-----END CERTIFICATE-----";
    for (std::size_t i = 0;;) {
        auto s = pem.find(b, i); if (s == std::string_view::npos) break; s += b.size();
        auto t = pem.find(e, s); if (t == std::string_view::npos) break;
        std::string der; unsigned acc = 0; int bits = 0;
        for (std::size_t k = s; k < t; ++k) {
            char c = pem[k];
            int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
            if (v < 0) continue;
            acc = (acc << 6) | unsigned(v); bits += 6;
            if (bits >= 8) { bits -= 8; der += char((acc >> bits) & 0xff); }
        }
        if (!der.empty()) out.push_back(std::move(der));
        i = t + e.size();
    }
    return out;
}
// The caller's trust anchors (ca_pem + ca_file), empty = the system store.
inline result<std::string> trust_pem(const http_request& rq) {
    std::string p = rq.ca_pem;
    if (!rq.ca_file.empty()) { auto f = read_file(rq.ca_file); if (!f) return {{}, f.err}; p += "\n" + *f; }
    if ((!rq.ca_file.empty() || !rq.ca_pem.empty()) && pem_certs(p).empty()) return {{}, errno_error(EINVAL, "https: no certificate in ca_file / ca_pem")};
    return {std::move(p), {}};
}
inline bool ip_literal(const std::string& h) { if (h.find(':') != std::string::npos) return true; in_addr a{}; return ::inet_pton(AF_INET, h.c_str(), &a) == 1; }
inline error tls_timeout() { return {ETIMEDOUT, "https: timed out"}; }
inline error tls_error(std::string what) { return errno_error(EPROTO, "https: " + std::move(what)); }

// OpenSSL >= 1.1 through dlopen. http() uses it on Linux, Android (when the app ships libssl) and FreeBSD; it is
// also usable elsewhere with explicit library paths (the tests run a local TLS server with it on every OS).
// Apple's /usr/lib/libssl aborts the process when loaded, so it is never tried there.
struct ssl_api {
    void *hs = nullptr, *hc = nullptr; bool ok = false; std::string lib;
    int (*OPENSSL_init_ssl)(std::uint64_t, const void*) = nullptr;
    const void* (*TLS_client_method)() = nullptr; const void* (*TLS_server_method)() = nullptr;
    void* (*SSL_CTX_new)(const void*) = nullptr; void (*SSL_CTX_free)(void*) = nullptr;
    long (*SSL_CTX_ctrl)(void*, int, long, void*) = nullptr; void (*SSL_CTX_set_verify)(void*, int, void*) = nullptr;
    int (*SSL_CTX_set_default_verify_paths)(void*) = nullptr; void* (*SSL_CTX_get_cert_store)(const void*) = nullptr;
    int (*SSL_CTX_use_certificate_chain_file)(void*, const char*) = nullptr; int (*SSL_CTX_use_PrivateKey_file)(void*, const char*, int) = nullptr;
    void* (*SSL_new)(void*) = nullptr; void (*SSL_free)(void*) = nullptr; void (*SSL_set_bio)(void*, void*, void*) = nullptr;
    long (*SSL_ctrl)(void*, int, long, void*) = nullptr; int (*SSL_set1_host)(void*, const char*) = nullptr; void* (*SSL_get0_param)(void*) = nullptr;
    int (*SSL_connect)(void*) = nullptr; int (*SSL_accept)(void*) = nullptr;
    int (*SSL_read)(void*, void*, int) = nullptr; int (*SSL_write)(void*, const void*, int) = nullptr;
    int (*SSL_get_error)(const void*, int) = nullptr; int (*SSL_shutdown)(void*) = nullptr; long (*SSL_get_verify_result)(const void*) = nullptr;
    void* (*BIO_meth_new)(int, const char*) = nullptr; int (*BIO_meth_set_write)(void*, int (*)(void*, const char*, int)) = nullptr;
    int (*BIO_meth_set_read)(void*, int (*)(void*, char*, int)) = nullptr; int (*BIO_meth_set_ctrl)(void*, long (*)(void*, int, long, void*)) = nullptr;
    void* (*BIO_new)(const void*) = nullptr; void (*BIO_set_data)(void*, void*) = nullptr; void* (*BIO_get_data)(void*) = nullptr;
    void (*BIO_set_init)(void*, int) = nullptr; void* (*BIO_new_mem_buf)(const void*, int) = nullptr; int (*BIO_free)(void*) = nullptr;
    void* (*PEM_read_bio_X509)(void*, void*, void*, void*) = nullptr; int (*X509_STORE_add_cert)(void*, void*) = nullptr; void (*X509_free)(void*) = nullptr;
    const char* (*X509_verify_cert_error_string)(long) = nullptr; int (*X509_VERIFY_PARAM_set1_ip_asc)(void*, const char*) = nullptr;
    unsigned long (*ERR_get_error)() = nullptr; void (*ERR_error_string_n)(unsigned long, char*, std::size_t) = nullptr; void (*ERR_clear_error)() = nullptr;
    void* bio_method = nullptr;

    struct sock_bio { socket* s = nullptr; int err = 0; bool eof = false; };
    static void* open_lib(const std::string& n) {
#if defined(FORGE_OS_WINDOWS)
        return reinterpret_cast<void*>(LoadLibraryExW(widen(n).c_str(), nullptr, 0x8 /*LOAD_WITH_ALTERED_SEARCH_PATH*/));
#else
        return ::dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    }
    static void* sym(void* h, const char* n) {
#if defined(FORGE_OS_WINDOWS)
        return h ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), n)) : nullptr;
#else
        return h ? ::dlsym(h, n) : nullptr;
#endif
    }
    static ssl_api load(const std::string& ssl, const std::string& crypto) {
        ssl_api a; a.hc = open_lib(crypto); a.hs = open_lib(ssl); if (!a.hs || !a.hc) return a;
#define FORGE_SSL_(h, n) a.n = reinterpret_cast<decltype(a.n)>(sym(a.h, #n)); if (!a.n) return a
        FORGE_SSL_(hs, OPENSSL_init_ssl); FORGE_SSL_(hs, TLS_client_method); FORGE_SSL_(hs, TLS_server_method); FORGE_SSL_(hs, SSL_CTX_new);
        FORGE_SSL_(hs, SSL_CTX_free); FORGE_SSL_(hs, SSL_CTX_ctrl); FORGE_SSL_(hs, SSL_CTX_set_verify); FORGE_SSL_(hs, SSL_CTX_set_default_verify_paths);
        FORGE_SSL_(hs, SSL_CTX_get_cert_store); FORGE_SSL_(hs, SSL_CTX_use_certificate_chain_file); FORGE_SSL_(hs, SSL_CTX_use_PrivateKey_file);
        FORGE_SSL_(hs, SSL_new); FORGE_SSL_(hs, SSL_free); FORGE_SSL_(hs, SSL_set_bio); FORGE_SSL_(hs, SSL_ctrl); FORGE_SSL_(hs, SSL_set1_host);
        FORGE_SSL_(hs, SSL_get0_param); FORGE_SSL_(hs, SSL_connect); FORGE_SSL_(hs, SSL_accept); FORGE_SSL_(hs, SSL_read); FORGE_SSL_(hs, SSL_write);
        FORGE_SSL_(hs, SSL_get_error); FORGE_SSL_(hs, SSL_shutdown); FORGE_SSL_(hs, SSL_get_verify_result);
        FORGE_SSL_(hc, BIO_meth_new); FORGE_SSL_(hc, BIO_meth_set_write); FORGE_SSL_(hc, BIO_meth_set_read); FORGE_SSL_(hc, BIO_meth_set_ctrl);
        FORGE_SSL_(hc, BIO_new); FORGE_SSL_(hc, BIO_set_data); FORGE_SSL_(hc, BIO_get_data); FORGE_SSL_(hc, BIO_set_init); FORGE_SSL_(hc, BIO_new_mem_buf);
        FORGE_SSL_(hc, BIO_free); FORGE_SSL_(hc, PEM_read_bio_X509); FORGE_SSL_(hc, X509_STORE_add_cert); FORGE_SSL_(hc, X509_free);
        FORGE_SSL_(hc, X509_verify_cert_error_string); FORGE_SSL_(hc, X509_VERIFY_PARAM_set1_ip_asc); FORGE_SSL_(hc, ERR_get_error);
        FORGE_SSL_(hc, ERR_error_string_n); FORGE_SSL_(hc, ERR_clear_error);
#undef FORGE_SSL_
        if (!a.OPENSSL_init_ssl(0, nullptr)) return a;
        // A BIO over os::socket: send() without SIGPIPE, and the socket error kept for timeout reporting.
        a.bio_method = a.BIO_meth_new(0x0400 | 0x7e /*BIO_TYPE_SOURCE_SINK | private index*/, "forge_socket");
        if (!a.bio_method) return a;
        a.BIO_meth_set_write(a.bio_method, &bio_write_); a.BIO_meth_set_read(a.bio_method, &bio_read_); a.BIO_meth_set_ctrl(a.bio_method, &bio_ctrl_);
        a.lib = ssl; a.ok = true; return a;
    }
    // The process-wide instance. The first call decides: explicit paths (tests), else the system libssl.
    static ssl_api& instance(const char* ssl = nullptr, const char* crypto = nullptr) {
        static ssl_api a = [&] {
            if (ssl && crypto) return load(ssl, crypto);
#if defined(FORGE_OS_POSIX) && !defined(__APPLE__)
            static const char* const names[][2] = {{"libssl.so.3", "libcrypto.so.3"}, {"libssl.so.30", "libcrypto.so.30"}, {"libssl.so.1.1", "libcrypto.so.1.1"},
                                                    {"libssl.so.111", "libcrypto.so.111"}, {"libssl.so", "libcrypto.so"}};
            for (auto& n : names) { auto x = load(n[0], n[1]); if (x.ok) return x; }
#endif
            return ssl_api{};
        }();
        return a;
    }
    // Wrap socket s in a BIO and hand it to ssl (which owns the BIO, not d).
    bool attach(void* ssl, sock_bio& d) const {
        void* b = BIO_new(bio_method); if (!b) return false;
        BIO_set_data(b, &d); BIO_set_init(b, 1); SSL_set_bio(ssl, b, b); return true;
    }
    static int bio_write_(void* b, const char* p, int n) {
        auto* d = static_cast<sock_bio*>(instance().BIO_get_data(b));
        long r = d->s->send(p, std::size_t(n)); if (r >= 0) return int(r);
        d->err = sock_errno(); return -1;
    }
    static int bio_read_(void* b, char* p, int n) {
        auto* d = static_cast<sock_bio*>(instance().BIO_get_data(b));
        long r = d->s->recv(p, std::size_t(n)); if (r > 0) return int(r);
        if (r == 0) { d->eof = true; return 0; }
        d->err = sock_errno(); return -1;
    }
    static long bio_ctrl_(void*, int cmd, long, void*) { return cmd == 11 /*BIO_CTRL_FLUSH*/ ? 1 : 0; }
    // The reason the last call on ssl failed.
    error failure(void* ssl, const sock_bio& d, const char* what, bool verified) const {
        if (d.err && timed_out(d.err)) { ERR_clear_error(); return tls_timeout(); }
        if (verified && ssl) if (long v = SSL_get_verify_result(ssl)) { ERR_clear_error(); return tls_error(std::string("certificate verification failed: ") + X509_verify_cert_error_string(v)); }
        if (unsigned long e = ERR_get_error()) { char b[256] = {}; ERR_error_string_n(e, b, sizeof b); ERR_clear_error(); return tls_error(std::string(what) + ": " + b); }
        if (d.err) return {d.err, std::string("https: ") + what + ": " + error_text(d.err)};
        return tls_error(std::string(what) + ": connection closed");
    }
};

#if defined(FORGE_OS_WINDOWS)
// Schannel (secur32) + chain verification (crypt32), both loaded at run time from System32.
struct sspi_api {
    PSecurityFunctionTableW f = nullptr;
    decltype(&CertGetCertificateChain) GetChain = nullptr; decltype(&CertVerifyCertificateChainPolicy) VerifyPolicy = nullptr;
    decltype(&CertFreeCertificateChain) FreeChain = nullptr; decltype(&CertFreeCertificateContext) FreeCert = nullptr;
    decltype(&CertOpenStore) OpenStore = nullptr; decltype(&CertCloseStore) CloseStore = nullptr;
    decltype(&CertAddEncodedCertificateToStore) AddEncoded = nullptr;
    decltype(&CertCreateCertificateChainEngine) CreateEngine = nullptr; decltype(&CertFreeCertificateChainEngine) FreeEngine = nullptr;
    bool ok = false;
    static const sspi_api& get() {
        static sspi_api a = [] {
            sspi_api x;
            HMODULE s = LoadLibraryExW(L"secur32.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/), c = LoadLibraryExW(L"crypt32.dll", nullptr, 0x800);
            if (!s || !c) return x;
            auto init = reinterpret_cast<INIT_SECURITY_INTERFACE_W>(reinterpret_cast<void*>(GetProcAddress(s, "InitSecurityInterfaceW")));
            if (!init || !(x.f = init())) return x;
#define FORGE_CRYPT_(m, n) x.m = reinterpret_cast<decltype(x.m)>(reinterpret_cast<void*>(GetProcAddress(c, n))); if (!x.m) return x
            FORGE_CRYPT_(GetChain, "CertGetCertificateChain"); FORGE_CRYPT_(VerifyPolicy, "CertVerifyCertificateChainPolicy");
            FORGE_CRYPT_(FreeChain, "CertFreeCertificateChain"); FORGE_CRYPT_(FreeCert, "CertFreeCertificateContext");
            FORGE_CRYPT_(OpenStore, "CertOpenStore"); FORGE_CRYPT_(CloseStore, "CertCloseStore"); FORGE_CRYPT_(AddEncoded, "CertAddEncodedCertificateToStore");
            FORGE_CRYPT_(CreateEngine, "CertCreateCertificateChainEngine"); FORGE_CRYPT_(FreeEngine, "CertFreeCertificateChainEngine");
#undef FORGE_CRYPT_
            x.ok = true; return x;
        }();
        return a;
    }
};
// SCH_CREDENTIALS / TLS_PARAMETERS (Windows 10 1809+, needed for TLS 1.3); declared here because older SDKs and
// mingw-w64 headers lack them.
struct sch_tls_parameters { DWORD cAlpnIds; void* rgstrAlpnIds; DWORD grbitDisabledProtocols; DWORD cDisabledCrypto; void* pDisabledCrypto; DWORD dwFlags; };
struct sch_credentials { DWORD dwVersion, dwCredFormat, cCreds; void* paCred; HCERTSTORE hRootStore; DWORD cMappers; void* aphMappers; DWORD dwSessionLifespan, dwFlags, cTlsParameters; sch_tls_parameters* pTlsParameters; };
class tls_stream {
public:
    tls_stream() = default;
    tls_stream(const tls_stream&) = delete;
    ~tls_stream() {
        auto& a = sspi_api::get();
        if (has_ctx_) a.f->DeleteSecurityContext(&ctx_);
        if (has_cred_) a.f->FreeCredentialsHandle(&cred_);
    }
    static result<std::unique_ptr<tls_stream>> open(const url& u, const http_request& rq, int timeout_ms) {
        auto& a = sspi_api::get(); if (!a.ok) return {{}, unsupported("https: Schannel")};
        std::vector<std::string> anchors;
        if (!rq.insecure_skip_verify) { auto p = trust_pem(rq); if (!p) return {{}, p.err}; anchors = pem_certs(*p); }
        auto s = socket::connect(u.host, u.port, timeout_ms); if (!s) return {{}, s.err};
        std::unique_ptr<tls_stream> t(new tls_stream); t->s_ = std::move(*s); t->s_.set_timeout_ms(unsigned(timeout_ms)); t->target_ = widen(u.host);
        const DWORD flags = 0x8 /*SCH_CRED_MANUAL_CRED_VALIDATION*/ | 0x10 /*SCH_CRED_NO_DEFAULT_CREDS*/ | 0x00400000 /*SCH_USE_STRONG_CRYPTO*/;
        // TLS 1.2 + 1.3 (SCH_CREDENTIALS, everything older disabled); before Windows 10 1809, TLS 1.2 only.
        sch_tls_parameters tp{}; tp.grbitDisabledProtocols = 0x0c | 0x30 | 0xc0 | 0x300;   // SSL2, SSL3, TLS 1.0, TLS 1.1 (both ends)
        sch_credentials sc{}; sc.dwVersion = 5 /*SCH_CREDENTIALS_VERSION*/; sc.dwFlags = flags; sc.cTlsParameters = 1; sc.pTlsParameters = &tp;
        SECURITY_STATUS st = a.f->AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &t->cred_, nullptr);
        if (st != SEC_E_OK) {
            SCHANNEL_CRED old{}; old.dwVersion = SCHANNEL_CRED_VERSION; old.grbitEnabledProtocols = 0x800 /*SP_PROT_TLS1_2_CLIENT*/; old.dwFlags = flags;
            st = a.f->AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &old, nullptr, nullptr, &t->cred_, nullptr);
        }
        if (st != SEC_E_OK) return {{}, sec_error("AcquireCredentialsHandle", st)};
        t->has_cred_ = true;
        if (auto e = t->handshake(true)) return {{}, e};
        if (!rq.insecure_skip_verify) if (auto e = t->verify(anchors)) return {{}, e};
        if ((st = a.f->QueryContextAttributesW(&t->ctx_, SECPKG_ATTR_STREAM_SIZES, &t->sizes_)) != SEC_E_OK) return {{}, sec_error("QueryContextAttributes", st)};
        return {std::move(t), {}};
    }
    void set_timeout_ms(unsigned ms) { s_.set_timeout_ms(ms); }
    bool send_all(const void* p, std::size_t n, error& e) {
        auto& a = sspi_api::get(); auto* b = static_cast<const char*>(p);
        std::string m(sizes_.cbHeader + sizes_.cbMaximumMessage + sizes_.cbTrailer, '\0');
        while (n) {
            std::size_t k = std::min<std::size_t>(n, sizes_.cbMaximumMessage);
            std::memcpy(&m[sizes_.cbHeader], b, k);
            SecBuffer sb[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, &m[0]}, {ULONG(k), SECBUFFER_DATA, &m[sizes_.cbHeader]},
                               {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, &m[sizes_.cbHeader + k]}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, sb};
            if (auto st = a.f->EncryptMessage(&ctx_, 0, &d, 0); st != SEC_E_OK) { e = sec_error("EncryptMessage", st); return false; }
            if (!s_.send_all(m.data(), std::size_t(sb[0].cbBuffer) + sb[1].cbBuffer + sb[2].cbBuffer)) { e = sock_failure("send"); return false; }
            b += k; n -= k;
        }
        return true;
    }
    long recv(void* p, std::size_t n, error& e) {
        auto& a = sspi_api::get();
        for (;;) {
            if (plain_at_ < plain_.size()) {
                std::size_t k = std::min(n, plain_.size() - plain_at_); std::memcpy(p, plain_.data() + plain_at_, k); plain_at_ += k; return long(k);
            }
            if (closed_) return 0;
            if (!in_.empty()) {
                SecBuffer sb[4] = {{ULONG(in_.size()), SECBUFFER_DATA, &in_[0]}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc d{SECBUFFER_VERSION, 4, sb};
                SECURITY_STATUS st = a.f->DecryptMessage(&ctx_, &d, 0, nullptr);
                if (st == SEC_E_OK || st == SEC_I_RENEGOTIATE || st == SEC_I_CONTEXT_EXPIRED) {
                    std::string data, extra;
                    for (auto& x : sb) {
                        if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) data.assign(static_cast<const char*>(x.pvBuffer), x.cbBuffer);
                        if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) extra.assign(static_cast<const char*>(x.pvBuffer), x.cbBuffer);
                    }
                    in_ = std::move(extra); plain_ = std::move(data); plain_at_ = 0;
                    if (st == SEC_I_CONTEXT_EXPIRED) closed_ = true;   // close_notify
                    if (st == SEC_I_RENEGOTIATE) if (auto r = handshake(false)) { e = r; return -1; }   // TLS 1.3 post-handshake messages
                    continue;
                }
                if (st != SEC_E_INCOMPLETE_MESSAGE) { e = sec_error("DecryptMessage", st); return -1; }
            }
            char b[16384]; long r = s_.recv(b, sizeof b);
            if (r == 0) return 0;   // closed without close_notify: the HTTP framing decides whether that truncated anything
            if (r < 0) { e = sock_failure("recv"); return -1; }
            in_.append(b, std::size_t(r));
        }
    }
private:
    socket s_; CredHandle cred_{}; CtxtHandle ctx_{}; bool has_cred_ = false, has_ctx_ = false, closed_ = false;
    std::wstring target_; SecPkgContext_StreamSizes sizes_{}; std::string in_, plain_; std::size_t plain_at_ = 0;
    static error sec_error(const char* what, SECURITY_STATUS st) { return {int(st), std::string("https: ") + what + ": " + error_text(int(st)), err_domain::win32}; }
    error sock_failure(const char* what) { int e = sock_errno(); if (timed_out(e)) return tls_timeout(); return {e, std::string("https: ") + what + ": " + error_text(e)}; }
    error handshake(bool first) {
        auto& a = sspi_api::get();
        const ULONG req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | 0x00080000 /*ISC_REQ_MANUAL_CRED_VALIDATION*/;
        bool need = !first && in_.empty();
        for (;;) {
            if (need) {
                char b[16384]; long r = s_.recv(b, sizeof b);
                if (r == 0) return tls_error("handshake: connection closed");
                if (r < 0) return sock_failure("handshake");
                in_.append(b, std::size_t(r));
            }
            SecBuffer ib[2] = {{ULONG(in_.size()), SECBUFFER_TOKEN, in_.empty() ? nullptr : &in_[0]}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc id{SECBUFFER_VERSION, 2, ib};
            SecBuffer ob[1] = {{0, SECBUFFER_TOKEN, nullptr}}; SecBufferDesc od{SECBUFFER_VERSION, 1, ob};
            ULONG attr = 0;
            SECURITY_STATUS st = a.f->InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_, const_cast<SEC_WCHAR*>(target_.c_str()), req, 0, 0,
                                                                 first ? nullptr : &id, 0, &ctx_, &od, &attr, nullptr);
            if (first && (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED)) has_ctx_ = true;
            bool was_first = first; first = false;
            if (ob[0].pvBuffer) {
                bool sent = !ob[0].cbBuffer || s_.send_all(ob[0].pvBuffer, ob[0].cbBuffer);
                a.f->FreeContextBuffer(ob[0].pvBuffer);
                if (!sent) return sock_failure("handshake send");
            }
            if (st == SEC_E_INCOMPLETE_MESSAGE) { need = true; continue; }
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) {
                if (!was_first) { if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) in_.erase(0, in_.size() - ib[1].cbBuffer); else in_.clear(); }
                if (st == SEC_E_OK) return {};
                need = in_.empty(); continue;
            }
            if (st == SEC_I_INCOMPLETE_CREDENTIALS) return tls_error("handshake: the server requires a client certificate");
            return sec_error("handshake", st);
        }
    }
    error verify(const std::vector<std::string>& anchors) {
        auto& a = sspi_api::get();
        PCCERT_CONTEXT cert = nullptr;
        if (auto st = a.f->QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert); st != SEC_E_OK || !cert) return sec_error("no server certificate", st);
        HCERTSTORE root = nullptr; HCERTCHAINENGINE engine = nullptr;   // nullptr: the system (current user) engine
        auto cleanup = [&] { if (engine) a.FreeEngine(engine); if (root) a.CloseStore(root, 0); a.FreeCert(cert); };
        if (!anchors.empty()) {   // a private engine whose only roots are the caller's CAs
            root = a.OpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
            for (auto& d : anchors) if (root) a.AddEncoded(root, X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(d.data()), DWORD(d.size()), CERT_STORE_ADD_ALWAYS, nullptr);
            CERT_CHAIN_ENGINE_CONFIG cfg{}; cfg.cbSize = sizeof cfg; cfg.hExclusiveRoot = root;
            if (!root || !a.CreateEngine(&cfg, &engine)) { auto e = last_error("https: CertCreateCertificateChainEngine"); cleanup(); return e; }
        }
        char server_auth[] = szOID_PKIX_KP_SERVER_AUTH; LPSTR usage[] = {server_auth};
        CERT_CHAIN_PARA cp{}; cp.cbSize = sizeof cp; cp.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR; cp.RequestedUsage.Usage.cUsageIdentifier = 1; cp.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (!a.GetChain(engine, cert, nullptr, cert->hCertStore, &cp, 0, nullptr, &chain)) { auto e = last_error("https: CertGetCertificateChain"); cleanup(); return e; }
        HTTPSPolicyCallbackData pol; std::memset(&pol, 0, sizeof pol); pol.cbStruct = sizeof pol; pol.dwAuthType = AUTHTYPE_SERVER; pol.pwszServerName = const_cast<WCHAR*>(target_.c_str());
        CERT_CHAIN_POLICY_PARA pp{}; pp.cbSize = sizeof pp; pp.pvExtraPolicyPara = &pol;
        CERT_CHAIN_POLICY_STATUS ps{}; ps.cbSize = sizeof ps;
        BOOL ran = a.VerifyPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps);
        a.FreeChain(chain); cleanup();
        if (!ran) return last_error("https: CertVerifyCertificateChainPolicy");
        if (ps.dwError) return {int(ps.dwError), "https: certificate verification failed: " + error_text(int(ps.dwError)), err_domain::win32};
        return {};
    }
};
#elif defined(__APPLE__) && defined(__BLOCKS__)
// Network.framework (nw_connection with TLS options) through dlopen/dlsym; libdispatch and the blocks runtime are
// part of libSystem. Verification runs in our verify block: SecTrust with an SSL policy for the host, the system
// anchors or the caller's CAs only.
struct nw_api {
    using cfg_block = void (^)(void* options);
    using state_block = void (^)(int state, void* error);
    using send_block = void (^)(void* error);
    using recv_block = void (^)(void* content, void* context, bool is_complete, void* error);
    using complete_block = void (^)(bool);
    using verify_block = void (^)(void* metadata, void* trust, complete_block complete);
    using apply_block = bool (^)(void* region, std::size_t offset, const void* buffer, std::size_t size);
    void* (*parameters_create_secure_tcp)(cfg_block, cfg_block) = nullptr;
    void* (*tls_copy_sec_protocol_options)(void*) = nullptr;
    void (*set_server_name)(void*, const char*) = nullptr;
    void (*set_min_version)(void*, std::uint16_t) = nullptr;
    void (*set_verify_block)(void*, verify_block, void* queue) = nullptr;
    void* (*sec_trust_copy_ref)(void*) = nullptr;
    void (*sec_release)(void*) = nullptr;
    void* (*endpoint_create_host)(const char*, const char*) = nullptr;
    void* (*connection_create)(void*, void*) = nullptr;
    void (*connection_set_queue)(void*, void*) = nullptr;
    void (*connection_set_state_changed_handler)(void*, state_block) = nullptr;
    void (*connection_start)(void*) = nullptr; void (*connection_cancel)(void*) = nullptr;
    void (*connection_send)(void*, void*, void*, bool, send_block) = nullptr;
    void (*connection_receive)(void*, std::uint32_t, std::uint32_t, recv_block) = nullptr;
    int (*error_code)(void*) = nullptr; int (*error_domain)(void*) = nullptr;
    void (*nw_release)(void*) = nullptr;
    cfg_block default_cfg = nullptr; void* default_message = nullptr;
    void* (*queue_create)(const char*, void*) = nullptr; void (*dispatch_release)(void*) = nullptr;
    void* (*data_create)(const void*, std::size_t, void*, void*) = nullptr; bool (*data_apply)(void*, apply_block) = nullptr;
    int (*SecTrustSetAnchorCertificates)(void*, const void*) = nullptr; int (*SecTrustSetAnchorCertificatesOnly)(void*, unsigned char) = nullptr;
    int (*SecTrustSetPolicies)(void*, const void*) = nullptr; void* (*SecPolicyCreateSSL)(unsigned char, const void*) = nullptr;
    bool (*SecTrustEvaluateWithError)(void*, void**) = nullptr; void* (*SecCertificateCreateWithData)(const void*, const void*) = nullptr;
    const void* (*CFErrorCopyDescription)(const void*) = nullptr;
    bool ok = false;
    static const nw_api& get() {
        static nw_api a = [] {
            nw_api x;
            void* nw = ::dlopen("/System/Library/Frameworks/Network.framework/Network", RTLD_LAZY);
            void* sec = ::dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
            auto& cf = detail::cf_api::get();
            if (!nw || !sec || !cf.ok) return x;
            auto sym = [&](const char* n) -> void* { void* p = ::dlsym(nw, n); if (!p) p = ::dlsym(sec, n); if (!p) p = ::dlsym(RTLD_DEFAULT, n); return p; };
#define FORGE_NW_(m, n) x.m = reinterpret_cast<decltype(x.m)>(sym(n)); if (!x.m) return x
            FORGE_NW_(parameters_create_secure_tcp, "nw_parameters_create_secure_tcp"); FORGE_NW_(tls_copy_sec_protocol_options, "nw_tls_copy_sec_protocol_options");
            FORGE_NW_(set_server_name, "sec_protocol_options_set_tls_server_name"); FORGE_NW_(set_min_version, "sec_protocol_options_set_min_tls_protocol_version");
            FORGE_NW_(set_verify_block, "sec_protocol_options_set_verify_block"); FORGE_NW_(sec_trust_copy_ref, "sec_trust_copy_ref");
            FORGE_NW_(endpoint_create_host, "nw_endpoint_create_host"); FORGE_NW_(connection_create, "nw_connection_create");
            FORGE_NW_(connection_set_queue, "nw_connection_set_queue"); FORGE_NW_(connection_set_state_changed_handler, "nw_connection_set_state_changed_handler");
            FORGE_NW_(connection_start, "nw_connection_start"); FORGE_NW_(connection_cancel, "nw_connection_cancel");
            FORGE_NW_(connection_send, "nw_connection_send"); FORGE_NW_(connection_receive, "nw_connection_receive");
            FORGE_NW_(error_code, "nw_error_get_error_code"); FORGE_NW_(error_domain, "nw_error_get_error_domain"); FORGE_NW_(nw_release, "nw_release");
            FORGE_NW_(queue_create, "dispatch_queue_create"); FORGE_NW_(dispatch_release, "dispatch_release");
            FORGE_NW_(data_create, "dispatch_data_create"); FORGE_NW_(data_apply, "dispatch_data_apply");
            FORGE_NW_(SecTrustSetAnchorCertificates, "SecTrustSetAnchorCertificates"); FORGE_NW_(SecTrustSetAnchorCertificatesOnly, "SecTrustSetAnchorCertificatesOnly");
            FORGE_NW_(SecTrustSetPolicies, "SecTrustSetPolicies"); FORGE_NW_(SecPolicyCreateSSL, "SecPolicyCreateSSL");
            FORGE_NW_(SecTrustEvaluateWithError, "SecTrustEvaluateWithError"); FORGE_NW_(SecCertificateCreateWithData, "SecCertificateCreateWithData");
            FORGE_NW_(CFErrorCopyDescription, "CFErrorCopyDescription");
#undef FORGE_NW_
            x.sec_release = reinterpret_cast<decltype(x.sec_release)>(sym("sec_release"));
            auto dc = static_cast<cfg_block const*>(sym("_nw_parameters_configure_protocol_default_configuration"));
            auto dm = static_cast<void* const*>(sym("_nw_content_context_default_message"));
            if (!dc || !*dc || !dm || !*dm) return x;
            x.default_cfg = *dc; x.default_message = *dm; x.ok = true; return x;
        }();
        return a;
    }
};
class tls_stream {
    struct shared {   // touched by Network.framework's queue; outlives the stream while callbacks are pending
        std::mutex m; std::condition_variable cv;
        int state = 0, domain = 0, code = 0; bool verify_failed = false; std::string verify_msg;
        bool sent = false; int send_domain = 0, send_code = 0;
        bool received = false, eof = false; int recv_domain = 0, recv_code = 0; std::string rbuf;
    };
public:
    tls_stream() = default;
    tls_stream(const tls_stream&) = delete;
    ~tls_stream() {
        auto& a = nw_api::get();
        if (conn_) { a.connection_cancel(conn_); a.nw_release(conn_); }
        if (q_) a.dispatch_release(q_);
    }
    static result<std::unique_ptr<tls_stream>> open(const url& u, const http_request& rq, int timeout_ms) {
        auto& a = nw_api::get(); if (!a.ok) return {{}, unsupported("https: Network.framework")};
        std::vector<std::string> anchors; const bool insecure = rq.insecure_skip_verify;
        if (!insecure) { auto p = trust_pem(rq); if (!p) return {{}, p.err}; anchors = pem_certs(*p); }
        std::unique_ptr<tls_stream> t(new tls_stream); t->timeout_ = unsigned(timeout_ms);
        auto st = t->st_; const nw_api* ap = &a; std::string host = u.host; const bool ip = ip_literal(u.host);
        void* q = t->q_ = a.queue_create("forge.https", nullptr);
        nw_api::cfg_block tls = ^(void* options) {
            void* so = ap->tls_copy_sec_protocol_options(options);
            if (!ip) ap->set_server_name(so, host.c_str());
            ap->set_min_version(so, 0x0303 /*tls_protocol_version_TLSv12*/);
            ap->set_verify_block(so, ^(void*, void* trust, nw_api::complete_block done) {
                bool good = insecure;
                if (!good) {
                    auto& cf = detail::cf_api::get(); std::string why = "untrusted";
                    if (void* tr = ap->sec_trust_copy_ref(trust)) {
                        auto hs = cf.str(host.c_str()); void* pol = ap->SecPolicyCreateSSL(1, hs);
                        if (pol) { const void* pols[] = {pol}; auto arr = cf.CFArrayCreate(nullptr, pols, 1, cf.kCFTypeArrayCallBacks); ap->SecTrustSetPolicies(tr, arr); cf.CFRelease(arr); }
                        if (!anchors.empty()) {
                            std::vector<const void*> certs;
                            for (auto& d : anchors) {
                                auto data = cf.CFDataCreate(nullptr, reinterpret_cast<const std::uint8_t*>(d.data()), detail::CFIndex(d.size()));
                                if (void* c = ap->SecCertificateCreateWithData(nullptr, data)) certs.push_back(c);
                                cf.CFRelease(data);
                            }
                            auto arr = cf.CFArrayCreate(nullptr, certs.data(), detail::CFIndex(certs.size()), cf.kCFTypeArrayCallBacks);
                            ap->SecTrustSetAnchorCertificates(tr, arr); ap->SecTrustSetAnchorCertificatesOnly(tr, 1);
                            cf.CFRelease(arr); for (auto c : certs) cf.CFRelease(c);
                        }
                        void* err = nullptr; good = ap->SecTrustEvaluateWithError(tr, &err);
                        if (err) { if (auto d = ap->CFErrorCopyDescription(err)) { why = cf.to_string(d); cf.CFRelease(d); } cf.CFRelease(err); }
                        if (pol) cf.CFRelease(pol);
                        cf.CFRelease(hs); cf.CFRelease(tr);
                    }
                    if (!good) { std::lock_guard<std::mutex> l(st->m); st->verify_failed = true; st->verify_msg = why; }
                }
                done(good);
            }, q);
            if (ap->sec_release) ap->sec_release(so);
        };
        void* params = a.parameters_create_secure_tcp(tls, a.default_cfg);
        void* ep = a.endpoint_create_host(u.host.c_str(), std::to_string(u.port).c_str());
        if (!params || !ep) { if (params) a.nw_release(params); if (ep) a.nw_release(ep); return {{}, tls_error("nw_parameters / nw_endpoint")}; }
        t->conn_ = a.connection_create(ep, params); a.nw_release(ep); a.nw_release(params);
        if (!t->conn_) return {{}, tls_error("nw_connection_create")};
        a.connection_set_queue(t->conn_, q);
        a.connection_set_state_changed_handler(t->conn_, ^(int s, void* e) {
            std::lock_guard<std::mutex> l(st->m); st->state = s;
            if (e) { st->domain = ap->error_domain(e); st->code = ap->error_code(e); }
            st->cv.notify_all();
        });
        a.connection_start(t->conn_);
        std::unique_lock<std::mutex> l(st->m);
        // 3 ready, 4 failed, 5 cancelled; 1 waiting with an error = the path failed (refused, unreachable...)
        bool done = st->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return st->state >= 3 || (st->state == 1 && st->code); });
        if (!done) return {{}, tls_timeout()};
        if (st->state == 3) return {std::move(t), {}};
        if (st->verify_failed) return {{}, tls_error("certificate verification failed: " + st->verify_msg)};
        return {{}, nw_failure("connect " + u.host + ":" + std::to_string(u.port), st->domain, st->code)};
    }
    void set_timeout_ms(unsigned ms) { timeout_ = ms; }
    bool send_all(const void* p, std::size_t n, error& e) {
        auto& a = nw_api::get(); auto st = st_;
        void* data = a.data_create(p, n, q_, nullptr);   // copies the bytes
        { std::lock_guard<std::mutex> l(st->m); st->sent = false; }
        const nw_api* ap = &a;
        a.connection_send(conn_, data, a.default_message, true, ^(void* err) {
            std::lock_guard<std::mutex> l(st->m); st->sent = true;
            if (err) { st->send_domain = ap->error_domain(err); st->send_code = ap->error_code(err); }
            st->cv.notify_all();
        });
        a.dispatch_release(data);
        std::unique_lock<std::mutex> l(st->m);
        if (!st->cv.wait_for(l, std::chrono::milliseconds(timeout_), [&] { return st->sent; })) { e = tls_timeout(); return false; }
        if (st->send_code) { e = nw_failure("send", st->send_domain, st->send_code); return false; }
        return true;
    }
    long recv(void* p, std::size_t n, error& e) {
        auto& a = nw_api::get(); auto st = st_;
        std::unique_lock<std::mutex> l(st->m);
        if (st->rbuf.empty() && !st->eof) {
            st->received = false; l.unlock();
            const nw_api* ap = &a;
            a.connection_receive(conn_, 1, std::uint32_t(std::min<std::size_t>(n, 1u << 20)), ^(void* content, void*, bool complete, void* err) {
                std::lock_guard<std::mutex> g(st->m);
                if (content) a_copy(ap, content, st->rbuf);
                if (err) { st->recv_domain = ap->error_domain(err); st->recv_code = ap->error_code(err); }
                else if (complete) st->eof = true;
                st->received = true; st->cv.notify_all();
            });
            l.lock();
            if (!st->cv.wait_for(l, std::chrono::milliseconds(timeout_), [&] { return st->received; })) { e = tls_timeout(); return -1; }
        }
        if (!st->rbuf.empty()) { std::size_t k = std::min(n, st->rbuf.size()); std::memcpy(p, st->rbuf.data(), k); st->rbuf.erase(0, k); return long(k); }
        if (st->recv_code) { e = nw_failure("recv", st->recv_domain, st->recv_code); return -1; }
        return 0;
    }
private:
    void* conn_ = nullptr; void* q_ = nullptr; unsigned timeout_ = 10000; std::shared_ptr<shared> st_ = std::make_shared<shared>();
    static void a_copy(const nw_api* ap, void* content, std::string& out) {
        std::string* o = &out;
        ap->data_apply(content, ^bool(void*, std::size_t, const void* buf, std::size_t size) { o->append(static_cast<const char*>(buf), size); return true; });
    }
    // nw_error domains: 1 POSIX errno, 2 DNS (DNSServiceErrorType), 3 TLS (OSStatus)
    static error nw_failure(const std::string& what, int domain, int code) {
        if (domain == 1) { if (code == ETIMEDOUT) return tls_timeout(); return errno_error(code, "https: " + what + ": " + std::strerror(code)); }
        if (domain == 2) return errno_error(EHOSTUNREACH, "https: " + what + ": DNS error " + std::to_string(code));
        return tls_error(what + ": TLS error (OSStatus " + std::to_string(code) + ")");
    }
};
#elif defined(FORGE_OS_POSIX) && !defined(__APPLE__)
class tls_stream {
public:
    tls_stream() = default;
    tls_stream(const tls_stream&) = delete;
    ~tls_stream() { auto& a = ssl_api::instance(); if (ssl_) a.SSL_free(ssl_); if (ctx_) a.SSL_CTX_free(ctx_); }
    static result<std::unique_ptr<tls_stream>> open(const url& u, const http_request& rq, int timeout_ms) {
        auto& a = ssl_api::instance(); if (!a.ok) return {{}, unsupported("https: no TLS library")};
        std::string anchors; const bool insecure = rq.insecure_skip_verify;
        if (!insecure) { auto p = trust_pem(rq); if (!p) return {{}, p.err}; anchors = std::move(*p); }
        auto s = socket::connect(u.host, u.port, timeout_ms); if (!s) return {{}, s.err};
        std::unique_ptr<tls_stream> t(new tls_stream); t->s_ = std::move(*s); t->s_.set_timeout_ms(unsigned(timeout_ms)); t->d_.s = &t->s_;
        a.ERR_clear_error();
        if (!(t->ctx_ = a.SSL_CTX_new(a.TLS_client_method()))) return {{}, a.failure(nullptr, t->d_, "SSL_CTX_new", false)};
        a.SSL_CTX_ctrl(t->ctx_, 123 /*SSL_CTRL_SET_MIN_PROTO_VERSION*/, 0x0303 /*TLS1_2_VERSION*/, nullptr);
        a.SSL_CTX_ctrl(t->ctx_, 33 /*SSL_CTRL_MODE*/, 4 /*SSL_MODE_AUTO_RETRY*/, nullptr);
        a.SSL_CTX_set_verify(t->ctx_, insecure ? 0 /*SSL_VERIFY_NONE*/ : 1 /*SSL_VERIFY_PEER*/, nullptr);
        if (!insecure && anchors.empty()) a.SSL_CTX_set_default_verify_paths(t->ctx_);
        else if (!insecure) {
            void* store = a.SSL_CTX_get_cert_store(t->ctx_); void* bio = a.BIO_new_mem_buf(anchors.data(), int(anchors.size()));
            while (void* x = bio ? a.PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr) { a.X509_STORE_add_cert(store, x); a.X509_free(x); }
            if (bio) a.BIO_free(bio);
            a.ERR_clear_error();   // the end-of-input "no start line"
        }
        if (!(t->ssl_ = a.SSL_new(t->ctx_))) return {{}, a.failure(nullptr, t->d_, "SSL_new", false)};
        const bool ip = ip_literal(u.host);
        if (!ip) a.SSL_ctrl(t->ssl_, 55 /*SSL_CTRL_SET_TLSEXT_HOSTNAME*/, 0 /*TLSEXT_NAMETYPE_host_name*/, const_cast<char*>(u.host.c_str()));
        if (!insecure && (ip ? a.X509_VERIFY_PARAM_set1_ip_asc(a.SSL_get0_param(t->ssl_), u.host.c_str()) : a.SSL_set1_host(t->ssl_, u.host.c_str())) != 1)
            return {{}, a.failure(nullptr, t->d_, "set host", false)};
        if (!a.attach(t->ssl_, t->d_)) return {{}, a.failure(nullptr, t->d_, "BIO_new", false)};
        if (a.SSL_connect(t->ssl_) != 1) return {{}, a.failure(t->ssl_, t->d_, "handshake", !insecure)};
        return {std::move(t), {}};
    }
    void set_timeout_ms(unsigned ms) { s_.set_timeout_ms(ms); }
    bool send_all(const void* p, std::size_t n, error& e) {
        auto& a = ssl_api::instance(); auto* b = static_cast<const char*>(p);
        while (n) {
            a.ERR_clear_error(); d_.err = 0;
            int r = a.SSL_write(ssl_, b, int(std::min<std::size_t>(n, INT_MAX)));
            if (r <= 0) { e = a.failure(nullptr, d_, "write", false); return false; }
            b += r; n -= std::size_t(r);
        }
        return true;
    }
    long recv(void* p, std::size_t n, error& e) {
        auto& a = ssl_api::instance(); a.ERR_clear_error(); d_.err = 0;
        int r = a.SSL_read(ssl_, p, int(std::min<std::size_t>(n, INT_MAX)));
        if (r > 0) return r;
        // close_notify, or a plain TCP close (no close_notify): the HTTP framing decides whether that truncated anything
        if (a.SSL_get_error(ssl_, r) == 6 /*SSL_ERROR_ZERO_RETURN*/ || (d_.eof && !d_.err)) { a.ERR_clear_error(); return 0; }
        e = a.failure(nullptr, d_, "read", false); return -1;
    }
private:
    socket s_; ssl_api::sock_bio d_; void* ctx_ = nullptr; void* ssl_ = nullptr;
};
#else
class tls_stream {
public:
    static result<std::unique_ptr<tls_stream>> open(const url&, const http_request&, int) { return {{}, unsupported("https: no TLS backend in this build")}; }
    void set_timeout_ms(unsigned) {}
    bool send_all(const void*, std::size_t, error&) { return false; }
    long recv(void*, std::size_t, error&) { return -1; }
};
#endif
// The transport under one exchange: plain TCP or TLS.
struct http_conn {
    socket s; std::unique_ptr<tls_stream> tls;
    void set_timeout_ms(unsigned ms) { if (tls) tls->set_timeout_ms(ms); else s.set_timeout_ms(ms); }
    bool send_all(const void* p, std::size_t n, error& e) {
        if (tls) return tls->send_all(p, n, e);
        if (s.send_all(p, n)) return true;
        e = last_error("http send"); return false;
    }
    long recv(void* p, std::size_t n, error& e) {
        if (tls) return tls->recv(p, n, e);
        long r = s.recv(p, n);
        if (r < 0) { int c = sock_errno(); e = timed_out(c) ? error{ETIMEDOUT, "http: timed out reading the response"} : last_error("http recv"); }
        return r;
    }
};
}
inline result<http_response> http(const http_request& rq);
namespace detail_net {
inline result<http_response> http_once(const http_request& rq, std::uint64_t end_ns) {
    auto u = parse_url(rq.url); if (!u) return {{}, u.err};
    const bool tls = u->scheme == "https";
    if (u->scheme != "http" && !tls) return {{}, unsupported("http: scheme '" + u->scheme + "' (only http:// and https://)")};
    if (!token(rq.method)) return {{}, {EINVAL, "http: invalid method"}};
    bool ua = false, len = false, host = false;
    for (auto& [k, v] : rq.headers) {
        if (!token(k) || !field_value(v)) return {{}, {EINVAL, "http: invalid header '" + k + "'"}};
        auto l = lower(k); ua |= l == "user-agent"; len |= l == "content-length"; host |= l == "host";
    }
    auto left = [&]() -> int {   // ms for the next blocking step: min(per-step timeout, what remains of the deadline)
        int t = rq.timeout_ms > 0 ? rq.timeout_ms : 1 << 30; if (!end_ns) return t;
        auto now = monotonic_ns(); if (now >= end_ns) return 0;
        return int(std::min<std::uint64_t>((end_ns - now) / 1000000 + 1, std::uint64_t(t)));
    };
    const error deadline{ETIMEDOUT, "http: deadline exceeded"};
    int t = left(); if (!t) return {{}, deadline};
    http_conn s;
    if (tls) { auto c = tls_stream::open(*u, rq, t); if (!c) return {{}, c.err}; s.tls = std::move(*c); }
    else { auto c = socket::connect(u->host, u->port, t); if (!c) return {{}, c.err}; s.s = std::move(*c); }
    error io;
    std::string req = rq.method + " " + encode_target(u->path, false) + (u->query.empty() ? "" : "?" + encode_target(u->query, true)) + " HTTP/1.1\r\n";
    if (!host) req += "Host: " + host_port(*u) + "\r\n";
    req += "Connection: close\r\n";
    for (auto& [k, v] : rq.headers) if (lower(k) != "connection") req += k + ": " + v + "\r\n";
    if (!ua) req += "User-Agent: forge\r\n";
    if (!rq.body.empty() || rq.method == "POST" || rq.method == "PUT" || rq.method == "PATCH") { if (!len) req += "Content-Length: " + std::to_string(rq.body.size()) + "\r\n"; }
    req += "\r\n"; req += rq.body;
    if (!(t = left())) return {{}, deadline};
    s.set_timeout_ms(unsigned(t));
    if (!s.send_all(req.data(), req.size(), io)) return {{}, io};
    http_response r; r.url = to_string(*u);
    std::string raw; char buf[16384]; std::size_t he = std::string::npos, body_at = 0;
    bool head = false, chunked = false, no_body = false; std::uint64_t clen = ~0ull;
    auto parse_head = [&]() -> error {   // status line + headers of raw[0, he)
        std::size_t pos = raw.find("\r\n"); std::string sl = raw.substr(0, pos);
        auto a = sl.find(' '); if (a == std::string::npos || sl.compare(0, 5, "HTTP/")) return {EPROTO, "http: bad status line"};
        r.status = std::atoi(sl.c_str() + a + 1); if (r.status < 100 || r.status > 999) return {EPROTO, "http: bad status line"};
        auto b = sl.find(' ', a + 1); r.reason = b == std::string::npos ? std::string() : sl.substr(b + 1);
        r.headers.clear(); r.header_list.clear();
        while (pos < he) {
            auto e = raw.find("\r\n", pos + 2); auto line = raw.substr(pos + 2, e - pos - 2); pos = e;
            auto c = line.find(':'); if (c == std::string::npos) continue;
            std::string k = lower(line.substr(0, c)), v = line.substr(c + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            r.header_list.emplace_back(k, v);
            auto it = r.headers.find(k);
            if (it == r.headers.end()) r.headers.emplace(k, v); else it->second += (k == "set-cookie" ? "\n" : ", ") + v;
        }
        chunked = false; clen = ~0ull;
        if (auto te = r.headers.find("transfer-encoding"); te != r.headers.end()) {
            if (lower(te->second).find("chunked") == std::string::npos) return {EPROTO, "http: unsupported transfer-encoding '" + te->second + "'"};
            chunked = true;   // RFC 9112 6.3: Transfer-Encoding overrides Content-Length
        } else for (auto& v : r.values("content-length")) {
            if (v.empty() || v.size() > 19 || v.find_first_not_of("0123456789") != std::string::npos) return {EPROTO, "http: bad content-length '" + v + "'"};
            auto n = std::strtoull(v.c_str(), nullptr, 10); if (clen != ~0ull && clen != n) return {EPROTO, "http: conflicting content-length"};
            clen = n;
        }
        no_body = head || r.status == 204 || r.status == 304 || r.status < 200;
        if (!no_body && clen != ~0ull && clen > rq.max_body) return {EFBIG, "http: response too large"};
        return {};
    };
    head = rq.method == "HEAD";
    for (bool closed = false; !closed;) {
        if (!(t = left())) return {{}, deadline};
        s.set_timeout_ms(unsigned(t));
        long n = s.recv(buf, sizeof buf, io);
        if (n < 0) return {{}, io};
        if (n == 0) closed = true; else raw.append(buf, std::size_t(n));
        if (raw.size() - body_at > rq.max_body + 65536) return {{}, {EFBIG, "http: response too large"}};
        while (he == std::string::npos) {   // head not parsed yet (or an interim 1xx was skipped)
            auto e = raw.find("\r\n\r\n"); if (e == std::string::npos) break;
            he = e; if (auto err = parse_head()) return {{}, err};
            if (r.status >= 100 && r.status < 200 && r.status != 101) { raw.erase(0, e + 4); he = std::string::npos; continue; }
            body_at = e + 4;
        }
        if (he == std::string::npos) { if (closed) return {{}, {EPROTO, raw.empty() ? "http: connection closed without a response" : "http: malformed response"}}; continue; }
        if (no_body) break;
        if (chunked) {
            int st = dechunk(raw, body_at, r.body, rq.max_body);
            if (st == 1) break;
            if (st == -1) return {{}, {EPROTO, "http: malformed chunked body"}};
            if (st == -2) return {{}, {EFBIG, "http: response too large"}};
            if (closed) return {{}, {EPROTO, "http: truncated chunked body (no final chunk)"}};
        } else if (clen != ~0ull) {
            if (raw.size() - body_at >= clen) break;
            if (closed) return {{}, {EPROTO, "http: truncated body (" + std::to_string(raw.size() - body_at) + " of " + std::to_string(clen) + " bytes)"}};
        } else if (raw.size() - body_at > rq.max_body) return {{}, {EFBIG, "http: response too large"}};
    }
    if (!no_body && !chunked) r.body = raw.substr(body_at, clen == ~0ull ? std::string::npos : std::size_t(clen));
    if (no_body) r.body.clear();
    if (r.status >= 300 && r.status < 400 && r.status != 304 && rq.max_redirects > 0 && r.headers.count("location")) {
        auto to = resolve_reference(*u, r.headers["location"]); if (!to) return {{}, to.err};
        http_request n = rq; n.max_redirects--; n.url = to_string(*to);
        auto drop = [&](std::initializer_list<const char*> names) { for (auto it = n.headers.begin(); it != n.headers.end();) { bool d = false; auto l = lower(it->first); for (auto x : names) d |= l == x; it = d ? n.headers.erase(it) : std::next(it); } };
        if (to->scheme != u->scheme || lower(to->host) != lower(u->host) || to->port != u->port) drop({"authorization", "proxy-authorization", "cookie", "host"});
        if (r.status == 303 || ((r.status == 301 || r.status == 302) && rq.method == "POST")) {   // what browsers do
            if (n.method != "HEAD") n.method = "GET";
            n.body.clear(); drop({"content-length", "content-type", "transfer-encoding"});
        }
        return http_once(n, end_ns);
    }
    return {std::move(r), {}};
}
}
inline result<http_response> http(const http_request& rq) {
    net_init();
    return detail_net::http_once(rq, rq.deadline_ms > 0 ? monotonic_ns() + std::uint64_t(rq.deadline_ms) * 1000000ull : 0);
}
inline result<http_response> http_get(const std::string& u, int timeout_ms = 10000) { http_request r; r.url = u; r.timeout_ms = timeout_ms; return http(r); }
} // namespace forge::os::net
