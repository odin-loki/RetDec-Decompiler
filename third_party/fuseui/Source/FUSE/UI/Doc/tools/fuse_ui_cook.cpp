// fuse_ui_cook: cooks `.fuseui` documents and `.fusestyle` sheets (UI8).
//   fuse_ui_cook --out=<dir> [--registration=<file.cpp>] [--fn=<name>] files...
// Writes <dir>/<name>.fuseuic / <name>.fusestylec and, with --registration, the C++ registration list for the
// widget classes all the documents use. Exit code 1 on any error (message with file and node path / line).
#include "fuse/ui/doc/cook.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace fuse::ui::doc;

static bool writeFile(const std::string& path, const void* data, std::size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(static_cast<const char*>(data), std::streamsize(n));
    return bool(f);
}
static std::string stem(const std::string& p) {
    const auto slash = p.find_last_of("/\\");
    std::string s = slash == std::string::npos ? p : p.substr(slash + 1);
    return s.substr(0, s.find_last_of('.'));
}

int main(int argc, char** argv) {
    std::string out = ".", reg, fn = "fuse_ui_register_cooked";
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--out=", 6)) out = argv[i] + 6;
        else if (!std::strncmp(argv[i], "--registration=", 15)) reg = argv[i] + 15;
        else if (!std::strncmp(argv[i], "--fn=", 5)) fn = argv[i] + 5;
        else files.push_back(argv[i]);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: fuse_ui_cook --out=<dir> [--registration=<file.cpp>] [--fn=<name>] files...\n");
        return 1;
    }
    std::vector<std::vector<std::string>> classes;
    int errors = 0;
    for (const std::string& f : files) {
        std::string text;
        if (!readTextFile(f.c_str(), text)) { std::fprintf(stderr, "%s: cannot read\n", f.c_str()); ++errors; continue; }
        if (f.ends_with(".fusestyle") || f.ends_with(".css")) {
            std::vector<fuse::ui::StyleDiagnostic> diags;
            auto c = cookStyle(text, &diags);
            for (const auto& d : diags) std::fprintf(stderr, "%s:%u: warning: %s\n", f.c_str(), d.line, d.message.c_str());
            if (!c) { std::fprintf(stderr, "%s: %s\n", f.c_str(), c.error().c_str()); ++errors; continue; }
            const std::string dst = out + "/" + stem(f) + ".fusestylec";
            if (!writeFile(dst, c->data(), c->size())) { std::fprintf(stderr, "%s: cannot write\n", dst.c_str()); ++errors; }
            std::printf("%s -> %s (%zu bytes)\n", f.c_str(), dst.c_str(), c->size());
        } else {
            CookInfo info;
            auto c = cookDocument(text, &info);
            if (!c) {
                const auto& e = c.error();
                std::fprintf(stderr, "%s:%u:%u: %s%s%s\n", f.c_str(), e.line, e.column, e.path.c_str(), e.path.empty() ? "" : ": ", e.message.c_str());
                ++errors;
                continue;
            }
            const std::string dst = out + "/" + stem(f) + ".fuseuic";
            if (!writeFile(dst, c->data(), c->size())) { std::fprintf(stderr, "%s: cannot write\n", dst.c_str()); ++errors; }
            std::printf("%s -> %s (%zu bytes, %u nodes, %u bindings, %u instructions)\n", f.c_str(), dst.c_str(), c->size(),
                        info.nodes, info.bindings, info.instructions);
            classes.push_back(std::move(info.classes));
        }
    }
    if (!reg.empty() && !errors) {
        const std::string src = registrationSource(registrationList(classes), fn);
        if (!writeFile(reg, src.data(), src.size())) { std::fprintf(stderr, "%s: cannot write\n", reg.c_str()); ++errors; }
    }
    return errors ? 1 : 0;
}
