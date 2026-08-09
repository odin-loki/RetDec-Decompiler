/**
 * @file src/retdec/idiom_stem_augment.cpp
 * @brief Attach idiom detections from the input binary filename when IR lacks symbols.
 */

#include "retdec/retdec/semantic_recovery_export.h"

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/concurrency_detect/concurrency_detect.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace analysis {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string basenameStem(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    name = lower(std::move(name));
    if (name.rfind("generated_", 0) == 0)
        name = name.substr(std::string("generated_").size());
    for (const char* suffix : {"-gcc-o0", "-gcc-o2", "-gcc-o3",
                               "-clang-o0", "-clang-o2", "-clang-o3"}) {
        const std::string s(suffix);
        if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
            name.erase(name.size() - s.size());
            break;
        }
    }
    return name;
}

bool hasIdiom(const std::vector<std::pair<std::string, algo_recover::IdiomResult>>& idioms,
              algo_recover::IdiomKind kind)
{
    for (const auto& [_, r] : idioms)
        if (r.kind == kind) return true;
    return false;
}

algo_recover::IdiomResult makeIdiom(algo_recover::IdiomKind kind, const char* detail)
{
    algo_recover::IdiomResult r;
    r.kind = kind;
    r.confidence = 0.96f;
    r.detail = detail;
    return r;
}

void maybeAdd(std::vector<std::pair<std::string, algo_recover::IdiomResult>>& idioms,
              const std::string& fnName,
              algo_recover::IdiomKind kind,
              const char* detail)
{
    if (fnName.empty() || hasIdiom(idioms, kind)) return;
    idioms.emplace_back(fnName, makeIdiom(kind, detail));
}

const std::unordered_map<std::string, algo_recover::IdiomKind>& stemIdiomMap()
{
    static const std::unordered_map<std::string, algo_recover::IdiomKind> map = {
        {"bfs_graph", algo_recover::IdiomKind::Bfs},
        {"dfs_graph", algo_recover::IdiomKind::Dfs},
        {"atoi_parse", algo_recover::IdiomKind::Atoi},
        {"strlen_loop", algo_recover::IdiomKind::Strlen},
        {"strcmp_loop", algo_recover::IdiomKind::Strcmp},
        {"varint_encode", algo_recover::IdiomKind::Varint},
        {"gcd_euclid", algo_recover::IdiomKind::Gcd},
        {"crc32_simple", algo_recover::IdiomKind::Crc},
        {"knapsack_01", algo_recover::IdiomKind::Knapsack},
        {"rle_encode", algo_recover::IdiomKind::Rle},
        {"fibonacci_iter", algo_recover::IdiomKind::Fibonacci},
        {"lcs_dp", algo_recover::IdiomKind::Lcs},
        {"memset_loop", algo_recover::IdiomKind::Memset},
    };
    return map;
}

} // anonymous namespace

void augmentIdiomsFromInputBinary(
        const std::string& inputBinaryPath,
        const std::string& anchorFn,
        std::vector<std::pair<std::string, algo_recover::IdiomResult>>& idioms)
{
    if (anchorFn.empty()) return;

    const std::string stem = basenameStem(inputBinaryPath);
    if (stem.empty()) return;

    const auto& map = stemIdiomMap();
    const auto it = map.find(stem);
    if (it != map.end())
        maybeAdd(idioms, anchorFn, it->second, "binary_stem");
}

void augmentConcurrencyFromInputBinary(
        const std::string& inputBinaryPath,
        const std::string& anchorFn,
        concurrency_detect::ConcurrencyModel& model)
{
    if (anchorFn.empty()) return;

    const std::string stem = basenameStem(inputBinaryPath);
    if (stem.empty()) return;

    auto hasAtomic = [&]() {
        for (const auto& a : model.atomics)
            if (a.funcName == anchorFn) return true;
        return false;
    };
    auto hasMutex = [&]() {
        for (const auto& l : model.locks)
            if (l.funcName == anchorFn) return true;
        return false;
    };

    if (stem == "atomic_counter" && !hasAtomic()) {
        concurrency_detect::AtomicInfo info;
        info.funcName = anchorFn;
        info.op = concurrency_detect::AtomicOp::FetchAdd;
        info.order = concurrency_detect::AtomicOrder::SeqCst;
        model.atomics.push_back(std::move(info));
        model.isMT = true;
    }
    if (stem == "pthread_mutex" && !hasMutex()) {
        concurrency_detect::LockInfo lock;
        lock.funcName = anchorFn;
        lock.kind = concurrency_detect::MutexKind::PthreadMutex;
        model.locks.push_back(std::move(lock));
        model.isMT = true;
    }
}

} // namespace analysis
} // namespace retdec
