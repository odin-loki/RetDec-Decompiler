/**
 * @file src/testing/test_harness.cpp
 * @brief Testing Infrastructure implementation.
 */

#include "retdec/testing/test_harness.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

namespace retdec::testing {

// ─── TestBinary ───────────────────────────────────────────────────────────────

TestBinary TestBinary::makeRaw(const std::vector<uint8_t>& data, uint64_t base) {
    TestBinary b;
    b.format_      = Format::Raw;
    b.entryPoint_  = base;
    Section s;
    s.name        = ".raw";
    s.virtualAddr = base;
    s.data        = data;
    s.executable  = true;
    b.sections_.push_back(s);
    return b;
}

// Minimal ELF64 stub
// ELF header + one LOAD segment + one .text section + optional symtab
static void write32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}
static void write64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) { v.push_back(x & 0xFF); x >>= 8; }
}
static void write16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
}
static void pad(std::vector<uint8_t>& v, size_t to, uint8_t fill = 0) {
    while (v.size() < to) v.push_back(fill);
}

std::vector<uint8_t> TestBinary::serialiseELF64() const {
    std::vector<uint8_t> out;

    // ELF64 Ehdr (64 bytes)
    // e_ident
    out.push_back(0x7f); out.push_back('E'); out.push_back('L'); out.push_back('F');
    out.push_back(2);    // EI_CLASS = ELFCLASS64
    out.push_back(1);    // EI_DATA  = ELFDATA2LSB
    out.push_back(1);    // EI_VERSION
    out.push_back(0);    // EI_OSABI
    for (int i = 0; i < 8; ++i) out.push_back(0); // padding
    write16(out, 2);     // e_type = ET_EXEC
    write16(out, 62);    // e_machine = EM_X86_64
    write32(out, 1);     // e_version
    write64(out, entryPoint_); // e_entry
    write64(out, 64);    // e_phoff = sizeof(Ehdr)
    write64(out, 0);     // e_shoff (filled later)
    write32(out, 0);     // e_flags
    write16(out, 64);    // e_ehsize
    write16(out, 56);    // e_phentsize
    write16(out, 1);     // e_phnum = 1 segment
    write16(out, 64);    // e_shentsize
    uint16_t shnum = static_cast<uint16_t>(sections_.size() + 1);
    write16(out, shnum); // e_shnum
    write16(out, 0);     // e_shstrndx = 0 (none for stub)

    // PT_LOAD program header (56 bytes)
    write32(out, 1);     // p_type = PT_LOAD
    write32(out, 5);     // p_flags = PF_R | PF_X
    uint64_t fileOffset = 64 + 56; // after headers
    write64(out, fileOffset);
    write64(out, entryPoint_);   // p_vaddr
    write64(out, entryPoint_);   // p_paddr
    uint64_t totalBytes = 0;
    for (const auto& s : sections_) totalBytes += s.data.size();
    write64(out, totalBytes);    // p_filesz
    write64(out, totalBytes);    // p_memsz
    write64(out, 0x1000);        // p_align

    // Section data
    for (const auto& sec : sections_)
        out.insert(out.end(), sec.data.begin(), sec.data.end());

    // Null section header
    for (int i = 0; i < 64; ++i) out.push_back(0);

    // One section header per section (minimal)
    for (size_t i = 0; i < sections_.size(); ++i) {
        const auto& sec = sections_[i];
        write32(out, 0);    // sh_name
        write32(out, sec.executable ? 1 : 1); // sh_type = SHT_PROGBITS
        write64(out, sec.executable ? 6ULL : 2ULL); // sh_flags
        write64(out, sec.virtualAddr); // sh_addr
        write64(out, fileOffset);      // sh_offset
        write64(out, sec.data.size()); // sh_size
        fileOffset += sec.data.size();
        write32(out, 0); write32(out, 0); // sh_link, sh_info
        write64(out, 16);  // sh_addralign
        write64(out, 0);   // sh_entsize
    }

    return out;
}

