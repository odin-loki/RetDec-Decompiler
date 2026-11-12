// Golden-output tests: every tests/script/*.fs runs through the tree-walker and the bytecode VM; both
// outputs must match each other and tests/script/<name>.out. An uncaught error appends "!<ErrorType>\n".
// FORGE_GOLDEN_UPDATE=1 rewrites the .out files from the VM output (review the diff before committing).
#include <forge/script/script.hpp>
#include "check.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
using namespace forge;
namespace fs = std::filesystem;
TEST_PROCESS_WORKER(script::run_process_worker_if_requested);   // sandbox_escape.fs starts a ProcessPool

static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string run_path(const std::string& src, const std::string& name, bool vm) {
    std::string out; script::options o; o.use_vm = vm; o.jit_threshold = 1; o.timeout_ms = 20000;
    // Echo progress to stderr as it happens, so a run that hangs shows which script and line it reached.
    std::fprintf(stderr, "== %s (%s)\n", name.c_str(), vm ? "vm" : "tree"); std::fflush(stderr);
    o.out = [&](std::string_view s) { out += s; std::fwrite(s.data(), 1, s.size(), stderr); std::fflush(stderr); };
    script::engine e(o); auto r = e.run(src, name);
    if (!r.ok) { out += "!" + r.error_type + "\n"; std::fprintf(stderr, "!! %s\n", r.error.c_str()); }   // message in the log only
    return out;
}

TEST(golden_scripts) {
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator("tests/script")) if (e.path().extension() == ".fs") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() >= 8);
    bool update = std::getenv("FORGE_GOLDEN_UPDATE") != nullptr;
    for (auto& f : files) {
        auto src = slurp(f); auto name = f.filename().string();
        auto tree = run_path(src, name, false), vm = run_path(src, name, true);
        CHECK_MSG(tree == vm, "%s: tree vs vm differ\n--- tree\n%s--- vm\n%s", name.c_str(), tree.c_str(), vm.c_str());
        auto want_p = fs::path(f).replace_extension(".out");
        if (update) { std::ofstream(want_p, std::ios::binary) << vm; continue; }
        CHECK_MSG(fs::exists(want_p), "%s: missing %s", name.c_str(), want_p.string().c_str());
        auto want = slurp(want_p); std::erase(want, '\r');   // CRLF checkout (core.autocrlf on Windows)
        CHECK_MSG(vm == want, "%s: output differs\n--- got\n%s--- want\n%s", name.c_str(), vm.c_str(), want.c_str());
    }
}

// Inputs the script fuzzer crashed on: each must now be a clean SyntaxError on both paths.
TEST(fuzz_regressions_are_syntax_errors) {
    for (const char* src : {"if(1)[]", "if(b())[]", "double d = 1e;", "for(a[]:m)", "auto [] = 1;"}) {
        for (bool vm : {false, true}) {
            script::options o; o.use_vm = vm; o.out = [](std::string_view) {};
            script::engine e(o); auto r = e.run(src, "<regress>");
            CHECK_MSG(!r.ok, "%s should fail", src);
        }
    }
}
