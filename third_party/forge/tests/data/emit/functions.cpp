// portable demo
int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
std::string greet(std::string who) { return "hello, " + who + "!"; }
int main() {
    std::string name = "it's \"quoted\" $HOME `x` \\ *";
    std::println("{}", greet(name));
    int total = 0;
    for (int i = 0; i < 10; ++i) { if (i % 3 == 0) continue; total += i; }
    std::println("total={} fib={} neg={}", total, fib(10), -7 / 2);
    for (auto w : {"a b", "c"}) std::println("[{}]", w);
    bool big = total > 20 && fib(5) == 5;
    std::println("big={} len={}", big, name.size());
    std::string home = os.env("FORGE_EMIT_NOPE", "none");
    std::println("env={} plat={}", home, os.platform);
    fs.write("forge_emit_t.txt", "line1\nline2\n");
    if (fs.exists("forge_emit_t.txt") && !fs.is_dir("forge_emit_t.txt")) std::print("{}", fs.read("forge_emit_t.txt"));
    fs.remove("forge_emit_t.txt");
    auto r = forge::sh("printf '%s|' {}; exit 3", name);
    std::println("out=[{}] code={} ok={}", r.out, r.code, r.ok);
    int n = 0;
    while (n < 5) { n += 2; if (n == 4) break; }
    std::cout << "n=" << n << std::endl;
    std::println("{}", n > 3 ? "gt" : "le");
    return 3;
}
