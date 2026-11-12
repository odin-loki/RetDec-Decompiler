// forge-serve — OpenAI-compatible HTTP server over the continuous-batching scheduler.
//
//   forge-serve <model.fpk|hf_dir|model.gguf> [--host 127.0.0.1] [--port 8080] [--max-active 8] [--ctx N] [--codec f32] [--watch] [--no-pin] [--numa-node N] [--mem-limit 6G|75%|off] [--max-queue N] [--kv-spill] [--policy F]   (see docs/serve.md)
//
//   GET  /health                 -> {"status":"ok"}
//   GET  /v1/models              -> model list
//   POST /v1/completions         {"prompt": str | [ids], "max_tokens", "temperature", "top_p", "top_k", "min_p",
//                                 "repetition_penalty", "seed", "stop": [str], "grammar": GBNF, "stream": bool,
//                                 "typical_p", "mirostat_tau", "mirostat_eta", "presence_penalty", "frequency_penalty", "response_format": {json_schema}}
//   POST /v1/chat/completions    {"messages": [{"role","content"}], ...same sampling fields}
//   GET  /metrics                -> scheduler counters
#include <forge/import/gguf.hpp>
#include <forge/inference/json_schema.hpp>
#include <forge/inference/inference.hpp>
#include <forge/core/net.hpp>
#include <forge/loadbalancer/proxy.hpp>
#include <forge/os/cpu_topology.hpp>
#include <forge/os/file_watch.hpp>
#include <forge/os/memory_pressure.hpp>
#include <forge/os/numa.hpp>
#include <forge/os/service.hpp>
#include <forge/security/security.hpp>
#include <signal.h>
#include <atomic>
#include <cstdio>
#include <filesystem>

using namespace forge;

namespace {

struct conn {
    forge_sock_t fd;
    bool send(std::string_view s) {
        while (!s.empty()) { auto n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL); if (n <= 0) return false; s.remove_prefix(std::size_t(n)); }
        return true;
    }
};

struct http_request { std::string method, path, body; std::map<std::string, std::string> headers; };

bool read_request(forge_sock_t fd, http_request& r) {
    std::string buf; char tmp[8192];
    std::size_t hdr_end;
    while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
        auto n = ::recv(fd, tmp, sizeof tmp, 0); if (n <= 0) return false; buf.append(tmp, std::size_t(n));
        if (buf.size() > (1u << 20)) return false;
    }
    auto line_end = buf.find("\r\n");
    auto first = buf.substr(0, line_end);
    auto sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    r.method = first.substr(0, sp1); r.path = first.substr(sp1 + 1, sp2 - sp1 - 1);
    std::size_t p = line_end + 2;
    while (p < hdr_end) {
        auto e = buf.find("\r\n", p); auto l = buf.substr(p, e - p); p = e + 2;
        auto c = l.find(':'); if (c == std::string::npos) continue;
        std::string k = l.substr(0, c); for (auto& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        auto v = l.substr(c + 1); while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        r.headers[k] = v;
    }
    std::size_t len = r.headers.count("content-length") ? std::stoul(r.headers["content-length"]) : 0;
    if (len > (64u << 20)) return false;
    r.body = buf.substr(hdr_end + 4);
    while (r.body.size() < len) { auto n = ::recv(fd, tmp, sizeof tmp, 0); if (n <= 0) return false; r.body.append(tmp, std::size_t(n)); }
    r.body.resize(len);
    return true;
}

std::string response(int code, const std::string& body, const char* type = "application/json") {
    const char* msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : code == 503 ? "Service Unavailable" : code == 507 ? "Insufficient Storage" : "Internal Server Error";
    return std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n{}Access-Control-Allow-Origin: *\r\n\r\n{}", code, msg, type, body.size(), code == 503 ? "Retry-After: 1\r\n" : "", body);
}
std::string error_json(const std::string& m) { return json::dump(json::value(json::object{{"error", json::value(json::object{{"message", m}, {"type", "invalid_request_error"}})}})); }

