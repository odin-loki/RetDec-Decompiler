#include <iterator>
#include <iostream>
#include <forge/os/process.hpp>
// forge — command-line front end: info | import | inspect | run | bench
#include <sys/stat.h>
#include <forge/import/gguf.hpp>
#include <forge/import/onnx.hpp>
#include <forge/simd/dispatch.hpp>
#include <forge/inference/inference.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <mutex>
#include <chrono>
#include <random>
#include <forge/simd/qdot_i8.hpp>
#include <forge/gpu/prims.hpp>
#include <forge/gpu/cuda.hpp>
#include <forge/script/script.hpp>
#include <forge/script/check.hpp>
#include <forge/script/local_ai.hpp>
#include <forge/security/security.hpp>

using namespace forge;

namespace {

struct args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> kv;
    std::set<std::string> flags;
    std::map<std::string, std::vector<std::string>> multi;  // repeated options (e.g. --pubkey a --pubkey b)
    std::vector<std::string> all(const std::string& k) const { auto it = multi.find(k); return it == multi.end() ? std::vector<std::string>{} : it->second; }
    std::string get(const std::string& k, const std::string& d = "") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    double num(const std::string& k, double d) const { auto it = kv.find(k); return it == kv.end() ? d : std::stod(it->second); }
    bool has(const std::string& f) const { return flags.count(f) || kv.count(f); }
};

args parse(int argc, char** argv, int from, std::set<std::string> boolean) {
    args a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0 || (s.size() == 2 && s[0] == '-')) {
            std::string k = s.substr(s[1] == '-' ? 2 : 1);
            if (auto eq = k.find('='); eq != std::string::npos) { a.kv[k.substr(0, eq)] = k.substr(eq + 1); a.multi[k.substr(0, eq)].push_back(k.substr(eq + 1)); continue; }
            if (boolean.count(k) || i + 1 >= argc) a.flags.insert(k); else { a.kv[k] = argv[++i]; a.multi[k].push_back(a.kv[k]); }
        } else a.pos.push_back(s);
    }
    return a;
}

int usage() {
    std::puts(
        "usage: forge <command> [options]\n"
        "  info                                   CPU features, lane target, grid backends, memory budget\n"
        "  import <hf_dir|model.gguf> -o <out.fpk> [--codec f32|f16|bf16|fp8_e4m3|fp8_e5m2|q8_0|q4_k|q4_g32|q8_g32|mxfp8|mxfp6|mxfp4|mxint8|nvfp4|tq2] [--template llama] [--emb-codec q8_0|q4_k|f32]\n"
        "      mixed precision (hf inputs): --codec q4_k_m|q5_k_m [--recipe forge|llama] [--output-tensor-type T] [--token-embedding-type T]\n"
        "      [--tensor-type REGEX=TYPE[,..]] [--pure] [--bf16-head]   (llama.cpp llama-quant.cpp rules; forge adds q6_k DeltaNet/attn_output, q8_0 head)\n"
        "  inspect <model.fpk> [--verify] [--tensors] [--blueprint]\n"
        "  run <model.fpk> (--prompt TEXT | --tokens 1,2,3) [-n 32] [--temp 0] [--top-k 0] [--top-p 1]\n"
        "      [--min-p 0] [--repeat-penalty 1] [--seed 0] [--ctx N] [--ids] [--logits out.f32] [--verify]\n"
        "      [--grammar file.gbnf] [--draft draft.fpk | --lookup [--ngram 3] [--ngram-min 1]] [--spec-k 0 (0 = planner)]\n"
        "      [--codec C (for hf/gguf inputs)]\n"
        "      (<model> may also be a Hugging Face directory or a .gguf: imported once into ~/.cache/forge)\n"
        "  bench <model.fpk> [-p 32] [-n 32]  |  bench --gemm 512  |  bench --kernels 1024 (MB, quantised matvec GB/s)\n"
        "  dump <model.fpk> <tensor> <out.f32>       decode one tensor to raw f32\n"
        "  keygen -o <name>                       Ed25519 key pair: <name>.key (secret seed), <name>.pub\n"
        "  sign <model.fpk> --key <name.key> [-o out.fpk]   add/extend the signature section\n"
        "  sign <file> --key <name.key> [-o file.sig] [--name N] [--detached]   any other file (plugin, script):\n"
        "      detached signature <file>.sig (Ed25519 over BLAKE3, binds the file name)\n"
        "  verify <model.fpk> [--pubkey <name.pub>]...      check hashes + signature (exit 1 on failure)\n"
        "  verify <file> [--sig file.sig] [--pubkey <name.pub>]...   check a detached signature (exit 1 on failure)\n"
        "  encrypt <in.fpk> -o <out.fpk> --key k.mkey | --new-key k.mkey   seal tensor data (XChaCha20-Poly1305);\n"
        "      load with FORGE_MODEL_KEY_FILE=k.mkey (or FORGE_MODEL_KEY=<hex>)\n"
        "  script <file.cpp> [args...]            run a FORGE script (interpreted, memory-safe C++23 subset)\n"
        "  script -e '<code>' [args...]          run a snippet;  script (no file): interactive REPL\n"
        "      [--tree] tree-walker only  [--debug] race/deadlock checks  [--timeout MS]  [--max-steps N]  [--heap BYTES]  [--no-proc] [--no-fs] [--no-net]\n"
        "      [--policy F]  FSS policy: capabilities from the policy, lockdown() before the script runs\n"
        "  security check [--policy F] [--json]   FSS controls: state, backend, verdict (exit 1 if a required one cannot be met)\n"
        "  run, bench, script and rpc-serve take --policy F (else $FORGE_SECURITY_POLICY, ./forge-security.json, the config\n"
        "      dir, $FORGE_SECURITY_PROFILE; with none of them no FSS profile is active and nothing changes; docs/security-api.md)\n"
        "  (strict loading: FORGE_REQUIRE_SIGNED=1 and/or FORGE_TRUSTED_KEYS=<hex>[,<hex>])");
    return 2;
}

// `forge run <hf-dir|model.gguf>`: import on the fly into a content-addressed cache and reuse it next time.
std::string resolve_model(const std::string& path, const std::string& codec) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(path) && !path.ends_with(".gguf") && !path.ends_with(".onnx")) return path;
    std::string key = fs::absolute(path).string() + "|" + codec + "|" + std::to_string(int(modelfile::format_version));
    auto stamp = [&](const fs::path& f) { key += std::format("|{}:{}:{}", f.filename().string(), (unsigned long long)fs::file_size(f),
                                                            (long long)fs::last_write_time(f).time_since_epoch().count()); };
    if (fs::is_directory(path)) { std::vector<fs::path> fl; for (auto& e : fs::directory_iterator(path)) if (e.is_regular_file()) fl.push_back(e.path());
                                  std::sort(fl.begin(), fl.end()); for (auto& f : fl) stamp(f); }
    else stamp(path);
    const char* home = std::getenv("HOME");
    fs::path dir = std::getenv("FORGE_CACHE") ? fs::path(std::getenv("FORGE_CACHE")) : fs::path(home ? home : "/tmp") / ".cache" / "forge";
    fs::create_directories(dir);
    auto h = hash::blake3::hex(hash::blake3::of({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()})).substr(0, 24);
    fs::path out = dir / (fs::path(path).filename().string() + "-" + codec + "-" + h + ".fpk");
    if (!fs::exists(out)) {
        import::import_options o; o.codec = codec;
        auto tmp = out.string() + ".partial";
        auto r = path.ends_with(".gguf") ? import::import_gguf(path, tmp, o) : path.ends_with(".onnx") ? import::import_onnx(path, tmp, o) : import::import_hf(path, tmp, o);
        fs::rename(tmp, out);
        std::fprintf(stderr, "[imported %s (%s, %zu tensors) -> %s]\n", path.c_str(), r.template_name.c_str(), r.tensors, out.c_str());
    } else std::fprintf(stderr, "[using cached %s]\n", out.c_str());
    return out.string();
}