std::vector<uint8_t> TestBinary::serialiseELF32() const {
    // Minimal ELF32, just a valid header
    std::vector<uint8_t> out;
    out.push_back(0x7f); out.push_back('E'); out.push_back('L'); out.push_back('F');
    out.push_back(1);    // ELFCLASS32
    out.push_back(1);    // ELFDATA2LSB
    out.push_back(1);
    out.push_back(0);
    for (int i = 0; i < 8; ++i) out.push_back(0);
    write16(out, 2); write16(out, 3); // ET_EXEC, EM_386
    write32(out, 1);
    write32(out, static_cast<uint32_t>(entryPoint_));
    write32(out, 52); // e_phoff
    write32(out, 0);  // e_shoff
    write32(out, 0);  // e_flags
    write16(out, 52); write16(out, 32); write16(out, 1);
    write16(out, 40); write16(out, 0);  write16(out, 0);

    // One PT_LOAD phdr
    write32(out, 1); write32(out, 52+32);
    write32(out, static_cast<uint32_t>(entryPoint_));
    write32(out, static_cast<uint32_t>(entryPoint_));
    uint32_t sz = 0;
    for (const auto& s : sections_) sz += static_cast<uint32_t>(s.data.size());
    write32(out, sz); write32(out, sz);
    write32(out, 5); write32(out, 0x1000);

    for (const auto& sec : sections_)
        out.insert(out.end(), sec.data.begin(), sec.data.end());
    return out;
}

std::vector<uint8_t> TestBinary::serialisePE32() const {
    // DOS stub + PE signature + minimal COFF header
    std::vector<uint8_t> out;
    // MZ header
    out.push_back('M'); out.push_back('Z');
    pad(out, 60, 0);
    write32(out, 64); // e_lfanew = 64
    pad(out, 64, 0);
    // PE sig
    out.push_back('P'); out.push_back('E'); out.push_back(0); out.push_back(0);
    write16(out, 0x014c); // Machine = i386
    write16(out, 1);      // NumberOfSections
    write32(out, 0);      // TimeDateStamp
    write32(out, 0);      // SymbolTablePtr
    write32(out, 0);      // NumberOfSymbols
    write16(out, 0xe0);   // SizeOfOptionalHeader
    write16(out, 0x102);  // Characteristics
    // Minimal optional header (PE32)
    write16(out, 0x010b); // Magic PE32
    out.push_back(14); out.push_back(0); // linker version
    write32(out, sections_.empty() ? 0 : static_cast<uint32_t>(sections_[0].data.size()));
    write32(out, 0); write32(out, 0);
    write32(out, static_cast<uint32_t>(entryPoint_));
    write32(out, 0x1000);
    write32(out, 0x1000); // ImageBase
    write32(out, 0x1000); // SectionAlignment
    write32(out, 0x200);  // FileAlignment
    write16(out, 6); write16(out, 0); // OS version
    write16(out, 0); write16(out, 0); // Image version
    write16(out, 6); write16(out, 0); // Subsystem version
    write32(out, 0);
    write32(out, static_cast<uint32_t>(0x2000)); // SizeOfImage
    write32(out, static_cast<uint32_t>(0x400));  // SizeOfHeaders
    write32(out, 0); write16(out, 3); write16(out, 0);
    write32(out, 0); write32(out, 0);
    write32(out, 0); write32(out, 0x100000); write32(out, 0x1000);
    write32(out, 16); // NumberOfRvaAndSizes
    // 16 data directories (zeroed)
    for (int i = 0; i < 16; ++i) { write32(out, 0); write32(out, 0); }
    return out;
}

std::vector<uint8_t> TestBinary::serialiseRaw() const {
    std::vector<uint8_t> out;
    for (const auto& s : sections_)
        out.insert(out.end(), s.data.begin(), s.data.end());
    return out;
}