// JSON strings must be valid UTF-8: byte-level tokens can split or produce invalid sequences.
std::size_t utf8_len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0; }
// valid range of the second byte for a lead byte (RFC 3629: no overlongs, surrogates or > U+10FFFF)
bool second_ok(unsigned char lead, unsigned char b) {
    if (lead == 0xE0) return b >= 0xA0 && b <= 0xBF;
    if (lead == 0xED) return b >= 0x80 && b <= 0x9F;
    if (lead == 0xF0) return b >= 0x90 && b <= 0xBF;
    if (lead == 0xF4) return b >= 0x80 && b <= 0x8F;
    return (b & 0xC0) == 0x80;
}
std::string sanitize(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
        auto n = utf8_len(static_cast<unsigned char>(s[i]));
        bool ok = n > 0 && i + n <= s.size();
        for (std::size_t k = 1; ok && k < n; ++k)
            ok = k == 1 ? second_ok(static_cast<unsigned char>(s[i]), static_cast<unsigned char>(s[i + 1])) : (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok) { o.append(s.substr(i, n)); i += n; } else { o += "\xEF\xBF\xBD"; ++i; }
    }
    return o;
}
// length of the prefix of `s` that does not end inside an incomplete (but so far valid) sequence
std::size_t complete_prefix(std::string_view s) {
    for (std::size_t back = 1; back <= 3 && back <= s.size(); ++back) {
        auto c = static_cast<unsigned char>(s[s.size() - back]);
        if ((c & 0xC0) == 0x80) continue;
        auto n = utf8_len(c);
        return n > back ? s.size() - back : s.size();
    }
    return s.size();
}

// per-request channel between the scheduler thread and the connection thread
struct channel {
    std::mutex mu; std::condition_variable cv;
    std::deque<int> toks; bool done = false; std::string reason; std::vector<int> all;
    std::atomic<bool> cancelled{false};
};

class server {
public:
    server(std::shared_ptr<inference::model> m, inference::batch_options bo, std::string name, std::size_t max_queue = 0)
        : m_(std::move(m)), sched_(m_, bo), name_(std::move(name)), max_queue_(max_queue) { sched_.start(); }
    ~server() { sched_.stop(); }
    void evict_kv(std::size_t keep) { sched_.request_kv_eviction(keep); }