std::vector<int> parse_ids(const std::string& s) {
    std::vector<int> r; std::size_t p = 0;
    while (p < s.size()) { auto e = s.find(',', p); if (e == std::string::npos) e = s.size(); if (e > p) r.push_back(std::stoi(s.substr(p, e - p))); p = e + 1; }
    return r;
}

// `forge security check [--policy F] [--json]`: what the FORGE Security Standard controls would be under the policy
// (load_policy order: --policy, $FORGE_SECURITY_POLICY, ./forge-security.json, the user config dir). Exit 0 when
// every required control can be met, 1 when one cannot, 2 for usage errors or a bad policy.
int cmd_security(const args& a) {
    if (a.pos.empty() || a.pos[0] != "check") return usage();
    security::policy p;
    try { p = security::load_policy(a.get("policy")); }
    catch (const security::policy_violation& e) { std::fprintf(stderr, "forge security: %s\n", e.what()); return 2; }
    auto r = security::check(p);
    std::fputs(a.has("json") ? (r.json() + "\n").c_str() : r.text().c_str(), stdout);
    return r.ok ? 0 : 1;
}

// The FSS policy for a command (--policy F, else the load_policy() search). An active one becomes the process's policy
// for enforce() (model loads check fs.read and integrity.models through it); profile none changes nothing.
security::policy activate_policy(const std::string& cli_path) {
    auto p = security::load_policy(cli_path);
    if (p.active()) security::set_active_policy(p);
    return p;
}

// SBX-1 for a server: the listen address must be allowed (net_listen), then lockdown() once the sockets are bound and
// everything is loaded, before the first request. Audited; throws permission_denied / sandbox_error.
void server_lockdown(const security::policy& p, const char* who, const std::string& listen) {
    if (!p.active()) return;
    auto r = security::lockdown(p);
    for (auto& c : r.controls)
        if (c.verdict != "ok") std::fprintf(stderr, "%s: security %s %s: %s%s\n", who, c.id.c_str(), c.name.c_str(), c.state.c_str(), c.detail.empty() ? "" : (" (" + c.detail + ")").c_str());
    security::audit("sandbox", security::decision::allow, "SBX-1", listen, std::string(who) + ": locked down (profile " + security::name(p.prof) + ", " + p.source + ") before serving");
}

int cmd_info() {
    auto& f = simd::detect();
    std::printf("cpu      %s\nfeatures %s\nlane     %s\n", f.brand.c_str(), f.summary().c_str(), simd::active().target);
    for (auto& b : gpu::probe_backends()) std::printf("grid     %-10s %s  %s\n", gpu::name(b.kind), b.available ? "yes" : "no ", b.detail.c_str());
    std::fputs(mem::governor::instance().describe().c_str(), stdout);
    return 0;
}

int cmd_import(const args& a) {
    if (a.pos.empty() || a.get("o").empty()) return usage();
    import::import_options o; o.codec = a.get("codec", "f32"); o.template_name = a.get("template"); o.emb_codec = a.get("emb-codec");
    o.recipe.recipe = a.get("recipe", "forge");
    o.recipe.output_type = a.get("output-tensor-type");
    o.recipe.token_embedding_type = a.get("token-embedding-type");
    o.recipe.pure = a.has("pure");
    o.recipe.head_bf16 = a.has("bf16-head");
    for (std::string tt = a.get("tensor-type"); !tt.empty();) {   // REGEX=TYPE[,REGEX=TYPE...]
        auto c = tt.find(','); o.recipe.tensor_types.push_back(import::parse_tensor_type(tt.substr(0, c)));
        tt = c == std::string::npos ? "" : tt.substr(c + 1);
    }
    activate_policy(a.get("policy"));   // FSS: importers check fs_read (input) / fs_write (output) under an active policy
    auto r = a.pos[0].ends_with(".gguf") ? import::import_gguf(a.pos[0], a.get("o"), o) : a.pos[0].ends_with(".onnx") ? import::import_onnx(a.pos[0], a.get("o"), o) : import::import_hf(a.pos[0], a.get("o"), o);
    std::printf("imported %s via template '%s': %zu tensors, %zu bytes -> %s\n", r.arch.c_str(), r.template_name.c_str(), r.tensors, r.bytes, a.get("o").c_str());
    return 0;
}

int cmd_inspect(const args& a) {
    if (a.pos.empty()) return usage();
    modelfile::reader r(a.pos[0]);
    std::printf("file      %s (%zu bytes)\nmanifest  %s\n", a.pos[0].c_str(), r.file_size(), json::dump(r.manifest()).c_str());
    for (auto& [n, s] : r.sections()) std::printf("section   %-10s offset %10llu size %10llu blake3 %s\n", n.c_str(),
                                                   (unsigned long long)s.offset, (unsigned long long)s.size, hash::blake3::hex(s.hash).substr(0, 16).c_str());
    auto bp = modelfile::parse_blueprint(r.section_text("blueprint"));
    std::printf("blueprint \"%s\" opset %d session %s, %zu params, %zu states, %zu tensors\n", bp.name.c_str(), bp.opset,
                bp.session.c_str(), bp.params.size(), bp.states.size(), r.tensors().size());
    if (a.has("tensors")) for (auto& [n, e] : r.tensors()) std::printf("  %-28s %-9s %s\n", n.c_str(), e.codec.c_str(), shape_str(e.shape).c_str());
    if (a.has("blueprint")) std::printf("%.*s\n", int(r.section_text("blueprint").size()), r.section_text("blueprint").data());
    if (a.has("verify")) {
        auto bad = r.verify();
        if (bad.empty()) std::puts("verify    ok (all sections and tensors match BLAKE3)");
        else { for (auto& b : bad) std::printf("verify    MISMATCH %s\n", b.c_str()); return 1; }
    }
    return 0;
}

int cmd_dump(const args& a) {  // decode one tensor to raw little-endian f32
    if (a.pos.size() < 3) return usage();
    modelfile::reader r(a.pos[0]);
    auto t = r.load(a.pos[1]);
    std::ofstream f(a.pos[2], std::ios::binary);
    f.write(reinterpret_cast<const char*>(t.data.data()), std::streamsize(t.size() * 4));
    std::printf("%s %s %s -> %s\n", a.pos[1].c_str(), r.entry(a.pos[1]).codec.c_str(), shape_str(t.shape).c_str(), a.pos[2].c_str());
    return 0;
}

