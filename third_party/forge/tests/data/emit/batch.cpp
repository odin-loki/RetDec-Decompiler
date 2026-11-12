// the cmd.exe subset (no functions, no .size(), no fs.read): every emitter must run it; cmd's parse-time
// specials (& | < > ( ) ^ % ! ") are in the strings on purpose
int main() {
    std::string name = "it's \"quoted\" & | < > (x) ^ %PATH% !bang!";
    std::println("hello, {}!", name);
    int total = 0;
    for (int i = 0; i < 10; ++i) { if (i % 3 == 0) continue; total += i; }
    std::println("total={} neg={} mod={} sub={}", total, -7 / 2, -7 % 2, total - -3);
    for (auto w : {"a b", "c & d"}) std::println("[{}]", w);
    for (auto k : {1, 2, 3}) total = total + k * 10;
    bool big = total > 20 && total != 5;
    std::println("big={} total={}", big, total);
    std::string e = os.env("FORGE_EMIT_NOPE", "none");
    os.setenv("FORGE_EMIT_X", "set (1)");
    std::println("env={} x={} plat={}", e, os.env("FORGE_EMIT_X", "?"), os.platform);
    fs.mkdir("forge_emit_dir/sub");
    fs.write("forge_emit_dir/sub/f.txt", "data");
    if (fs.exists("forge_emit_dir/sub/f.txt") && fs.is_dir("forge_emit_dir") && !fs.is_dir("forge_emit_dir/sub/f.txt")) std::println("files ok");
    fs.remove("forge_emit_dir");
    std::println("gone={}", fs.exists("forge_emit_dir"));
    int n = 0;
    while (n < 5) { n += 2; if (n == 4) break; }
    std::cout << "n=" << n << std::endl;
    std::println("{}", n > 3 ? "gt" : "le");
    if (name == "x" || n == 4) std::println("or ok"); else std::println("or bad");
    return 4;
}