    void handle(forge_sock_t fd) {
        conn c{fd};
        http_request r;
        if (!read_request(fd, r)) { forge_sock_close(fd); return; }
        try { route(c, r); }
        catch (const forge_exception& e) { c.send(response(e.code == errc::out_of_memory ? 503 : 400, error_json(e.what()))); }
        catch (const std::bad_alloc&) { c.send(response(503, error_json("out of memory"))); }
        catch (const std::exception& e) { c.send(response(400, error_json(e.what()))); }
        forge_sock_close(fd);
    }
private:
    void route(conn& c, const http_request& r) {
        auto path = r.path.substr(0, r.path.find('?'));
        if (r.method == "OPTIONS") { c.send("HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Methods: GET, POST\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); return; }
        if (path == "/health") { c.send(response(200, R"({"status":"ok"})")); return; }
        if (path == "/v1/models") {
            c.send(response(200, json::dump(json::value(json::object{{"object", "list"}, {"data", json::value(json::array{json::value(json::object{
                {"id", name_}, {"object", "model"}, {"owned_by", "forge"}})})}})))); return;
        }
        if (path == "/metrics") {
            auto& s = sched_.stats();
            c.send(response(200, std::format("forge_steps {}\nforge_completed {}\nforge_prefix_hits {}\nforge_prefix_tokens_reused {}\nforge_peak_active {}\nforge_peak_kv_bytes {}\n"
                                             "forge_queue_waiting {}\nforge_queue_active {}\nforge_admission_deferred_total {}\nforge_rejected_total {}\nforge_kv_evictions_total {}\n"
                                             "forge_kv_spills_total {}\nforge_kv_reloads_total {}\nforge_kv_spilled {}\n",
                                             s.steps, s.completed, s.prefix_hits, s.prefix_tokens_reused, s.peak_active, s.peak_kv_bytes,
                                             sched_.waiting(), sched_.active_count(), s.deferred, rejected_.load(), sched_.evictions(),
                                             s.prefix_spills, s.prefix_reloads, sched_.spilled()) + mem::governor::instance().prometheus(), "text/plain")); return;
        }
        bool chat = path == "/v1/chat/completions";
        if (path != "/v1/completions" && !chat) { c.send(response(404, error_json("no route " + path))); return; }
        if (r.method != "POST") { c.send(response(405, error_json("use POST"))); return; }
        // Backpressure: a full queue answers 503 + Retry-After instead of piling up work it cannot hold.
        if (max_queue_ && sched_.waiting() >= max_queue_) { ++rejected_; c.send(response(503, error_json("server busy: request queue is full, retry later"))); return; }
        auto j = json::parse(r.body);
        auto* tok = m_->tokenizer();
        std::vector<int> prompt;
        if (chat) {
            if (!tok) raise(errc::unsupported, "chat needs a tokenizer");
            std::string text;
            for (auto& msg : j["messages"].arr()) text += msg.str_or("role", "user") + ": " + msg.str_or("content", "") + "\n";
            text += "assistant:";
            prompt = tok->encode(text);
            if (tok->add_bos()) prompt.insert(prompt.begin(), tok->bos());
        } else {
            auto& p = j["prompt"];
            if (p.is_arr()) for (auto& x : p.arr()) prompt.push_back(int(x.num()));
            else {
                if (!tok) raise(errc::unsupported, "text prompts need a tokenizer; send token ids");
                prompt = tok->encode(p.str());
                if (tok->add_bos() && !j.find("add_bos")) prompt.insert(prompt.begin(), tok->bos());
                else if (auto* ab = j.find("add_bos"); ab && ab->is_bool() && ab->b() && tok->bos() >= 0) prompt.insert(prompt.begin(), tok->bos());
            }
        }
        if (prompt.empty()) raise(errc::invalid_argument, "empty prompt");
        inference::request q;
        q.prompt = prompt;
        q.max_new = int(j.num_or("max_tokens", 16));
        q.sampling.temperature = float(j.num_or("temperature", 0));
        q.sampling.top_p = float(j.num_or("top_p", 1));
        q.sampling.top_k = int(j.num_or("top_k", 0));
        q.sampling.min_p = float(j.num_or("min_p", 0));
        q.sampling.typical_p = float(j.num_or("typical_p", 1));
        q.sampling.mirostat_tau = float(j.num_or("mirostat_tau", 0));
        q.sampling.mirostat_eta = float(j.num_or("mirostat_eta", 0.1));
        q.sampling.presence_penalty = float(j.num_or("presence_penalty", 0));
        q.sampling.frequency_penalty = float(j.num_or("frequency_penalty", 0));
        q.sampling.repetition_penalty = float(j.num_or("repetition_penalty", 1));
        q.sampling.seed = std::uint64_t(j.num_or("seed", 0));
        if (auto* g = j.find("grammar"); g && g->is_str()) q.grammar = std::make_shared<inference::grammar>(g->str());
        // OpenAI-style structured output: {"response_format": {"type": "json_schema", "json_schema": {"schema": {...}}}}
        if (auto* rf = j.find("response_format"); rf && rf->str_or("type", "") == "json_schema")
            if (auto* js = rf->find("json_schema"); js && js->find("schema")) q.grammar = std::make_shared<inference::grammar>(inference::json_schema_to_gbnf((*js)["schema"]));
        std::vector<std::string> stops;
        if (auto* s = j.find("stop")) { if (s->is_str()) stops.push_back(s->str()); else if (s->is_arr()) for (auto& x : s->arr()) stops.push_back(x.str()); }
        bool stream = false; if (auto* s = j.find("stream"); s && s->is_bool()) stream = s->b();

        auto ch = std::make_shared<channel>();
        q.on_token = [ch](int t) { std::lock_guard lk(ch->mu); ch->toks.push_back(t); ch->cv.notify_all(); return !ch->cancelled.load(); };
        q.on_done = [ch](const std::vector<int>& all, const std::string& why) { std::lock_guard lk(ch->mu); ch->done = true; ch->reason = why; ch->all = all; ch->cv.notify_all(); };
        sched_.submit(std::move(q));

        auto id = std::format("cmpl-{}", ++counter_);
        auto created = double(std::time(nullptr));
        std::string text; std::vector<int> ids; std::string finish = "length";
        std::size_t sent = 0;   // bytes of `text` already streamed
        auto piece = [&](int t) { return tok ? tok->decode_one(t) : std::format("{} ", t); };
        if (stream) c.send("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        for (;;) {
            std::unique_lock lk(ch->mu);
            ch->cv.wait(lk, [&] { return ch->done || !ch->toks.empty(); });
            std::vector<int> batch(ch->toks.begin(), ch->toks.end()); ch->toks.clear();
            bool done = ch->done; std::string why = ch->reason;
            lk.unlock();
            bool hit_stop = false;
            for (int t : batch) {
                if (t == (tok ? tok->eos() : -1)) continue;
                std::string p = piece(t);
                std::size_t before = text.size();
                text += p; ids.push_back(t);
                for (auto& s : stops) if (auto f = text.find(s); f != std::string::npos) { text.resize(f); hit_stop = true; break; }
                (void)before;
                if (stream) {
                    auto upto = hit_stop ? text.size() : complete_prefix(text);
                    p = upto > sent ? sanitize(std::string_view(text).substr(sent, upto - sent)) : std::string();
                    sent = std::max(sent, upto);
                }
                if (stream && !p.empty()) {
                    json::object delta = chat ? json::object{{"index", 0}, {"delta", json::value(json::object{{"content", p}})}}
                                              : json::object{{"index", 0}, {"text", p}, {"token", t}};
                    json::value ev(json::object{{"id", id}, {"object", chat ? "chat.completion.chunk" : "text_completion"}, {"created", created},
                                                {"model", name_}, {"choices", json::value(json::array{json::value(delta)})}});
                    if (!c.send("data: " + json::dump(ev) + "\n\n")) { ch->cancelled = true; hit_stop = true; }
                }
                if (hit_stop) break;
            }
            if (hit_stop) { ch->cancelled = true; finish = "stop";
                            std::unique_lock l2(ch->mu); ch->cv.wait(l2, [&] { return ch->done; }); break; }
            if (done) { finish = why == "stop" || why == "grammar" ? "stop" : why == "length" ? "length" : why; break; }
        }
        json::array idv; for (int t : ids) idv.push_back(json::value(t));
        json::object usage{{"prompt_tokens", double(prompt.size())}, {"completion_tokens", double(ids.size())}, {"total_tokens", double(prompt.size() + ids.size())}};
        if (stream) {
            json::object fin = chat ? json::object{{"index", 0}, {"delta", json::value(json::object{})}, {"finish_reason", finish}}
                                    : json::object{{"index", 0}, {"text", ""}, {"finish_reason", finish}};
            c.send("data: " + json::dump(json::value(json::object{{"id", id}, {"object", chat ? "chat.completion.chunk" : "text_completion"},
                                                                   {"choices", json::value(json::array{json::value(fin)})}, {"usage", json::value(usage)}})) + "\n\ndata: [DONE]\n\n");
            return;
        }
        json::object choice = chat ? json::object{{"index", 0}, {"message", json::value(json::object{{"role", "assistant"}, {"content", sanitize(text)}})}, {"finish_reason", finish}}
                                   : json::object{{"index", 0}, {"text", sanitize(text)}, {"tokens", json::value(idv)}, {"finish_reason", finish}};
        c.send(response(200, json::dump(json::value(json::object{{"id", id}, {"object", chat ? "chat.completion" : "text_completion"}, {"created", created},
                                                                  {"model", name_}, {"choices", json::value(json::array{json::value(choice)})}, {"usage", json::value(usage)}}))));
    }
    std::shared_ptr<inference::model> m_;
    inference::batch_scheduler sched_;
    std::string name_;
    std::atomic<std::uint64_t> counter_{0}, rejected_{0};
    std::size_t max_queue_ = 0;
};

bool has_flag(int argc, char** argv, const std::string& k) {
    for (int i = 1; i < argc; ++i) if (argv[i] == "--" + k) return true;
    return false;
}
std::string arg(int argc, char** argv, const std::string& k, const std::string& d) {
    for (int i = 1; i + 1 < argc; ++i) if (argv[i] == "--" + k) return argv[i + 1];
    return d;
}

// FSS (docs/serve.md "Security policy"): --policy F, else the load_policy() search. An active policy becomes the
// process's policy for enforce() (model loads check fs.read and integrity.models through it); profile none: nothing.
security::policy activate_policy(int argc, char** argv) {
    auto p = security::load_policy(arg(argc, argv, "policy", ""));
    if (p.active()) security::set_active_policy(p);
    return p;
}
std::string host_port(const std::string& h, int port) { return (h.find(':') != std::string::npos ? "[" + h + "]" : h) + ":" + std::to_string(port); }
// SBX-1: after the socket is bound and the model loaded, before the first request.
void server_lockdown(const security::policy& p, const char* who, const std::string& listen) {
    if (!p.active()) return;
    auto r = security::lockdown(p);
    for (auto& c : r.controls)
        if (c.verdict != "ok") std::fprintf(stderr, "%s: security %s %s: %s%s\n", who, c.id.c_str(), c.name.c_str(), c.state.c_str(), c.detail.empty() ? "" : (" (" + c.detail + ")").c_str());
    security::audit("sandbox", security::decision::allow, "SBX-1", listen, std::string(who) + ": locked down (profile " + security::name(p.prof) + ", " + p.source + ") before serving");
}

// forge-serve proxy --backend host:port [--backend ...] [--mode l4|l7] [--strategy p2c] [--host] [--port]
//   [--health-ms 2000] [--retries 3] [--route /prefix=host:port,host:port] [--policy F]
int run_proxy(int argc, char** argv) {
    using namespace forge::loadbalancer;
    config c; auto st = parse_strategy(arg(argc, argv, "strategy", "p2c"));
    if (!st) { std::fprintf(stderr, "forge-serve proxy: unknown --strategy\n"); return 2; }
    c.strat = *st;
    auto p = std::make_shared<pool>(c);
    proxy_config pc;
    std::vector<std::string> targets;   // every backend, for the policy's net_connect check
    for (int i = 2; i + 1 < argc; ++i) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--backend") { p->add({v, v}); targets.push_back(v); }
        else if (k == "--route") {
            auto eq = v.find('='); if (eq == std::string::npos) continue;
            auto rp = std::make_shared<pool>(c); std::string list = v.substr(eq + 1);
            for (std::size_t a = 0; a <= list.size();) { auto b = list.find(',', a); if (b == std::string::npos) b = list.size(); if (b > a) { rp->add({list.substr(a, b - a), list.substr(a, b - a)}); targets.push_back(list.substr(a, b - a)); } a = b + 1; }
            rp->set_health_check(tcp_probe());
            pc.routes.push_back({v.substr(0, eq), rp});
        }
    }
    if (!p->size() && pc.routes.empty()) { std::fprintf(stderr, "forge-serve proxy: need at least one --backend host:port\n"); return 2; }
    pc.mode = arg(argc, argv, "mode", "l7") == "l4" ? proxy_config::l4 : proxy_config::l7;
    pc.listen_host = arg(argc, argv, "host", "127.0.0.1"); pc.listen_port = std::stoi(arg(argc, argv, "port", "8080"));
    pc.retries = unsigned(std::stoi(arg(argc, argv, "retries", "3")));
    p->set_health_check(tcp_probe());
    security::policy pol;
    try {
        pol = activate_policy(argc, argv);
        if (pol.active()) {
            security::enforce(pol, security::capability::net_listen, host_port(pc.listen_host, pc.listen_port));
            for (auto& t : targets) security::enforce(pol, security::capability::net_connect, t);
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "forge-serve proxy: %s\n", e.what()); return 1; }
    proxy px(p, pc);
    if (auto s = px.start(); !s) { std::fprintf(stderr, "forge-serve proxy: %s\n", s.err.what.c_str()); return 1; }
    try { server_lockdown(pol, "forge-serve proxy", host_port(pc.listen_host, px.port())); }
    catch (const std::exception& e) { px.stop(); std::fprintf(stderr, "forge-serve proxy: %s\n", e.what()); return 1; }
    std::printf("forge-serve proxy (%s, %s) on %s:%d, %zu backends\n", pc.mode == proxy_config::l4 ? "l4" : "l7", strategy_name(c.strat), pc.listen_host.c_str(), px.port(), p->size());
    std::fflush(stdout);
    static std::atomic<bool> quit{false};
    forge::os::on_signal(forge::os::sig::interrupt, [](int) { quit = true; });
    forge::os::on_signal(forge::os::sig::terminate, [](int) { quit = true; });
    health_checker hc(*p, std::uint64_t(std::stoll(arg(argc, argv, "health-ms", "2000"))) * 1000000ull);
    std::vector<std::unique_ptr<health_checker>> rhc; for (auto& r : pc.routes) rhc.push_back(std::make_unique<health_checker>(*r.target, 2'000'000'000ull));
    while (!quit) forge::os::sleep_ns(100'000'000);
    px.stop();
    std::printf("forge-serve proxy: served %llu, errors %llu\n", (unsigned long long)px.served(), (unsigned long long)px.errors());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "proxy") return run_proxy(argc, argv);
    if (argc < 2 || argv[1][0] == '-') {
        std::puts("usage: forge-serve <model.fpk|hf_dir|model.gguf> [--host 127.0.0.1] [--port 8080] [--max-active 8] [--ctx N] [--codec f32] [--watch] [--no-pin] [--numa-node N] [--mem-limit 6G|75%|off] [--max-queue N] [--kv-spill] [--policy F]\n       forge-serve proxy --backend host:port [...] [--mode l4|l7] [--strategy p2c|rr|wrr|lc|ewma|hash|kv] [--port 8080]");
        return 2;
    }
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    forge::net_init();
    try {
        // FSS: the policy is active before anything is loaded (model fs.read / integrity checks), the listen address
        // must be allowed, and lockdown() runs once the socket is bound and the model loaded (SBX-1).
        const auto pol = activate_policy(argc, argv);
        const std::string listen_host = arg(argc, argv, "host", "127.0.0.1");
        if (pol.active()) {
            security::enforce(pol, security::capability::net_listen, host_port(listen_host, std::stoi(arg(argc, argv, "port", "8080"))));
            if (has_flag(argc, argv, "watch") && !security::try_enforce(pol, security::capability::exec, forge::os::exe_path()))
                throw security::policy_violation("--watch reloads by re-executing forge-serve, but the security policy (" + pol.source + ") denies exec", "SBX-1");
        }
        std::string path = argv[1];
        namespace fs = std::filesystem;
        if (fs::is_directory(path) || path.ends_with(".gguf")) {   // import into a temp .fpk
            auto out = (fs::temp_directory_path() / (fs::path(path).filename().string() + ".serve.fpk")).string();
            import::import_options o; o.codec = arg(argc, argv, "codec", "f32");
            if (path.ends_with(".gguf")) import::import_gguf(path, out, o); else import::import_hf(path, out, o);
            path = out;
        }
        // --numa-node N: bind to a node before loading so weights/KV are first-touched node-local.
        if (auto nn = arg(argc, argv, "numa-node", ""); !nn.empty()) {
            auto r = forge::os::bind_thread_to_node(unsigned(std::stoul(nn)));
            std::printf("forge-serve: numa node %s%s\n", nn.c_str(), r ? "" : (" (bind failed: " + r.err.what + ")").c_str());
        }
        // Memory budget (docs/memory.md): --mem-limit overrides FORGE_MEM_LIMIT; 0/off disables.
        auto& host = mem::host_budget();
        if (auto ml = arg(argc, argv, "mem-limit", ""); !ml.empty()) {
            if (ml == "0" || ml == "off") host.set_limit(0);
            else if (auto v = mem::parse_size(ml, mem::detect_host_limits().effective())) host.set_limit(*v);
            else raise(errc::invalid_argument, "bad --mem-limit '{}' (use e.g. 6G, 512M or 75%)", ml);
        }
        mem::governor::instance().watch_pressure();
        auto m = inference::model::open(path);
        if (m->blueprint().session != "generative_sequence") raise(errc::unsupported, "model session '{}' is not generative", m->blueprint().session);
        inference::batch_options bo;
        bo.max_active = std::stoi(arg(argc, argv, "max-active", "8"));
        bo.session.context = std::stoll(arg(argc, argv, "ctx", "0"));
        if (host.limit()) bo.budget = &host;
        // --kv-spill: prefix KV evicted from memory goes to disk (FORGE_MEM_SPILL_DIR, capped by FORGE_MEM_SPILL_MAX)
        // and is read back when a prompt hits it, instead of being recomputed (docs/memory.md)
        std::unique_ptr<mem::spill_store> spill;
        if (has_flag(argc, argv, "kv-spill")) { spill = std::make_unique<mem::spill_store>(); bo.spill = spill.get(); }
        namespace fos = forge::os;
        // P-core pinning: on hybrid CPUs keep the main thread (and the scheduler/request threads it spawns, which
        // inherit the mask) on performance cores. --no-pin disables.
        if (!has_flag(argc, argv, "no-pin")) if (auto t = fos::cpu_topology_query(); t.hybrid) {
            auto r = fos::pin_thread_to_kind(fos::core_kind::performance);
            std::printf("forge-serve: hybrid CPU, pinned to %zu P-cores%s\n", t.of_kind(fos::core_kind::performance).size(), r ? "" : " (failed)");
        }
        server srv(m, bo, fs::path(argv[1]).filename().string(), std::size_t(std::stoull(arg(argc, argv, "max-queue", "0"))));
        // Memory pressure -> drop cached prefix KV sessions (moderate: keep 1, critical: keep none).
        fos::memory_pressure::instance().subscribe([&srv](fos::pressure_level l) {
            if (l == fos::pressure_level::normal) return;
            srv.evict_kv(l == fos::pressure_level::critical ? 0 : 1);
            fos::service::log(fos::log_level::warning, std::string("memory pressure ") + fos::pressure_name(l) + ": evicting prefix KV cache", "forge-serve");
        });
        forge_sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(std::uint16_t(std::stoi(arg(argc, argv, "port", "8080"))));
        if (inet_pton(AF_INET, arg(argc, argv, "host", "127.0.0.1").c_str(), &a.sin_addr) != 1) raise(errc::invalid_argument, "bad --host");
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) raise(errc::io_error, "bind failed: {}", std::strerror(errno));
        ::listen(s, 64);
        socklen_t al = sizeof a; getsockname(s, reinterpret_cast<sockaddr*>(&a), &al);
        server_lockdown(pol, "forge-serve", host_port(listen_host, ntohs(a.sin_port)));
        std::printf("forge-serve: %s on http://%s:%d (max_active %d, memory budget %s)\n", path.c_str(), arg(argc, argv, "host", "127.0.0.1").c_str(), ntohs(a.sin_port), bo.max_active,
                    host.limit() ? mem::format_size(host.limit()).c_str() : "off");
        std::fflush(stdout);
        fos::service::ready("serving " + path);
        // Hot reload: --watch re-execs forge-serve when the model file changes (systemd sees RELOADING=1 then READY=1).
        std::thread watcher;
        if (has_flag(argc, argv, "watch")) watcher = std::thread([&, s] {
            fos::file_watcher w; if (auto st = w.add(argv[1]); !st) { std::fprintf(stderr, "forge-serve: --watch: %s\n", st.err.what.c_str()); return; }
            for (;;) for (auto& e : w.poll(1000)) if (e.kind != fos::watch_kind::removed) {
                fos::sleep_ns(500'000'000);   // let the writer finish
                std::printf("forge-serve: %s changed, reloading\n", e.path.c_str()); std::fflush(stdout);
                fos::service::reloading();
#if !defined(_WIN32)
                forge_sock_close(s);
                std::vector<char*> av(argv, argv + argc); av.push_back(nullptr);
                ::execv(fos::exe_path().c_str(), av.data());
                std::fprintf(stderr, "forge-serve: re-exec failed: %s\n", std::strerror(errno));
#else
                std::fprintf(stderr, "forge-serve: hot reload re-exec is not supported on Windows; restart the service\n");
#endif
            }
        });
        if (watcher.joinable()) watcher.detach();
        for (;;) {
            forge_sock_t fd = ::accept(s, nullptr, nullptr);
            if (fd == FORGE_SOCK_BAD) continue;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            std::thread([&srv, fd] { srv.handle(fd); }).detach();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "forge-serve: error: %s\n", e.what());
        return 1;
    }
}