int cmd_keygen(const args& a) {
    if (a.get("o").empty()) return usage();
    auto k = modelfile::generate_keypair();
    auto base = a.get("o");
    {   // FSS-FIO-1: the secret is 0600 from its first byte (exclusive temp file + rename), never world-readable
        auto hex = modelfile::detail::to_hex(k.seed) + "\n";
        auto w = os::fs::write_file_atomic(base + ".key", hex, 0600); crypto_wipe(hex.data(), hex.size());
        if (!w) throw std::runtime_error("cannot write " + base + ".key: " + w.err.what);
    }
    { std::ofstream f(base + ".pub"); f << modelfile::detail::to_hex(k.pub) << "\n"; }
    std::printf("key id %s\npublic %s\nwrote %s.key (keep secret) and %s.pub\n", modelfile::key_id(k.pub).c_str(), modelfile::detail::to_hex(k.pub).c_str(), base.c_str(), base.c_str());
    return 0;
}
// .fpk files carry an embedded signature; anything else (plugins, scripts, ...) or --detached gets `<file>.sig`
// (FSS INT-2, security/integrity.hpp).
bool is_fpk(const std::string& f) {
    std::ifstream s(f, std::ios::binary); char m[8] = {};
    return s.read(m, 8) && std::memcmp(m, modelfile::magic, 8) == 0;
}
int cmd_sign(const args& a) {
    if (a.pos.empty() || a.get("key").empty()) return usage();
    auto k = modelfile::load_secret_key(a.get("key"));
    if (a.has("detached") || !is_fpk(a.pos[0])) {   // forge sign FILE --key K [-o FILE.sig] [--name NAME]
        const std::string& f = a.pos[0];
        auto bytes = os::read_file(f);
        if (!bytes) throw std::runtime_error("cannot read " + f + ": " + bytes.err.what);
        auto out = a.get("o").empty() ? security::detached_sig_path(f) : a.get("o");
        std::string old; if (auto e = os::read_file(out)) old = e.value;   // extend an existing .sig (another signer)
        auto nm = a.get("name").empty() ? security::signed_name(f) : a.get("name");
        auto text = security::detached_sign({reinterpret_cast<const std::uint8_t*>(bytes.value.data()), bytes.value.size()}, nm, k.seed, old);
        auto tmp = out + ".tmp";
        if (auto w = os::write_file(tmp, text); !w) throw std::runtime_error("cannot write " + tmp + ": " + w.err.what);
        if (auto rs = os::rename(tmp, out); !rs) { os::remove(tmp); throw std::runtime_error("cannot replace " + out + ": " + rs.err.what); }
        std::printf("signed %s (as '%s') with key %s -> %s\n", f.c_str(), nm.c_str(), modelfile::key_id(k.pub).c_str(), out.c_str());
        return 0;
    }
    auto out = a.get("o").empty() ? a.pos[0] : a.get("o");
    modelfile::sign_file(a.pos[0], out, k);
    std::printf("signed %s with key %s -> %s\n", a.pos[0].c_str(), modelfile::key_id(k.pub).c_str(), out.c_str());
    return 0;
}
int cmd_encrypt(const args& a) {   // forge encrypt <in.fpk> -o <out.fpk> [--key k.mkey | --new-key k.mkey]
    if (a.pos.empty() || a.get("o").empty() || (a.get("key").empty() && a.get("new-key").empty())) return usage();
    modelfile::model_key k;
    if (!a.get("new-key").empty()) {
        k = modelfile::generate_model_key();
        auto hex = modelfile::detail::hex(k.data(), 32) + "\n";   // FSS-FIO-1: 0600 from the first byte
        auto st = os::fs::write_file_atomic(a.get("new-key"), hex, 0600); crypto_wipe(hex.data(), hex.size());
        if (!st) throw std::runtime_error("cannot write " + a.get("new-key") + ": " + st.err.what);
    } else {
        activate_policy(a.get("policy"));
        security::require_private_file("model key", a.get("key"), security::file_kind::secret);   // FSS-FIO-2
        std::ifstream f(a.get("key")); std::string h, w; while (f >> w) h += w;
        auto b = modelfile::detail::unhex(h); if (b.size() != 32) throw std::runtime_error("model key must be 64 hex chars");
        std::memcpy(k.data(), b.data(), 32);
    }
    modelfile::encrypt_file(a.pos[0], a.get("o"), k);
    std::printf("encrypted %s -> %s (key id %s)\n", a.pos[0].c_str(), a.get("o").c_str(), modelfile::detail::enc_key_id(k).c_str());
    return 0;
}
int cmd_verify(const args& a) {
    if (a.pos.empty()) return usage();
    std::vector<std::string> trusted;
    for (auto& p : a.all("pubkey")) trusted.push_back(modelfile::load_public_key(p));
    if (a.has("detached") || !a.get("sig").empty() || !is_fpk(a.pos[0])) {   // forge verify FILE [--sig FILE.sig] [--pubkey K]...
        auto rep = security::detached_verify_file(a.pos[0], trusted, a.get("sig"));
        for (auto& k : rep.valid_keys) std::printf("signature ok   key %s\n", k.c_str());
        for (auto& k : rep.invalid_keys) std::printf("signature BAD  key %s\n", k.c_str());
        std::printf("%s: %s\n", a.pos[0].c_str(), rep.trusted ? "VERIFIED" : rep.message.c_str());
        return rep.trusted ? 0 : 1;
    }
    modelfile::reader r(a.pos[0]);
    auto bad = r.verify();
    for (auto& b : bad) std::printf("hash MISMATCH %s\n", b.c_str());
    auto rep = modelfile::verify_signature(r, trusted);
    for (auto& k : rep.valid_keys) std::printf("signature ok   key %s\n", k.c_str());
    for (auto& k : rep.invalid_keys) std::printf("signature BAD  key %s\n", k.c_str());
    std::printf("%s: %s\n", a.pos[0].c_str(), bad.empty() && rep.trusted ? "VERIFIED" : rep.message.c_str());
    return bad.empty() && rep.trusted ? 0 : 1;
}

// one_shot models: --input name=file.f32 (raw little-endian f32, repeatable), --out prefix -> prefix.<output>.f32
int run_one_shot(inference::session& s, const args& a) {
    std::map<std::string, std::vector<float>> in; i64 seq = 1;
    const json::value* decl = s.manifest().find("onnx_inputs");
    for (auto& spec : a.all("input")) {
        auto eq = spec.find('=');
        if (eq == std::string::npos) raise(errc::invalid_argument, "--input expects name=file, got '{}'", spec);
        auto name = spec.substr(0, eq); auto bytes = import::read_file(spec.substr(eq + 1));
        if (bytes.size() % 4) raise(errc::invalid_argument, "input file {} is not raw f32", spec.substr(eq + 1));
        std::vector<float> v(bytes.size() / 4); std::memcpy(v.data(), bytes.data(), bytes.size());
        if (decl) for (auto& d : decl->arr()) {
            if (d["onnx"].str() != name && d["name"].str() != name) continue;
            name = d["name"].str(); i64 k = 1; bool sym = false;
            for (auto& x : d["shape"].arr()) { if (x.num() < 0) sym = true; else k *= i64(x.num()); }
            if (sym && k) seq = i64(v.size()) / k;
        }
        in[name] = std::move(v);
    }
    auto out = s.infer(in, seq);
    auto prefix = a.get("out");
    for (auto& [k, v] : out) {
        if (prefix.empty()) { std::printf("%s:", k.c_str()); for (auto x : v) std::printf(" %.6g", x); std::printf("\n"); continue; }
        std::ofstream f(prefix + "." + k + ".f32", std::ios::binary);
        f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * 4));
    }
    return 0;
}

