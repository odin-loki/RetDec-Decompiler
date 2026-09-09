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
	// The exporter builds its comment from emittedType (falling back to
	// kindName) and from elementType, not from kind alone. Caching only the
	// kind meant a warm run printed "STL: std::array" where the cold run
	// printed "STL: ring_buffer" -- the same binary, two different outputs,
	// which is what CACHE-05 compares.
	obj.AddMember("emitted", toJsonString(r->emittedType, a), a);
	obj.AddMember("elemKind", static_cast<int>(r->elementType.kind), a);
	obj.AddMember("elemWidth", static_cast<int>(r->elementType.byteWidth), a);
	obj.AddMember("elemSigned", r->elementType.isSigned, a);
	if (!r->elementType.name.empty()) obj.AddMember("elemName", toJsonString(r->elementType.name, a), a);
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
		// Same drift the algo path had: serializeContainer writes kindName()
		// ("std::vector"), and this matched six bare names ("Vector") that
		// kindName() never produces. Eight of the fourteen kinds had no case.
		const std::string k(obj["kind"].GetString(), obj["kind"].GetStringLength());
		r.kind = container_detect::containerKindFromName(k);
	}
	if (r.kind == container_detect::ContainerKind::Unknown) return std::nullopt;

	if (obj.HasMember("emitted") && obj["emitted"].IsString())
		r.emittedType.assign(obj["emitted"].GetString(), obj["emitted"].GetStringLength());
	if (obj.HasMember("elemKind") && obj["elemKind"].IsInt())
	{
		const int ek = obj["elemKind"].GetInt();
		if (ek >= 0 && ek <= static_cast<int>(container_detect::RecoveredType::Kind::String))
			r.elementType.kind = static_cast<container_detect::RecoveredType::Kind>(ek);
	}
	if (obj.HasMember("elemWidth") && obj["elemWidth"].IsInt())
	{
		const int w = obj["elemWidth"].GetInt();
		if (w >= 0 && w <= 255) r.elementType.byteWidth = static_cast<uint8_t>(w);
	}
	if (obj.HasMember("elemSigned") && obj["elemSigned"].IsBool()) r.elementType.isSigned = obj["elemSigned"].GetBool();
	if (obj.HasMember("elemName") && obj["elemName"].IsString())
		r.elementType.name.assign(obj["elemName"].GetString(), obj["elemName"].GetStringLength());
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
	// toString() prints the element type and the compiler variant, and the
	// exporter also prefixes the detail with "evidence:symbol_name" when the
	// variant is known -- so dropping these made a warm run's detail differ
	// from a cold one's, the same way the container path did.
	obj.AddMember("variant", static_cast<int>(r->compilerVariant), a);
	obj.AddMember("elemKind", static_cast<int>(r->elementType.kind), a);
	obj.AddMember("elemWidth", static_cast<int>(r->elementType.byteWidth), a);
	obj.AddMember("elemSigned", r->elementType.isSigned, a);
	if (!r->elementType.name.empty()) obj.AddMember("elemName", toJsonString(r->elementType.name, a), a);
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
	if (obj.HasMember("variant") && obj["variant"].IsInt())
	{
		const int v = obj["variant"].GetInt();
		if (v >= 0 && v <= static_cast<int>(sort_detect::CompilerVariant::MSVC))
			r.compilerVariant = static_cast<sort_detect::CompilerVariant>(v);
	}
	if (obj.HasMember("elemKind") && obj["elemKind"].IsInt())
	{
		const int ek = obj["elemKind"].GetInt();
		if (ek >= 0 && ek <= static_cast<int>(sort_detect::ElementType::Kind::Struct))
			r.elementType.kind = static_cast<sort_detect::ElementType::Kind>(ek);
	}
	if (obj.HasMember("elemWidth") && obj["elemWidth"].IsInt())
	{
		const int w = obj["elemWidth"].GetInt();
		if (w >= 0 && w <= 255) r.elementType.byteWidth = static_cast<uint8_t>(w);
	}
	if (obj.HasMember("elemSigned") && obj["elemSigned"].IsBool()) r.elementType.isSigned = obj["elemSigned"].GetBool();
	if (obj.HasMember("elemName") && obj["elemName"].IsString())
		r.elementType.name.assign(obj["elemName"].GetString(), obj["elemName"].GetStringLength());
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
	// toString() prints the tier, and the exporter uses toString() as the
	// detection's detail. Recomputing it from a default-constructed tier makes
	// the warm run's detail differ from the cold run's.
	obj.AddMember("tier", static_cast<int>(r->tier), a);
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
		// serializeAlgo writes kindName(), which spells these "std::find_if",
		// "std::transform" and so on. What was here matched five bare names --
		// "Find", "Transform" -- that kindName() never produces, so no kind
		// ever survived a round trip: every cached algorithm read back as
		// Unknown and the emitted comment said "unknown detected" where the
		// cold run said "std::find_if detected". Twelve of the seventeen kinds
		// had no case at all. One table, read both ways, cannot drift.
		const std::string k(obj["kind"].GetString(), obj["kind"].GetStringLength());
		r.kind = algo_recover::algorithmKindFromName(k);
	}
	if (obj.HasMember("tier") && obj["tier"].IsInt())
	{
		const int t = obj["tier"].GetInt();
		if (t >= 0 && t <= static_cast<int>(algo_recover::EmissionTier::High))
			r.tier = static_cast<algo_recover::EmissionTier>(t);
	}
	// A cache entry that cannot name its kind is worse than a miss: it makes
	// the warm run disagree with the cold one.
	if (r.kind == algo_recover::AlgorithmKind::Unknown) return std::nullopt;
	return r;
}

