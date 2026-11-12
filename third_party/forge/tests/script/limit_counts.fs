// Allocation-count, open-handle and child-process-count limits (limit {allocs / handles / procs}).

// ---- allocs: managed-heap objects allocated by this thread inside the block -> OutOfMemoryError
try {
    limit {.allocs = 200} {
        std::vector<std::string> xs;
        for (int i = 0; i < 10000; ++i) xs.push_back(std::to_string(i));
    }
} catch (const OutOfMemoryError& e) { std::println("allocs: {}", e.what()); }
limit {.allocs = 100000} { std::vector<std::string> ys{"a", "b"}; std::println("allocs: within the limit {}", ys.size()); }
try { limit {.allocs = 0} { std::string s = std::to_string(12345); } }
catch (const OutOfMemoryError& e) { std::println("allocs 0: OutOfMemoryError"); }
// the limit ends with the block
std::vector<std::string> after; for (int i = 0; i < 1000; ++i) after.push_back(std::to_string(i));
std::println("allocs: unlimited again ({})", after.size());

// ---- handles: OS handles (files, locks, watchers, processes) open at once -> TooManyFilesError
std::string path = forge::fs::temp_file("forge-limit-");
limit {.handles = 2} {
    auto a = forge::fs::open(path, "w");
    auto b = forge::fs::open(path);
    try { auto c = forge::fs::open(path); std::println("third handle opened (wrong)"); }
    catch (const TooManyFilesError& e) { std::println("handles: {}", e.what()); }
    a.close();   // closing one frees a slot
    auto d = forge::fs::open(path);
    std::println("handles: reopened after close");
}
{
    // handles go back to the quota when they are released
    int opened = 0;
    limit {.handles = 1} { for (int i = 0; i < 5; ++i) { auto f = forge::fs::open(path); ++opened; } }
    std::println("handles: {} sequential opens with limit 1", opened);
}
// nested blocks count against every enclosing quota
limit {.handles = 1} {
    auto outer = forge::fs::open(path);
    try { limit {.handles = 5} { auto inner = forge::fs::open(path); } }
    catch (const TooManyFilesError& e) { std::println("handles: nested {}", e.what()); }
}
forge::fs::remove(path);

// ---- procs: child processes started inside the block -> LimitExceededError
// Windows need not have a POSIX sh: forge::shell is the host shell (pwsh / powershell / cmd there, whose output
// ends in CRLF, hence trim()), argv commands go through cmd, and the refusal's "(not starting <exe>)" is cut off.
std::vector<std::string> echo_argv(const std::string& w) {
    if (forge::os::platform == "windows") return std::vector<std::string>{"cmd", "/c", "echo " + w};
    return std::vector<std::string>{"sh", "-c", "echo " + w};
}
std::string refusal(const std::string& what) { return what.substr(0, what.find(" (not starting")); }
limit {.procs = 2} {
    std::println("procs: {}", forge::shell("echo one").out.trim());
    std::println("procs: {}", forge::run(echo_argv("two")).out.trim());
    try { forge::shell("echo three"); std::println("third process started (wrong)"); }
    catch (const LimitExceededError& e) { std::println("procs: {}", refusal(e.what())); }
}
// tasks started in the block share its quota
try {
    limit {.procs = 1} {
        auto t = std::async([] { return forge::shell("echo task").out.trim(); });
        std::println("procs: {}", t.get());
        auto u = std::async([] { return forge::shell("echo again").out; });
        u.get();
    }
} catch (const LimitExceededError& e) { std::println("procs: task {}", refusal(e.what())); }
std::println("procs: unlimited again: {}", forge::shell("echo four").out.trim());

// ---- bad counts
try { limit {.handles = -1} {} } catch (const ValueError& e) { std::println("ValueError: {}", e.what()); }