int cmd_run(const args& a) {
    if (a.pos.empty()) return usage();
    activate_policy(a.get("policy"));
    inference::session_options so; so.context = i64(a.num("ctx", 0)); so.verify = a.has("verify");
    inference::session s(resolve_model(a.pos[0], a.get("codec", "f32")), so);
    if (s.blueprint().session != "generative_sequence") return run_one_shot(s, a);
    std::vector<int> prompt;
    if (a.kv.count("tokens")) prompt = parse_ids(a.get("tokens"));
    else if (a.kv.count("prompt")) {
        if (!s.tokenizer()) raise(errc::not_found, "model has no tokenizer; use --tokens");
        prompt = s.tokenizer()->encode(a.get("prompt"));
        if (s.tokenizer()->add_bos() && !a.has("no-bos")) prompt.insert(prompt.begin(), s.tokenizer()->bos());
    } else return usage();
    inference::sampler_config sc;
    sc.temperature = float(a.num("temp", 0)); sc.top_k = int(a.num("top-k", 0)); sc.top_p = float(a.num("top-p", 1));
    sc.min_p = float(a.num("min-p", 0)); sc.repetition_penalty = float(a.num("repeat-penalty", 1)); sc.seed = std::uint64_t(a.num("seed", 0));
    inference::sampler smp(sc);
    bool ids = a.has("ids") || !s.tokenizer();
    if (a.kv.count("logits")) {  // dump prefill logits for golden comparison
        auto l = s.forward(prompt);
        std::ofstream f(a.get("logits"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(l.data()), std::streamsize(l.size() * 4));
        s.reset();
    }
    std::unique_ptr<inference::grammar_constraint> gc;
    if (a.kv.count("grammar")) {
        if (!s.tokenizer()) raise(errc::unsupported, "--grammar needs a model tokenizer");
        auto g = std::make_shared<inference::grammar>(import::read_file(a.get("grammar")));
        gc = std::make_unique<inference::grammar_constraint>(g, *s.tokenizer(), int(s.vocab()), s.tokenizer()->eos());
        smp.filter = [&](std::span<float> l) { gc->apply(l); }; smp.allow = [&](int t) { return gc->allows(t); };
    }
    auto show = [&](int t) {
        if (gc && !gc->accept(t)) return false;
        if (ids) std::printf("%d ", t); else std::printf("%s", s.tokenizer()->decode_one(t).c_str());
        std::fflush(stdout);
        return !(gc && gc->finished());
    };
    // Speculative decoding: --draft model.fpk or --lookup (n-gram drafts from the prompt and output).
    // --spec-k N: N drafts per pass; 0 (default) = chosen per pass by the planner from measured costs.
    // Not with --grammar: the constraint advances one token at a time.
    if ((a.kv.count("draft") || a.has("lookup")) && !gc) {
        inference::speculative_stats sst;
        int k = int(a.num("spec-k", 0)), n = int(a.num("n", 32));
        if (a.kv.count("draft")) {
            inference::session d(resolve_model(a.get("draft"), a.get("codec", "f32")), so);
            inference::speculative_generate(s, d, prompt, n, smp, k, show, &sst);
        } else {
            int ng = std::max(1, int(a.num("ngram", 3)));
            inference::lookup_generate(s, prompt, n, smp, k, ng, show, &sst, nullptr, int(a.num("ngram-min", 1)));
        }
        std::printf("\n");
        std::string pos;
        for (std::size_t i = 0; i < 8 && sst.pos_tried[i]; ++i) pos += std::format("{}{:.0f}%", i ? " " : "", 100.0 * double(sst.pos_won[i]) / double(sst.pos_tried[i]));
        std::fprintf(stderr, "[speculative: %zu tokens in %.3fs = %.1f tok/s decode (prefill %.3fs); %zu passes, %zu with drafts, %zu drafted, acceptance %.0f%% (by position: %s), %.2f tokens/pass, %zu replay rows, %zu probes]\n",
                     sst.new_tokens, sst.seconds, sst.decode_tps(), sst.prefill_seconds, sst.passes, sst.rounds, sst.drafted, 100 * sst.acceptance(), pos.c_str(),
                     sst.tokens_per_pass(), sst.replayed, sst.probes);
        return 0;
    }
    inference::generate_stats st;
    std::vector<int> out = s.generate(prompt, int(a.num("n", 32)), smp, show, &st);
    std::printf("\n");
    std::fprintf(stderr, "[%zu prompt tokens in %.3fs, %zu new tokens in %.3fs = %.1f tok/s; plans %zu compiled, %zu reused]\n",
                 st.prompt_tokens, st.prefill_s, st.new_tokens, st.decode_s, st.new_tokens > 1 ? double(st.new_tokens - 1) / st.decode_s : 0.0,
                 s.plans().misses(), s.plans().hits());
    return 0;
}

int cmd_bench_gemm(int n) {
    std::vector<float> A(std::size_t(n) * n, 0.5f), B(std::size_t(n) * n, 0.25f), C(std::size_t(n) * n);
    for (const char* t : {"scalar", "avx2", "avx512", "neon", "sve", "rvv"}) {
        auto* k = simd::by_name(t); if (!k) continue;
        k->gemm_nt(A.data(), B.data(), C.data(), std::size_t(n), std::size_t(n), std::size_t(n));
        auto t0 = std::chrono::steady_clock::now(); int reps = 0;
        do { k->gemm_nt(A.data(), B.data(), C.data(), std::size_t(n), std::size_t(n), std::size_t(n)); ++reps; }
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / reps;
        std::printf("simd::%-7s gemm_nt %d^3: %8.2f GFLOP/s (1 thread)\n", t, n, 2.0 * n * n * n / s / 1e9);
    }
    auto t0 = std::chrono::steady_clock::now(); int reps = 0;
    do { gpu::prims::gemm_nt(A.data(), B.data(), C.data(), std::size_t(n), std::size_t(n), std::size_t(n)); ++reps; }
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / reps;
    std::printf("gpu::prims gemm_nt %d^3: %8.2f GFLOP/s (all threads)\n", n, 2.0 * n * n * n / s / 1e9);
    return 0;
}

// Quantised matvec bandwidth per weight format: a matrix of `mb` MB (well past the last-level cache) times 1..8
// activation vectors, on the integer kernels and on the f32 decode + dot path, with every pool thread.
int cmd_bench_kernels(int mb) {
    using namespace forge::graph;
    const std::size_t k = 4096;
    std::mt19937 rng(1);
    std::printf("quantised matvec, k=%zu, %d MB matrix, %u threads, integer kernels: %s, prefetch %d B\n", k, mb, task::pool::global().size(),
                simd::qi8_active().target, simd::qi8_prefetch_bytes);
    for (const char* cn : {"q4_k", "q5_k", "q6_k", "q8_0"}) {
        const auto* c = codecs::find(cn); const int fmt = qweights::fmt_of(c);
        const std::size_t rb = c->bytes(k), n = std::size_t(mb) * 1000000 / rb, bytes = n * rb;
        std::vector<std::uint8_t> w(bytes);
        for (auto& b : w) b = std::uint8_t(rng());
        const std::uint16_t one = 0x2000;   // a small finite f16 for every scale field
        for (std::size_t j = 0; j < n * (k / c->block); ++j) {
            std::uint8_t* blk = w.data() + j * c->block_bytes;
            if (c->name == "q6_k") std::memcpy(blk + 208, &one, 2);
            else { std::memcpy(blk, &one, 2); if (c->name != "q8_0") std::memcpy(blk + 2, &one, 2); }
        }
        std::vector<float> x(8 * k), y(8 * n);
        for (auto& v : x) v = float(rng() % 2001) / 1000.f - 1.f;
        auto timeit = [&](auto&& f) {
            f(); double best = 1e30;
            for (int rep = 0; rep < 5; ++rep) {
                auto t0 = std::chrono::steady_clock::now(); f();
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            return best;
        };
        std::printf("  %-5s", cn);
        for (std::size_t nv : {1, 2, 4, 8}) {
            double s = timeit([&] { qweights::gemm_nt_i8(fmt, w.data(), rb, n, x.data(), nv, k, y.data()); });
            std::printf("  int8 nv=%zu %6.1f GB/s", nv, double(bytes) / s / 1e9);
        }
        double s = timeit([&] {
            task::parallel_for(n, 8, [&](std::size_t b, std::size_t e) {
                const auto& ker = simd::active();
                for (std::size_t j = b; j < e; ++j) y[j] = ker.qdot(fmt, w.data() + j * rb, x.data(), k, c->decode, c->block, c->block_bytes);
            });
        });
        std::printf("  | f32 nv=1 %6.1f GB/s\n", double(bytes) / s / 1e9);
    }
    return 0;
}

int cmd_bench(const args& a) {
    if (a.kv.count("gemm")) return cmd_bench_gemm(int(a.num("gemm", 512)));
    if (a.kv.count("kernels")) return cmd_bench_kernels(int(a.num("kernels", 1024)));
    if (a.pos.empty()) return usage();
    activate_policy(a.get("policy"));
    inference::session s(resolve_model(a.pos[0], a.get("codec", "f32")));
    int p = int(a.num("p", 32)), n = int(a.num("n", 32));
    std::vector<int> prompt(std::size_t(p), 1);
    inference::sampler smp;
    // --warmup N (default 1): untimed short runs first, so one-off weight uploads / page-ins of the mmapped
    // model and kernel loads are not counted as prefill or decode time (llama-bench does the same).
    for (int w = int(a.num("warmup", 1)); w > 0; --w) {
        std::vector<int> wp(std::size_t(std::min(p, 64)), 1); inference::generate_stats ws;
        s.generate(wp, 2, smp, {}, &ws); s.reset();
    }
    // GPU driver calls (CUDA/HIP): a prefill-only run (1 new token) and the full run, so the decode share is
    // (full - prefill-only) / (n - 1) per token. "launches" are kernels issued directly, "captured" are kernels
    // recorded into a CUDA graph (replayed by one graph launch), instantiates are graphs (re)built, node_updates are
    // kernel nodes patched in place on replay, dtoh/htod/dtod are copies (dtod a copy kernel under graphs), syncs are stream syncs.
    using snap = gpu::cuda_hip::call_counters::snapshot;
    auto now = [] { return gpu::cuda_hip::counters().read(); };
    auto diff = [](const snap& a, const snap& b) {
        return snap{b.launches - a.launches, b.captured - a.captured, b.graph_launches - a.graph_launches, b.dtoh - a.dtoh,
                    b.htod - a.htod, b.dtod - a.dtod, b.syncs - a.syncs, b.instantiates - a.instantiates, b.node_updates - a.node_updates, b.replayed_runs - a.replayed_runs};
    };
    snap c0 = now();
    { inference::generate_stats ps; s.generate(prompt, 1, smp, {}, &ps, false); s.reset(); }
    snap pre = diff(c0, now());
    inference::generate_stats st;
    snap c1 = now();
    s.generate(prompt, n, smp, {}, &st, /*stop_at_eos=*/false);   // bench: always decode n tokens
    snap all = diff(c1, now());
    std::printf("prefill %d tokens: %.1f tok/s\ndecode  %zu tokens: %.1f tok/s\n", p, p / st.prefill_s, st.new_tokens,
                st.new_tokens > 1 ? double(st.new_tokens - 1) / st.decode_s : 0.0);
    if (all.launches + all.captured > 0) {
        auto pr = [](const char* what, const snap& c, double per) {
            std::printf("%s: launches %.1f captured %.1f graph_launches %.1f instantiates %.1f node_updates %.1f replayed %.2f dtoh %.1f htod %.1f dtod %.1f syncs %.1f\n", what,
                        c.launches / per, c.captured / per, c.graph_launches / per, double(std::int64_t(c.instantiates)) / per, double(std::int64_t(c.node_updates)) / per, double(std::int64_t(c.replayed_runs)) / per, c.dtoh / per, c.htod / per, c.dtod / per, c.syncs / per);
        };
        pr("gpu prefill (total)", pre, 1.0);
        if (st.new_tokens > 1) pr("gpu decode (per token)", diff(pre, all), double(st.new_tokens - 1));
    }
    return 0;
}

// forge rpc-serve [--port 7000] [--bind 127.0.0.1] [--device cpu|vulkan|cuda:0...]
//   [--token T | --token-file F]... [--authorized-keys FILE | --pubkey HEX|FILE]...
//   [--tls-cert C --tls-key K [--tls-client-ca CA [--tls-client-map FILE]...]] [--quiet]
//   [--allow-scripts [--script-timeout MS] [--script-mem-mb N] [--script-no-proc] [--script-no-fs] [--script-no-net]]
// Without tokens/keys it is unauthenticated (keep it on loopback). Scripts require auth. See docs/rpc.md.
int cmd_rpc_serve(const args& a) {
    // FSS (docs/rpc.md "Security policy"): with an active policy the listen address must be allowed, script children
    // need exec, and the server locks itself down after binding, before it accepts a request.
    const auto pol = activate_policy(a.get("policy"));
    std::string why, dev = a.get("device", "cpu");
    auto reg = graph::device_registry(dev, &why, false);
    if (!reg && dev != "cpu") std::fprintf(stderr, "forge: %s; serving on cpu\n", why.c_str());
    graph::rpc::server_security sec;
    auto toks = a.all("token"); if (toks.empty() && a.has("token")) toks.push_back(a.get("token"));
    for (auto& t : toks) {   // "--token T" (full access) or "--token 'allow=device T'" (a token-file line)
        if (!t.starts_with("allow=")) { sec.tokens.push_back(t); continue; }
        for (auto& e : graph::rpc::parse_tokens(t, "--token")) sec.tokens.push_back(std::move(e));
    }
    auto files = a.all("token-file"); if (files.empty() && a.has("token-file")) files.push_back(a.get("token-file"));
    // FSS-FIO-2: secrets (token files, the TLS key) must be owner-only and trusted lists (authorized keys, client
    // maps) writable only by their owner; profile strict refuses otherwise, other profiles warn (audit).
    for (auto& f : files) security::require_private_file(pol, "--token-file", f, security::file_kind::secret);
    for (auto& f : files) {   // "[allow=device,scripts] <token> [name]" per line (graph/rpc_secure.hpp)
        auto r = os::read_file(f); if (!r) throw std::runtime_error("cannot read " + f);
        struct wipe_on_exit { std::string& s; ~wipe_on_exit() { if (!s.empty()) crypto_wipe(s.data(), s.size()); } } w{r.value};
        auto ts = graph::rpc::parse_tokens(r.value, f);
        if (ts.empty()) throw std::runtime_error(f + ": no tokens");
        for (auto& e : ts) sec.tokens.push_back(std::move(e));
    }
    auto keyfiles = a.all("authorized-keys"); if (keyfiles.empty() && a.has("authorized-keys")) keyfiles.push_back(a.get("authorized-keys"));
    for (auto& f : keyfiles) {   // "[allow=device,scripts] <hex> [comment]" per line (graph/rpc_secure.hpp)
        security::require_private_file(pol, "--authorized-keys", f, security::file_kind::trusted);
        auto r = os::read_file(f); if (!r) throw std::runtime_error("cannot read " + f);
        try { for (auto& k : graph::rpc::parse_authorized_keys(r.value)) sec.authorized_keys.push_back(std::move(k)); }
        catch (const std::exception& e) { throw std::runtime_error(f + ": " + e.what()); }
    }
    auto pubs = a.all("pubkey"); if (pubs.empty() && a.has("pubkey")) pubs.push_back(a.get("pubkey"));
    for (auto& p : pubs) sec.authorized_keys.push_back(p.size() == 64 && p.find('/') == std::string::npos && !std::filesystem::exists(p) ? p : modelfile::load_public_key(p));
    sec.tls.cert_file = a.get("tls-cert"); sec.tls.key_file = a.get("tls-key"); sec.tls.client_ca_file = a.get("tls-client-ca");
    if (sec.tls.cert_file.empty() != sec.tls.key_file.empty()) throw std::runtime_error("--tls-cert and --tls-key go together");
    security::require_private_file(pol, "--tls-key", sec.tls.key_file, security::file_kind::secret);
    if (!sec.tls.client_ca_file.empty() && sec.tls.cert_file.empty()) throw std::runtime_error("--tls-client-ca needs --tls-cert and --tls-key");
    auto maps = a.all("tls-client-map"); if (maps.empty() && a.has("tls-client-map")) maps.push_back(a.get("tls-client-map"));
    for (auto& f : maps) {   // "[allow=...] sha256:<hex>|cn:<name> [comment]" per line (graph/rpc_secure.hpp)
        security::require_private_file(pol, "--tls-client-map", f, security::file_kind::trusted);
        auto r = os::read_file(f); if (!r) throw std::runtime_error("cannot read " + f);
        auto rs = graph::rpc::parse_client_map(r.value, f);
        if (rs.empty()) throw std::runtime_error(f + ": no rules (an empty client map would accept no certificate)");
        for (auto& x : rs) sec.client_map.push_back(std::move(x));
    }
    if (!sec.client_map.empty() && sec.tls.client_ca_file.empty()) throw std::runtime_error("--tls-client-map needs --tls-client-ca");
    sec.allow_scripts = a.has("allow-scripts");
    std::string bind = a.get("bind", "127.0.0.1");
    if (pol.active()) {
        security::enforce(pol, security::capability::net_listen, (bind.find(':') != std::string::npos ? "[" + bind + "]" : bind) + ":" + std::to_string(int(a.num("port", 7000))));
        if (sec.allow_scripts && !security::try_enforce(pol, security::capability::exec, os::exe_path()))
            throw security::policy_violation("rpc-serve --allow-scripts starts a `forge script` child per script, but the security policy (" + pol.source + ") denies exec", "SBX-1");
    }
    if (!sec.auth_required() && sec.tls.client_ca_file.empty() && bind != "127.0.0.1" && bind != "::1" && bind != "localhost")
        std::fprintf(stderr, "forge rpc: WARNING: no authentication and bound to %s; anyone who can reach it can use it\n", bind.c_str());
    graph::rpc::server srv(reg.get(), int(a.num("port", 7000)), bind, sec);
    if (!a.has("quiet")) srv.log = [](const std::string& m) { std::fprintf(stderr, "forge rpc: %s\n", m.c_str()); };
    if (sec.allow_scripts) {
        auto def_tmo = std::uint64_t(a.num("script-timeout", 60000));
        auto mem_mb = std::uint64_t(a.num("script-mem-mb", 2048));
        bool no_proc = a.has("script-no-proc"), no_fs = a.has("script-no-fs"), no_net = a.has("script-no-net");
        std::string self = os::exe_path();
        if (self.empty()) throw std::runtime_error("rpc-serve --allow-scripts: cannot find this executable to start script children");
        // children load the same policy (an explicit --policy is passed on as an absolute path; otherwise they find it
        // the way this process did) and lock themselves down before running the script
        const std::string child_policy = a.get("policy").empty() ? std::string() : os::absolute(a.get("policy"));
        // Each remote script runs in its own child process (`forge script --rpc-child`), so ForgeScript's OS sandbox
        // (seccomp / Capsicum / pledge / sandbox_init / restricted token) applies to the script without touching the
        // server's own sockets, and a crash, leak or runaway loop dies with the child. The source goes over stdin.
        srv.signed_script_handler = [self, def_tmo, mem_mb, no_proc, no_fs, no_net, child_policy](const std::string& src, const std::string& name, const std::vector<std::string>& argv, std::uint32_t tmo,
                                                                                                const std::string& signature) {
            auto t = tmo ? std::min<std::uint64_t>(tmo, def_tmo) : def_tmo;
            os::spawn_spec sp;
            sp.argv = {self, "script", "--rpc-child", name.empty() ? "<remote>" : name, "--timeout", std::to_string(t)};
            if (no_proc) sp.argv.push_back("--no-proc");
            if (no_fs) sp.argv.push_back("--no-fs");
            if (no_net) sp.argv.push_back("--no-net");
            if (!child_policy.empty()) { sp.argv.push_back("--policy"); sp.argv.push_back(child_policy); }
            // the client's detached signature (FSS INT-2), hex so it survives the command line; the child verifies it
            if (!signature.empty()) { sp.argv.push_back("--rpc-sig"); sp.argv.push_back(security::detail::hex({reinterpret_cast<const std::uint8_t*>(signature.data()), signature.size()})); }
            // Windows cannot deny fs/net inside a running process (only run_sandboxed's AppContainer can), so there the
            // child applies what it can (job object) and the interpreter checks the rest.
#if defined(_WIN32)
            if (no_proc || no_fs || no_net) { sp.argv.push_back("--os-sandbox"); sp.argv.push_back("best-effort"); }
#else
            if (no_proc || no_fs || no_net) { sp.argv.push_back("--os-sandbox"); sp.argv.push_back("require"); }
#endif
            sp.argv.push_back("--args"); for (auto& x : argv) sp.argv.push_back(x);
            sp.stdin_data = src; sp.timeout_ms = t + 5000; sp.max_output = 32u << 20;
            sp.limits.memory_bytes = mem_mb << 20;
            sp.env_unset = {"FORGE_SCRIPT_CACHE_SIGN_SEED", "FORGE_RPC_TOKEN", "FORGE_RPC_KEY", "FORGE_RPC_TLS_KEY"};
            auto r = os::spawn_wait(sp);
            if (!r) return graph::rpc::server::script_result{false, "RuntimeError", "could not start script child: " + r.err.what, ""};
            auto& pr = r.value;
            if (pr.timed_out) return graph::rpc::server::script_result{false, "TimeoutError", "script exceeded " + std::to_string(t) + " ms", pr.out};
            if (pr.exit_code == 0) return graph::rpc::server::script_result{true, "", "", pr.out};
            // child protocol: stderr = "<ErrorType>\n<message>"; a signal death or sandbox failure has none.
            // Log lines ("[forge:security W] ...", from the child's policy and lockdown) precede the protocol: skip them.
            std::string err = pr.err;
            while (err.rfind("[forge:", 0) == 0) { auto nl = err.find('\n'); err = nl == std::string::npos ? std::string() : err.substr(nl + 1); }
            std::string et = "RuntimeError", em = err.empty() ? pr.err : err;
            if (auto nl = err.find('\n'); nl != std::string::npos && nl > 0 && nl < 64 && err.find(' ') > nl) { et = err.substr(0, nl); em = err.substr(nl + 1); if (!et.empty() && et.back() == '\r') et.pop_back(); }   // CRLF on Windows
            if (pr.term_signal) { et = "RuntimeError"; em = "script child killed by signal " + std::to_string(pr.term_signal) + (pr.err.empty() ? "" : ": " + pr.err); }
            while (!em.empty() && em.back() == '\n') em.pop_back();
            return graph::rpc::server::script_result{false, et, em, pr.out};
        };
    }
    server_lockdown(pol, "forge rpc", bind + ":" + std::to_string(srv.port()));
    std::printf("forge rpc: listening on %s:%d (%s)%s%s%s%s%s\n", bind.c_str(), srv.port(), reg ? dev.c_str() : "cpu",
                srv.tls() ? " tls" : "", sec.tls.client_ca_file.empty() ? "" : " mtls", sec.client_map.empty() ? "" : " cert-map",
                sec.auth_required() ? " auth" : " NO-AUTH", sec.allow_scripts ? " scripts" : "");
    std::fflush(stdout);
    srv.run();
    return 0;
}

} // namespace

// FSS for `forge script` (docs/scripting.md "Security policy"). With an active policy (--policy F or the load_policy()
// search) the capabilities come from the policy (P1: net only if net.mode is not none, proc only with exec, env only
// with env) and the --no-* flags narrow the policy in turn; fs operations are path-scoped by the policy's fs.read /
// fs.write roots (enforce_scope in the runtime). integrity.scripts (INT-1/INT-2) is checked where the source is read:
// engine::run_file and imports verify `<file>.sig` on the bytes they parse, an rpc child verifies the source it got
// on stdin against the signature the client sent (--rpc-sig). Source without a file or signature (-e, the REPL)
// cannot pass "required" and is refused here. Then lockdown(policy) confines the process before any script code runs;
// it replaces options::os_sandbox. No active policy: nothing here changes anything.
void script_policy(forge::script::options& o, const std::string& cli_path, const std::string& what, bool verifiable) {
    auto p = security::load_policy(cli_path);
    if (!p.active()) return;
    if (p.integrity.scripts == security::requirement::required && !verifiable) {
        security::audit("integrity", security::decision::fail, "INT-4", what, "script: integrity.scripts is required and this source has no detached signature");
        throw security::integrity_error(what + ": integrity.scripts is required in " + p.source + ", and code given with -e, typed into the REPL or sent without a signature cannot be verified (run a signed file)", "INT-4");
    }
    if (!o.allow_fs) { p.fs.read.clear(); p.fs.write.clear(); }
    if (!o.allow_net) p.net.mode = security::net_mode::none;
    if (!o.allow_proc) p.exec = false;
    if (!o.allow_env) p.env = false;
    o.allow_fs = !(p.fs.read.empty() && p.fs.write.empty());
    o.allow_net = p.net.mode != security::net_mode::none;
    o.allow_proc = p.exec;
    o.allow_env = p.env;
    o.os_sandbox = forge::script::options::os_sandbox_mode::off;   // lockdown() below applies the OS confinement
    security::audit("script", security::decision::allow, "CAP-1", what,
                    std::string("script runs with fs=") + (o.allow_fs ? "scoped" : "off") + " net=" + security::name(p.net.mode) +
                    " proc=" + (o.allow_proc ? "on" : "off") + " env=" + (o.allow_env ? "on" : "off") + " (profile " + security::name(p.prof) + ")");
    security::lockdown(p);
}

// forge script [opts] <file> [args...] | -e <code> [args...] | (REPL)
int cmd_script(int argc, char** argv) {
    forge::script::options o;
    std::string file, code, emit_target, emit_out; bool have_code = false, os_sandbox_set = false, check_only = false, check_json = false; std::vector<std::string> module_dirs;
    std::string rpc_child, policy_path, rpc_sig_hex;
    int i = 2;
    for (; i < argc; ++i) {
        std::string s = argv[i];
        auto val = [&](const char* what) -> std::string { if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + what); return argv[++i]; };
        if (s == "-e") { code = val("-e"); have_code = true; ++i; break; }
        else if (s == "--rpc-child") { rpc_child = val("--rpc-child"); have_code = true; }   // rpc-serve: source on stdin
        else if (s == "--rpc-sig") rpc_sig_hex = val("--rpc-sig");   // rpc-serve: the client's detached signature (hex of the .sig text)
        else if (s == "--args") { ++i; break; }
        else if (s == "--emit") emit_target = val("--emit");
        else if (s == "-o") emit_out = val("-o");
        else if (s == "--emit-targets") { for (auto& t : forge::script::emit_targets()) std::printf("%s\n", t.c_str()); return 0; }
        else if (s == "--check") check_only = true;           // static checks only (docs/scripting.md "Static checks")
        else if (s == "--json") check_json = true;
        else if (s == "--module-dir") module_dirs.push_back(val("--module-dir"));   // extra import dir for --check
        else if (s == "--no-check") o.static_check = false;
        else if (s == "--ask-model") o.ask_model = val("--ask-model");   // in-process model for forge::ask & co
        else if (s == "--ask-max-tokens") o.ask_max_tokens = std::stoi(val("--ask-max-tokens"));
        else if (s == "--strict-check") o.strict_check = true;
        else if (s == "--tree") o.use_vm = false;
        else if (s == "--debug") o.debug = true;
        else if (s == "--timeout") o.timeout_ms = std::stoull(val("--timeout"));
        else if (s == "--max-steps") o.max_steps = std::stoull(val("--max-steps"));
        else if (s == "--heap") o.heap_bytes = std::stoull(val("--heap"));
        else if (s == "--no-proc") o.allow_proc = false;
        else if (s == "--cache-dir") o.fsc_cache_dir = val("--cache-dir");
        else if (s == "--cache-trust") o.fsc_trusted_keys.push_back(val("--cache-trust"));
        else if (s == "--cache-require-signed") o.fsc_require_signed = true;
        else if (s == "--cache-sign-seed") o.fsc_sign_seed = val("--cache-sign-seed");
        else if (s == "--policy") policy_path = val("--policy");
        else if (s == "--no-fs") o.allow_fs = false;
        else if (s == "--no-net") o.allow_net = false;
        else if (s == "--os-sandbox") { os_sandbox_set = true; auto m = val("--os-sandbox"); o.os_sandbox = m == "off" ? forge::script::options::os_sandbox_mode::off : m == "require" ? forge::script::options::os_sandbox_mode::require : m == "best-effort" ? forge::script::options::os_sandbox_mode::best_effort : throw std::runtime_error("--os-sandbox takes off|best-effort|require"); }
        else if (s == "--") { ++i; if (i < argc) file = argv[i++]; break; }
        else if (s.starts_with("--")) throw std::runtime_error("unknown script option " + s);
        else { file = s; ++i; break; }
    }
    for (; i < argc; ++i) o.argv.push_back(argv[i]);
    (void)os_sandbox_set;   // ForgeScript enforces cap flags at the OS level by default (options::os_sandbox)
    // AI-drafted code (forge::ai_run / ai_eval) runs only after an explicit "y" on the terminal.
    o.confirm = [](const std::string& code) {
        std::fprintf(stderr, "\nAI drafted:\n%s\nRun it? [y/N] ", code.c_str()); std::fflush(stderr);
        char buf[16] = {}; if (!std::fgets(buf, sizeof buf, stdin)) return false;
        return buf[0] == 'y' || buf[0] == 'Y';
    };
    if (!emit_target.empty()) for (std::size_t k = 0; k + 1 < o.argv.size(); ++k) if (o.argv[k] == "-o") { emit_out = o.argv[k + 1]; o.argv.erase(o.argv.begin() + k, o.argv.begin() + k + 2); break; }
    if (check_only) {
        if (!have_code) { if (file.empty()) throw std::runtime_error("--check needs a script file or -e"); std::ifstream f(file, std::ios::binary); if (!f) throw std::runtime_error("cannot read " + file); code.assign(std::istreambuf_iterator<char>(f), {}); }
        forge::script::check_options co{o.allow_fs, o.allow_net, o.allow_proc, o.allow_env, true, module_dirs};
        auto ds = forge::script::check(code, have_code ? "<-e>" : file, co);
        auto txt = check_json ? forge::script::to_json(ds) + "\n" : forge::script::to_text(ds);
        std::fwrite(txt.data(), 1, txt.size(), check_json ? stdout : stderr);
        return forge::script::has_errors(ds) || (o.strict_check && !ds.empty()) ? 2 : 0;
    }
    if (!emit_target.empty()) {   // write once, emit anywhere: translate instead of running
        if (!have_code) { if (file.empty()) throw std::runtime_error("--emit needs a script file or -e"); std::ifstream f(file, std::ios::binary); if (!f) throw std::runtime_error("cannot read " + file); code.assign(std::istreambuf_iterator<char>(f), {}); }
        try {
            auto text = forge::script::emit(code, have_code ? "<-e>" : file, emit_target);
            if (emit_out.empty()) std::fwrite(text.data(), 1, text.size(), stdout);
            else { std::ofstream f(emit_out, std::ios::binary); f << text; f.close(); std::filesystem::permissions(emit_out, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec, std::filesystem::perm_options::add); }
            return 0;
        } catch (const forge::script::emit_error& ex) { std::fprintf(stderr, "%s:%d: emit %s: %s\n", file.empty() ? "<-e>" : file.c_str(), ex.line, emit_target.c_str(), ex.what()); return 2; }
    }
    try { script_policy(o, policy_path, !rpc_child.empty() ? rpc_child : have_code ? std::string("<-e>") : file.empty() ? std::string("<repl>") : file,
                        !rpc_child.empty() ? !rpc_sig_hex.empty() : !have_code && !file.empty()); }
    catch (const security::security_error& e) {
        if (rpc_child.empty()) throw;
        // rpc-serve child protocol: stderr = "<ErrorType>\n<message>" (script classes, FSS-ERR-1)
        const char* t = e.sc == security::sec_code::integrity ? "IntegrityError" : e.sc == security::sec_code::policy ? "PolicyViolation"
                      : e.sc == security::sec_code::permission ? "PermissionError" : "SandboxViolationError";
        std::fprintf(stderr, "%s\n%s", t, e.what());
        return 1;
    }
    if (!rpc_child.empty()) {
        code.assign(std::istreambuf_iterator<char>(std::cin), {});
        o.confirm = nullptr;   // no terminal: AI-drafted code never runs remotely
        try {   // INT-1/INT-2: the source as received, against the client's signature for the script's name
            std::vector<std::uint8_t> sig;
            if (!rpc_sig_hex.empty() && !security::detail::unhex(rpc_sig_hex, sig)) throw security::integrity_error(rpc_child + ": malformed --rpc-sig", "INT-4");
            security::verify_artefact(*security::active_policy(), security::artefact::script, rpc_child,
                                      {reinterpret_cast<const std::uint8_t*>(code.data()), code.size()}, std::string(sig.begin(), sig.end()), security::signed_name(rpc_child));
        } catch (const security::integrity_error& e) { std::fprintf(stderr, "IntegrityError\n%s", e.what()); return 1; }
        forge::script::engine e(o);
        auto r = e.run(code, rpc_child);
        std::fflush(stdout);
        if (!r.ok) { std::fprintf(stderr, "%s\n%s", r.error_type.empty() ? "RuntimeError" : r.error_type.c_str(), r.error.c_str()); return r.exit_code ? r.exit_code : 1; }
        return r.exit_code;
    }
    forge::script::engine e(o);
    if (!have_code && file.empty()) return e.repl();
    auto r = have_code ? e.run(code, "<-e>") : e.run_file(file);
    std::fflush(stdout);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.error.c_str());
    return r.exit_code;
}