rapidjson::Value
serializeIdioms(const std::vector<algo_recover::IdiomResult>& idioms, rapidjson::Document::AllocatorType& a)
{
	// FunctionDetections has carried an `idioms` vector all along and neither
	// side of this file touched it, so a warm run lost every idiom detection
	// the cold run had made: CACHE-05 saw "[RetDec] DFS detected" and
	// "[RetDec] GraphTraversal detected" in the cold output and neither in the
	// warm one. Everything the exporter reads -- exportLabels() and
	// toString() -- derives from these three fields.
	//
	// The kind travels as its enum value rather than through a name table.
	// The cache file is private and versioned, so there is no compatibility
	// reason for a name here, and two name tables that have to agree is
	// exactly what went wrong on the container and algorithm paths.
	rapidjson::Value arr(rapidjson::kArrayType);
	for (const auto& i: idioms)
	{
		if (i.kind == algo_recover::IdiomKind::Unknown) continue;
		rapidjson::Value o(rapidjson::kObjectType);
		o.AddMember("kind", static_cast<int>(i.kind), a);
		o.AddMember("confidence", i.confidence, a);
		o.AddMember("detail", toJsonString(i.detail, a), a);
		arr.PushBack(o, a);
	}
	return arr;
}

std::vector<algo_recover::IdiomResult> deserializeIdioms(const rapidjson::Value& arr)
{
	std::vector<algo_recover::IdiomResult> out;
	if (!arr.IsArray()) return out;
	for (const auto& o: arr.GetArray())
	{
		if (!o.IsObject()) continue;
		if (!o.HasMember("kind") || !o["kind"].IsInt()) continue;
		const int k = o["kind"].GetInt();
		if (k <= static_cast<int>(algo_recover::IdiomKind::Unknown)) continue;
		if (k > static_cast<int>(algo_recover::IdiomKind::HashTableChaining)) continue;

		algo_recover::IdiomResult r;
		r.kind = static_cast<algo_recover::IdiomKind>(k);
		if (o.HasMember("confidence") && o["confidence"].IsNumber())
			r.confidence = static_cast<float>(o["confidence"].GetDouble());
		if (o.HasMember("detail") && o["detail"].IsString())
			r.detail.assign(o["detail"].GetString(), o["detail"].GetStringLength());
		out.push_back(std::move(r));
	}
	return out;
}

rapidjson::Value serializeDetections(
        const FunctionDetections& d,
        rapidjson::Document::AllocatorType& a)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("container", serializeContainer(d.container, a), a);
    obj.AddMember("sort", serializeSort(d.sort, a), a);
    obj.AddMember("algo", serializeAlgo(d.algo, a), a);
	obj.AddMember("idioms", serializeIdioms(d.idioms, a), a);
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
	if (obj.HasMember("idioms")) d.idioms = deserializeIdioms(obj["idioms"]);
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
