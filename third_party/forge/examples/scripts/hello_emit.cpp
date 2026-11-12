int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int main() {
    std::string who = os.env("USER_NAME", "world");
    std::println("hello, {}! fib(10)={} on {}", who, fib(10), os.platform);
    for (auto f : {"a.txt", "b.txt"}) { fs.write(f, "x"); std::println("{} exists={}", f, fs.exists(f)); fs.remove(f); }
    auto r = forge::sh("echo {}", "it's $safe");
    std::print("sh said: {}", r.out);
    return r.ok ? 0 : 1;
}