int main(int argc, char** argv) {
    if (forge::script::run_process_worker_if_requested(argc, argv)) return 0;   // ProcessPool spawn-mode worker
    forge::script::install_local_ai();   // forge::ask & co run FORGE_ASK_MODEL / --ask-model in-process
    if (argc < 2) return usage();
    std::string c = argv[1];
    if (c == "script") {
        try { return cmd_script(argc, argv); }
        catch (const std::exception& e) { std::fprintf(stderr, "forge script: %s\n", e.what()); return 2; }
    }
    try {
        auto a = parse(argc, argv, 2, {"verify", "tensors", "blueprint", "ids", "no-bos", "allow-scripts", "script-no-proc", "script-no-fs", "script-no-net", "quiet", "json", "detached", "lookup"});
        if (c == "info") return cmd_info();
        if (c == "import") return cmd_import(a);
        if (c == "inspect") return cmd_inspect(a);
        if (c == "run") return cmd_run(a);
        if (c == "bench") return cmd_bench(a);
        if (c == "dump") return cmd_dump(a);
        if (c == "keygen") return cmd_keygen(a);
        if (c == "sign") { activate_policy(a.get("policy")); return cmd_sign(a); }   // FIO-2 on the secret key under a policy
        if (c == "verify") return cmd_verify(a);
        if (c == "encrypt") return cmd_encrypt(a);
        if (c == "rpc-serve") return cmd_rpc_serve(a);
        if (c == "security") return cmd_security(a);
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "forge: error: %s\n", e.what());
        return 1;
    }
}
