/**
 * @file src/retdec/function_analysis_cache.cpp
 * @brief Per-function incremental cache for post-pipeline analysis detectors.
 */

#include "retdec/retdec/function_analysis_cache.h"

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/container_detect/container_detect.h"
#include "retdec/fileformat/utils/crypto.h"
#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/StringRef.h>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

namespace retdec {
namespace analysis {

namespace {

void appendU64(std::vector<unsigned char>& buf, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<unsigned char>(v & 0xffu));
        v >>= 8;
    }
}

void appendCString(std::vector<unsigned char>& buf, llvm::StringRef s)
{
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

std::string makeIndexKey(const std::string& name, const std::string& bodyHash)
{
    return name + '\0' + bodyHash;
}

rapidjson::Value toJsonString(const std::string& s, rapidjson::Document::AllocatorType& a)
{
    return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
}

rapidjson::Value serializeContainer(
        const std::optional<container_detect::ContainerResult>& r,
        rapidjson::Document::AllocatorType& a)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    if (!r || r->kind == container_detect::ContainerKind::Unknown) {
        obj.AddMember("detected", false, a);
        return obj;
    }
    obj.AddMember("detected", true, a);
    obj.AddMember("kind", toJsonString(r->kindName(), a), a);
    obj.AddMember("confidence", r->confidence, a);
    obj.AddMember("summary", toJsonString(r->toString(), a), a);
    return obj;
}

std::optional<container_detect::ContainerResult> deserializeContainer(
        const rapidjson::Value& obj)
{
    if (!obj.IsObject() || !obj.HasMember("detected") || !obj["detected"].IsBool())
        return std::nullopt;
    if (!obj["detected"].GetBool()) return std::nullopt;

    container_detect::ContainerResult r;
    r.confidence = obj.HasMember("confidence") && obj["confidence"].IsNumber()
        ? static_cast<float>(obj["confidence"].GetDouble()) : 0.5f;
    if (obj.HasMember("kind") && obj["kind"].IsString()) {
        const std::string k(obj["kind"].GetString(), obj["kind"].GetStringLength());
        if (k == "Vector") r.kind = container_detect::ContainerKind::Vector;
        else if (k == "List") r.kind = container_detect::ContainerKind::List;
        else if (k == "Map") r.kind = container_detect::ContainerKind::Map;
        else if (k == "UnorderedMap") r.kind = container_detect::ContainerKind::UnorderedMap;
        else if (k == "String") r.kind = container_detect::ContainerKind::String;
        else if (k == "SharedPtr") r.kind = container_detect::ContainerKind::SharedPtr;
    }
    return r;
}

rapidjson::Value serializeSort(
        const std::optional<sort_detect::SortResult>& r,
        rapidjson::Document::AllocatorType& a)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    if (!r || r->algorithm == sort_detect::SortAlgorithm::Unknown) {
        obj.AddMember("detected", false, a);
        return obj;
    }
    obj.AddMember("detected", true, a);
    obj.AddMember("algorithm", toJsonString(r->algorithmName(), a), a);
    obj.AddMember("confidence", r->confidence, a);
    obj.AddMember("summary", toJsonString(r->toString(), a), a);
    return obj;
}

std::optional<sort_detect::SortResult> deserializeSort(const rapidjson::Value& obj)
{
    if (!obj.IsObject() || !obj.HasMember("detected") || !obj["detected"].IsBool())
        return std::nullopt;
    if (!obj["detected"].GetBool()) return std::nullopt;

    sort_detect::SortResult r;
    r.confidence = obj.HasMember("confidence") && obj["confidence"].IsNumber()
        ? static_cast<float>(obj["confidence"].GetDouble()) : 0.5f;
    if (obj.HasMember("algorithm") && obj["algorithm"].IsString()) {
        const std::string alg(obj["algorithm"].GetString(), obj["algorithm"].GetStringLength());
        // Exact algorithmName() match only — no substring find.
        using SA = sort_detect::SortAlgorithm;
        const SA all[] = {
            SA::Quicksort, SA::Introsort, SA::Mergesort, SA::Heapsort,
            SA::Radixsort, SA::InsertionSort, SA::SelectionSort,
            SA::BubbleSort, SA::Timsort
        };
        for (SA a : all) {
            sort_detect::SortResult probe;
            probe.algorithm = a;
            if (alg == probe.algorithmName()) {
                r.algorithm = a;
                break;
            }
        }
    }
    return r;
}