std::vector<uint8_t> TestBinary::serialise() const {
    switch (format_) {
    case Format::ELF64: return serialiseELF64();
    case Format::ELF32: return serialiseELF32();
    case Format::PE32:  return serialisePE32();
    default:            return serialiseRaw();
    }
}

TestBinary TestBinary::makeELF64(const std::vector<uint8_t>& text,
                                   const std::vector<Symbol>& syms) {
    TestBinary b;
    b.format_ = Format::ELF64;
    Section s;
    s.name = ".text"; s.virtualAddr = 0x401000; s.data = text; s.executable = true;
    if (s.data.empty()) s.data = {0xC3}; // ret
    b.sections_.push_back(s);
    for (const auto& sym : syms) b.symbols_.push_back(sym);
    return b;
}

TestBinary TestBinary::makeELF32(const std::vector<uint8_t>& text,
                                   const std::vector<Symbol>& syms) {
    TestBinary b;
    b.format_ = Format::ELF32;
    Section s;
    s.name = ".text"; s.virtualAddr = 0x8048000; s.data = text; s.executable = true;
    if (s.data.empty()) s.data = {0xC3};
    b.sections_.push_back(s);
    for (const auto& sym : syms) b.symbols_.push_back(sym);
    return b;
}

TestBinary TestBinary::makePE32(const std::vector<uint8_t>& text) {
    TestBinary b;
    b.format_ = Format::PE32;
    Section s;
    s.name = ".text"; s.virtualAddr = 0x1000; s.data = text; s.executable = true;
    if (s.data.empty()) s.data = {0xC3};
    b.sections_.push_back(s);
    return b;
}

TestBinary& TestBinary::addSection(const Section& section) {
    sections_.push_back(section);
    return *this;
}

TestBinary& TestBinary::addSymbol(const Symbol& sym) {
    symbols_.push_back(sym);
    return *this;
}

std::string TestBinary::writeToTempFile(const std::string& suffix) const {
    std::string path;
#ifdef _WIN32
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    char tmpFile[MAX_PATH];
    GetTempFileNameA(tmpDir, "rdc", 0, tmpFile);
    path = std::string(tmpFile) + suffix;
    DeleteFileA(tmpFile);
#else
    path = "/tmp/retdec_test_XXXXXX" + suffix;
    // Simpler: just use a fixed temp name with pid
    path = "/tmp/retdec_test_" + std::to_string(getpid()) + suffix;
#endif
    auto data = serialise();
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return path;
}

// ─── SnapshotTester ──────────────────────────────────────────────────────────

SnapshotTester::SnapshotTester(const std::string& dir) : snapshotDir_(dir) {
    fs::create_directories(dir);
}

std::string SnapshotTester::snapshotPath(const std::string& name) const {
    return snapshotDir_ + "/" + name + ".snap";
}

std::string SnapshotTester::computeHash(const std::string& content) {
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    for (char c : content) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ULL;
    }
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << h;
    return os.str();
}

std::string SnapshotTester::makeDiff(const std::string& expected,
                                      const std::string& actual) {
    // Simple line diff
    std::ostringstream os;
    std::istringstream se(expected), sa(actual);
    std::string le, la;
    int line = 1;
    while (std::getline(se, le) || std::getline(sa, la)) {
        if (le != la) {
            os << "@@ line " << line << " @@\n";
            if (!le.empty()) os << "- " << le << "\n";
            if (!la.empty()) os << "+ " << la << "\n";
        }
        le.clear(); la.clear();
        ++line;
    }
    return os.str();
}

bool SnapshotTester::isUpdateMode() {
    const char* env = std::getenv("RETDEC_UPDATE_SNAPSHOTS");
    return env && std::string(env) == "1";
}

