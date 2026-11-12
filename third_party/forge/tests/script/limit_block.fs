// `limit {...} { body }` block syntax (sugar for forge::limit(opts, fn)) and string sizes / durations.
int total = 0;
limit {{"timeout", "5s"}, {"mem", "64M"}} { total += 5; }
std::println("statement block: {}", total);

auto v = limit {.cpu_s = "2s", .timeout = 5000} { return 6 * 7; };
std::println("expression block: {}", v);

forge::limit {.proc_mem = "512M", .proc_cpu = "10s"} { total += 1; }
std::println("qualified name: {}", total);

std::map<std::string, std::string> opts{{"timeout", "1h"}};
limit (opts) { std::println("options from a variable"); }

try { limit {.timeout = "30ms"} { while (true) {} } }
catch (const TimeoutError& e) { std::println("timeout: {}", e.what()); }

try {
    limit {.mem = "64K"} {
        std::vector<std::string> xs;
        for (int i = 0; i < 100000; ++i) xs.push_back(std::string(64, 'x'));
    }
} catch (const OutOfMemoryError& e) { std::println("mem \"64K\": OutOfMemoryError"); }

limit {.mem = "1MiB"} { std::vector<int> ys{1, 2, 3}; std::println("mem \"1MiB\": ok {}", ys.size()); }

// nested blocks only narrow: the inner 10s timeout cannot outlive the outer 50ms one
try { limit {.timeout = "50ms"} { limit {.timeout = "10s"} { while (true) {} } } }
catch (const TimeoutError& e) { std::println("nested: inner block stopped by the outer timeout"); }

// limits are restored after the block, also after an exception
limit {.timeout = 20} { std::println("short block"); }
for (int i = 0; i < 200000; ++i) {}
std::println("after the blocks");

// a block is a scope of its own; locals of the enclosing scope are visible and writable
std::string log;
for (int i = 0; i < 3; ++i) {
    limit {.cpu_s = 5} { log += std::to_string(i); }
}
std::println("loop: {}", log);

// option errors
for (auto bad : {"2X", "lots", ""}) {
    try { forge::limit(std::map<std::string, std::string>{{"mem", bad}}, [] {}); }
    catch (const ValueError& e) { std::println("ValueError: {}", e.what()); }
}
try { forge::limit(std::map<std::string, std::string>{{"timeout", "5 fortnights"}}, [] {}); }
catch (const ValueError& e) { std::println("ValueError: {}", e.what()); }
try { forge::limit(std::map<std::string, int>{{"timeuot", 5}}, [] {}); }
catch (const ValueError& e) { std::println("ValueError: {}", e.what()); }