rapidjson::Value serializeAlgo(
        const std::optional<algo_recover::AlgorithmResult>& r,
        rapidjson::Document::AllocatorType& a)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    if (!r || r->kind == algo_recover::AlgorithmKind::Unknown) {
        obj.AddMember("detected", false, a);
        return obj;
    }
    obj.AddMember("detected", true, a);
    obj.AddMember("kind", toJsonString(r->kindName(), a), a);
    obj.AddMember("confidence", r->confidence, a);
    obj.AddMember("summary", toJsonString(r->toString(), a), a);
    return obj;
}

std::optional<algo_recover::AlgorithmResult> deserializeAlgo(const rapidjson::Value& obj)
{
    if (!obj.IsObject() || !obj.HasMember("detected") || !obj["detected"].IsBool())
        return std::nullopt;
    if (!obj["detected"].GetBool()) return std::nullopt;

    algo_recover::AlgorithmResult r;
    r.confidence = obj.HasMember("confidence") && obj["confidence"].IsNumber()
        ? static_cast<float>(obj["confidence"].GetDouble()) : 0.5f;
    if (obj.HasMember("kind") && obj["kind"].IsString()) {
        const std::string k(obj["kind"].GetString(), obj["kind"].GetStringLength());
        if (k == "Transform") r.kind = algo_recover::AlgorithmKind::Transform;
        else if (k == "Accumulate") r.kind = algo_recover::AlgorithmKind::Accumulate;
        else if (k == "Find") r.kind = algo_recover::AlgorithmKind::Find;
        else if (k == "Partition") r.kind = algo_recover::AlgorithmKind::Partition;
        else if (k == "ForEach") r.kind = algo_recover::AlgorithmKind::ForEach;
    }
    return r;
}

rapidjson::Value serializeDetections(
        const FunctionDetections& d,
        rapidjson::Document::AllocatorType& a)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("container", serializeContainer(d.container, a), a);
    obj.AddMember("sort", serializeSort(d.sort, a), a);
    obj.AddMember("algo", serializeAlgo(d.algo, a), a);
    return obj;
}

FunctionDetections deserializeDetections(const rapidjson::Value& obj)
{
    FunctionDetections d;
    if (!obj.IsObject()) return d;
    if (obj.HasMember("container"))
        d.container = deserializeContainer(obj["container"]);
    if (obj.HasMember("sort"))
        d.sort = deserializeSort(obj["sort"]);
    if (obj.HasMember("algo"))
        d.algo = deserializeAlgo(obj["algo"]);
    return d;
}

} // namespace

bool parallelAnalysisEnabled()
{
    const char* e = std::getenv("RETDEC_PARALLEL_ANALYSIS");
    if (e != nullptr)
        return e[0] != '\0' && e[0] != '0';
    return std::thread::hardware_concurrency() > 2;
}

bool incrementalCacheEnabled()
{
    const char* e = std::getenv("RETDEC_INCREMENTAL_CACHE");
    if (e != nullptr)
        return e[0] != '\0' && e[0] != '0';
    return true;
}

std::string functionAnalysisCachePath(const std::string& outputCPath)
{
    if (outputCPath.empty()) return {};
    const auto dot = outputCPath.rfind('.');
    if (dot == std::string::npos)
        return outputCPath + kFunctionCacheSuffix;
    return outputCPath.substr(0, dot) + kFunctionCacheSuffix;
}

std::string computeFunctionBodyHash(
        const llvm::Module& module,
        const ssa::SSAFunction& fn)
{
    std::vector<unsigned char> buf;
    appendU64(buf, fn.blockCount());
    appendU64(buf, fn.instrCount());
    appendCString(buf, fn.name());

    const llvm::Function* lf = module.getFunction(fn.name());
    if (lf != nullptr && !lf->isDeclaration()) {
        for (const llvm::BasicBlock& bb : *lf) {
            appendU64(buf, bb.size());
            for (const llvm::Instruction& inst : bb) {
                appendU64(buf, inst.getOpcode());
                appendU64(buf, inst.getNumOperands());
                for (unsigned oi = 0, on = inst.getNumOperands(); oi < on; ++oi) {
                    if (const auto* cvi =
                            llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(oi))) {
                        const llvm::APInt& val = cvi->getValue();
                        appendU64(buf, val.getBitWidth());
                        const unsigned nWords = val.getNumWords();
                        const auto* words = val.getRawData();
                        for (unsigned w = 0; w < nWords; ++w)
                            appendU64(buf, words[w]);
                    }
                }
                if (const auto* ci = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (const llvm::Function* callee = ci->getCalledFunction())
                        appendCString(buf, callee->getName());
                }
            }
        }
    }
    return fileformat::getSha256(buf.data(), buf.size());
}