SnapshotTester::CompareResult SnapshotTester::compare(const std::string& name,
                                                        const std::string& output) {
    CompareResult cr;
    cr.actual       = output;
    cr.snapshotPath = snapshotPath(name);

    std::ifstream snap(cr.snapshotPath);
    if (!snap.is_open() || isUpdateMode()) {
        // First run or update mode: write snapshot
        update(name, output);
        cr.result = Result::NewSnapshot;
        return cr;
    }

    cr.expected = std::string(std::istreambuf_iterator<char>(snap),
                               std::istreambuf_iterator<char>());
    if (cr.expected == output) {
        cr.result = Result::Match;
    } else {
        cr.result = Result::Mismatch;
        cr.diff   = makeDiff(cr.expected, output);
    }
    return cr;
}

bool SnapshotTester::update(const std::string& name, const std::string& content) {
    std::ofstream f(snapshotPath(name));
    if (!f.is_open()) return false;
    f << content;
    return true;
}

bool SnapshotTester::remove(const std::string& name) {
    return fs::remove(snapshotPath(name));
}

// ─── CorpusRunner ─────────────────────────────────────────────────────────────

CorpusRunner::CorpusRunner(const std::string& dir) : corpusDir_(dir) {}

std::vector<CorpusEntry> CorpusRunner::collect(const std::string& ext) const {
    std::vector<CorpusEntry> result;
    if (!fs::exists(corpusDir_)) return syntheticCorpus();

    for (const auto& entry : fs::recursive_directory_iterator(corpusDir_)) {
        if (!entry.is_regular_file()) continue;
        std::string p = entry.path().string();
        // Skip .expected files
        if (p.size() >= 9 && p.substr(p.size() - 9) == ".expected") continue;
        if (!ext.empty() && p.substr(p.rfind('.') + 1) != ext.substr(1)) continue;

        CorpusEntry ce;
        ce.binaryPath = p;
        std::string expectedPath = p + ".expected";
        ce.hasExpected = fs::exists(expectedPath);
        if (ce.hasExpected) ce.expectedOutputPath = expectedPath;

        // Infer architecture from directory name
        std::string dir = entry.path().parent_path().filename().string();
        if (dir.find("x86_64") != std::string::npos) ce.architecture = "x86_64";
        else if (dir.find("x86") != std::string::npos) ce.architecture = "x86";
        else if (dir.find("arm") != std::string::npos) ce.architecture = "arm";
        else if (dir.find("mips") != std::string::npos) ce.architecture = "mips";

        // Infer format
        std::string fext = entry.path().extension().string();
        if (fext == ".elf" || fext == ".so")    ce.format = "elf";
        else if (fext == ".exe" || fext == ".dll") ce.format = "pe";
        else if (fext == ".o")  ce.format = "obj";

        result.push_back(ce);
    }
    return result;
}

void CorpusRunner::iterate(const std::function<bool(const CorpusEntry&)>& fn,
                            const std::string& ext) const {
    for (const auto& e : collect(ext))
        if (!fn(e)) break;
}

std::vector<CorpusEntry> CorpusRunner::syntheticCorpus() {
    // Return a set of TestBinary-backed entries that always exist
    std::vector<CorpusEntry> result;
    const std::vector<std::pair<std::string, std::string>> archs = {
        {"x86_64", "elf"}, {"x86", "elf"}, {"x86", "pe"}
    };
    for (const auto& [arch, fmt] : archs) {
        CorpusEntry e;
        e.binaryPath    = "<synthetic:" + arch + ":" + fmt + ">";
        e.architecture  = arch;
        e.format        = fmt;
        e.hasExpected   = false;
        e.notes         = "synthetic test binary";
        result.push_back(e);
    }
    return result;
}

// ─── PerformanceAsserter ─────────────────────────────────────────────────────

bool PerformanceAsserter::isSoftMode() {
    const char* env = std::getenv("RETDEC_SOFT_PERF_ASSERT");
    return env && std::string(env) == "1";
}

BenchmarkResult PerformanceAsserter::benchmark(const std::function<void()>& fn,
                                                 int64_t iterations,
                                                 int64_t inputBytes,
                                                 const std::string&) {
    std::vector<double> times;
    times.reserve(iterations);
    auto wallStart = Clock::now();
    for (int64_t i = 0; i < iterations; ++i) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        times.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / 1e6);
    }
    auto wallEnd = Clock::now();

    std::sort(times.begin(), times.end());

    BenchmarkResult r;
    r.iterations = iterations;
    r.totalMs    = std::accumulate(times.begin(), times.end(), 0.0);
    r.minMs      = times.front();
    r.maxMs      = times.back();
    r.avgMs      = r.totalMs / iterations;
    r.p50Ms      = times[iterations / 2];
    r.p95Ms      = times[static_cast<size_t>(0.95 * (iterations - 1))];
    r.p99Ms      = times[static_cast<size_t>(0.99 * (iterations - 1))];
    if (inputBytes > 0 && r.totalMs > 0.0)
        r.throughputBps = static_cast<double>(inputBytes * iterations) / (r.totalMs / 1000.0);
    (void)wallEnd; (void)wallStart;
    return r;
}

bool PerformanceAsserter::assertMaxMs(const std::function<void()>& fn,
                                       double maxMs,
                                       const std::string& desc) {
    auto t0 = Clock::now();
    fn();
    double ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()) / 1e6;
    bool ok = ms <= maxMs;
    if (!ok && !isSoftMode()) {
        fprintf(stderr, "PERF ASSERT FAILED: %s took %.2f ms (max %.2f ms)\n",
                desc.c_str(), ms, maxMs);
    }
    return ok;
}

bool PerformanceAsserter::assertThroughput(const std::function<void()>& fn,
                                             int64_t inputBytes,
                                             double  minBps,
                                             const std::string& desc) {
    auto r = benchmark(fn, 1, inputBytes);
    bool ok = r.throughputBps >= minBps;
    if (!ok && !isSoftMode()) {
        fprintf(stderr, "PERF THROUGHPUT FAILED: %s %.0f B/s (min %.0f B/s)\n",
                desc.c_str(), r.throughputBps, minBps);
    }
    return ok;
}

std::string BenchmarkResult::format() const {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "Benchmark: " << iterations << " iterations\n";
    os << "  avg=" << avgMs << " ms  min=" << minMs << " ms  max=" << maxMs << " ms\n";
    os << "  p50=" << p50Ms << " ms  p95=" << p95Ms << " ms  p99=" << p99Ms << " ms\n";
    if (throughputBps > 0)
        os << "  throughput=" << throughputBps / 1e6 << " MB/s\n";
    return os.str();
}

// ─── MockPipeline ─────────────────────────────────────────────────────────────

MockPipeline& MockPipeline::addStage(const std::string& name, StageHandler handler) {
    stages_.push_back({name, std::move(handler), false});
    return *this;
}

MockPipeline& MockPipeline::skipStage(const std::string& name) {
    for (auto& s : stages_)
        if (s.name == name) { s.skip = true; return *this; }
    return *this;
}

std::string MockPipeline::run(const std::string& input) {
    std::string current = input;
    intermediates_.clear();
    executedStages_.clear();
    for (const auto& stage : stages_) {
        if (stage.skip) continue;
        current = stage.handler(current);
        intermediates_[stage.name] = current;
        executedStages_.push_back(stage.name);
    }
    return current;
}

// ─── TestLogger ──────────────────────────────────────────────────────────────

std::vector<std::string> TestLogger::messages_;

void TestLogger::install()  { messages_.clear(); }
void TestLogger::uninstall(){ messages_.clear(); }
void TestLogger::clear()    { messages_.clear(); }

bool TestLogger::contains(const std::string& sub) {
    for (const auto& m : messages_)
        if (m.find(sub) != std::string::npos) return true;
    return false;
}

const std::vector<std::string>& TestLogger::messages() {
    return messages_;
}

std::string TestLogger::dump() {
    std::string out;
    for (const auto& m : messages_) { out += m; out += '\n'; }
    return out;
}

} // namespace retdec::testing