FunctionDetections analyseFunctionDetections(const ssa::SSAFunction& fn)
{
    FunctionDetections out;

    {
        container_detect::ContainerDetector cdet;
        auto cr = cdet.analyseFunction(fn);
        if (cr.kind != container_detect::ContainerKind::Unknown)
            out.container = std::move(cr);
    }
    {
        algo_recover::IdiomDetector idet;
        out.idioms = idet.detect(fn);
    }
    {
        algo_recover::AlgorithmDetector adet;
        auto ar = adet.detect(fn);
        if (ar.kind != algo_recover::AlgorithmKind::Unknown)
            out.algo = std::move(ar);
    }
    if (!out.idioms.empty() && out.algo
        && out.algo->kind == algo_recover::AlgorithmKind::Copy
        && out.algo->confidence < 0.85f) {
        out.algo.reset();
    }
    {
        sort_detect::SortDetector sd;
        auto sr = sd.analyseFunction(fn);
        if (sr.algorithm != sort_detect::SortAlgorithm::Unknown)
            out.sort = std::move(sr);
    }
    return out;
}

FunctionAnalysisCache FunctionAnalysisCache::loadFromFile(const std::string& path)
{
    FunctionAnalysisCache cache;
    if (path.empty()) return cache;

    std::ifstream in(path, std::ios::binary);
    if (!in) return cache;

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return cache;

    rapidjson::Document doc;
    doc.Parse(content.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return cache;
    if (!doc.HasMember("version") || !doc["version"].IsUint()
        || doc["version"].GetUint() != kVersion)
        return cache;
    if (!doc.HasMember("functions") || !doc["functions"].IsArray())
        return cache;

    for (const auto& item : doc["functions"].GetArray()) {
        if (!item.IsObject()) continue;
        if (!item.HasMember("name") || !item["name"].IsString()) continue;
        if (!item.HasMember("bodyHash") || !item["bodyHash"].IsString()) continue;

        Entry e;
        e.name = std::string(item["name"].GetString(), item["name"].GetStringLength());
        e.bodyHash = std::string(item["bodyHash"].GetString(), item["bodyHash"].GetStringLength());
        if (item.HasMember("detections"))
            e.detections = deserializeDetections(item["detections"]);
        cache.put(std::move(e));
    }
    return cache;
}

bool FunctionAnalysisCache::saveToFile(const std::string& path) const
{
    if (path.empty()) return false;

    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();
    doc.AddMember("version", kVersion, a);

    rapidjson::Value functions(rapidjson::kArrayType);
    for (const Entry& e : entries_) {
        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("name", toJsonString(e.name, a), a);
        item.AddMember("bodyHash", toJsonString(e.bodyHash, a), a);
        item.AddMember("detections", serializeDetections(e.detections, a), a);
        functions.PushBack(item, a);
    }
    doc.AddMember("functions", functions, a);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
    return static_cast<bool>(out);
}

const FunctionAnalysisCache::Entry* FunctionAnalysisCache::lookup(
        const std::string& name,
        const std::string& bodyHash) const
{
    const auto it = index_.find(makeIndexKey(name, bodyHash));
    if (it == index_.end()) return nullptr;
    return &entries_[it->second];
}

void FunctionAnalysisCache::put(Entry entry)
{
    const std::string key = makeIndexKey(entry.name, entry.bodyHash);
    const auto it = index_.find(key);
    if (it != index_.end()) {
        entries_[it->second] = std::move(entry);
        return;
    }
    index_[key] = entries_.size();
    entries_.push_back(std::move(entry));
}

} // namespace analysis
} // namespace retdec
