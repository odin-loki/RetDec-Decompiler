/**
 * @file src/module_cluster/module_cluster.cpp
 * @brief Module Clustering and CMake Project Generation implementation.
 */

#include "retdec/module_cluster/module_cluster.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <unordered_set>

namespace retdec::module_cluster {

// ─── ClusterResult helpers ────────────────────────────────────────────────────

const Module* ClusterResult::entryModule() const {
    for (const auto& m : modules)
        if (m.isEntryModule) return &m;
    return modules.empty() ? nullptr : &modules[0];
}

const Module* ClusterResult::findModule(const std::string& funcName) const {
    for (const auto& m : modules)
        for (const auto& f : m.functions)
            if (f == funcName) return &m;
    return nullptr;
}

// ─── LouvainClusterer ─────────────────────────────────────────────────────────

LouvainClusterer::LouvainClusterer(LouvainConfig cfg)
    : cfg_(cfg) {}

std::unordered_map<std::string, int>
LouvainClusterer::cluster(const CallGraph& graph) {
    if (graph.functions.empty()) return {};

    // Build adjacency (undirected for modularity: merge both directions)
    // adjacency[u][v] = weight
    std::unordered_map<std::string,
        std::unordered_map<std::string, double>> adj;
    double totalWeight = 0.0;

    for (const auto& e : graph.edges) {
        if (e.caller == e.callee) continue;
        double w = static_cast<double>(e.weight);
        adj[e.caller][e.callee] += w;
        adj[e.callee][e.caller] += w;
        totalWeight += w;
    }
    if (totalWeight == 0.0) totalWeight = 1.0;

    // Initialise nodes
    std::vector<Node> nodes;
    nodes.reserve(graph.functions.size());
    std::unordered_map<std::string, int> nameToIdx;

    for (int i = 0; i < static_cast<int>(graph.functions.size()); ++i) {
        const auto& fn = graph.functions[i];
        Node n;
        n.name      = fn.name;
        n.community = i;   // each node starts in its own community

        // Seed by name prefix if requested
        if (cfg_.useSeedByName && !fn.namePrefix.empty()) {
            // Assign community based on prefix hash (same prefix → same seed)
            std::hash<std::string> hasher;
            n.community = static_cast<int>(hasher(fn.namePrefix) % graph.functions.size());
        }

        for (const auto& [nb, w] : adj[fn.name])
            n.degree += w;
        n.selfWeight = 0.0;
        nodes.push_back(n);
        nameToIdx[fn.name] = i;
    }

    // Phase 1: node moving
    bool improved = true;
    iterations_ = 0;
    while (improved && iterations_ < cfg_.maxIterations) {
        improved = false;
        ++iterations_;
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            int   bestCom   = nodes[i].community;
            double bestGain = 0.0;

            // Collect candidate communities from neighbours
            std::unordered_set<int> candidateComs;
            auto it = adj.find(nodes[i].name);
            if (it != adj.end()) {
                for (const auto& [nb, w] : it->second) {
                    auto jt = nameToIdx.find(nb);
                    if (jt != nameToIdx.end())
                        candidateComs.insert(nodes[jt->second].community);
                }
            }
            candidateComs.insert(nodes[i].community);

            // Compute leaving cost from current community (Louvain full ΔQ formula)
            double k_i_in_curr = 0.0;
            double sigma_tot_curr = 0.0;
            {
                auto nbIt = adj.find(nodes[i].name);
                if (nbIt != adj.end()) {
                    for (const auto& [nb, w] : nbIt->second) {
                        auto jt = nameToIdx.find(nb);
                        if (jt != nameToIdx.end() &&
                            nodes[jt->second].community == nodes[i].community)
                            k_i_in_curr += w;
                    }
                }
                for (const auto& n2 : nodes)
                    if (n2.community == nodes[i].community) sigma_tot_curr += n2.degree;
            }

            for (int com : candidateComs) {
                if (com == nodes[i].community) continue;
                // Compute gain of joining community com
                double k_i_in = 0.0;
                double sigma_tot = 0.0;
                auto nbIt = adj.find(nodes[i].name);
                if (nbIt != adj.end()) {
                    for (const auto& [nb, w] : nbIt->second) {
                        auto jt = nameToIdx.find(nb);
                        if (jt != nameToIdx.end() && nodes[jt->second].community == com)
                            k_i_in += w;
                    }
                }
                for (const auto& n2 : nodes)
                    if (n2.community == com) sigma_tot += n2.degree;

                // Full ΔQ: join gain minus leaving cost (prevents ping-pong oscillation)
                double joinGain  = k_i_in     - cfg_.resolution * sigma_tot *
                                   nodes[i].degree / (2.0 * totalWeight);
                double leaveCost = k_i_in_curr - cfg_.resolution *
                                   (sigma_tot_curr - nodes[i].degree) *
                                   nodes[i].degree / (2.0 * totalWeight);
                double gain = (joinGain - leaveCost) / totalWeight;
                if (gain > bestGain) {
                    bestGain = gain;
                    bestCom  = com;
                }
            }

            if (bestCom != nodes[i].community) {
                nodes[i].community = bestCom;
                improved = true;
            }
        }
    }

    // Remap community IDs to 0-based contiguous
    std::unordered_map<int, int> comRemap;
    int nextId = 0;
    std::unordered_map<std::string, int> result;
    for (const auto& n : nodes) {
        if (!comRemap.count(n.community))
            comRemap[n.community] = nextId++;
        result[n.name] = comRemap[n.community];
    }

    // Compute final modularity
    modularity_ = computeModularity(nodes, adj, totalWeight);
    return result;
}

double LouvainClusterer::computeModularity(
    const std::vector<Node>& nodes,
    const std::unordered_map<std::string,
          std::unordered_map<std::string, double>>& adj,
    double totalWeight) const {
    if (totalWeight == 0.0) return 0.0;
    double Q = 0.0;
    std::unordered_map<std::string, int> idx;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        idx[nodes[i].name] = i;

    for (const auto& [u, nbMap] : adj) {
        auto iu = idx.find(u);
        if (iu == idx.end()) continue;
        for (const auto& [v, w] : nbMap) {
            auto iv = idx.find(v);
            if (iv == idx.end()) continue;
            if (nodes[iu->second].community == nodes[iv->second].community) {
                Q += w - cfg_.resolution *
                     nodes[iu->second].degree * nodes[iv->second].degree /
                     (2.0 * totalWeight);
            }
        }
    }
    return Q / (2.0 * totalWeight);
}

// ─── ModuleNamer ──────────────────────────────────────────────────────────────

std::string ModuleNamer::longestCommonPrefix(const std::vector<std::string>& names) {
    if (names.empty()) return "";
    std::string prefix = names[0];
    for (const auto& s : names) {
        size_t i = 0;
        while (i < prefix.size() && i < s.size() && prefix[i] == s[i]) ++i;
        prefix = prefix.substr(0, i);
    }
    // Strip trailing underscores or digits
    while (!prefix.empty() && (prefix.back() == '_' || std::isdigit(prefix.back())))
        prefix.pop_back();
    return prefix;
}

static const std::unordered_map<std::string, std::string> kLibPrefixes = {
    {"ssl", "ssl"}, {"SSL", "ssl"}, {"EVP", "crypto"}, {"BIO", "crypto"},
    {"sqlite3", "sqlite"}, {"curl", "curl"}, {"CURL", "curl"},
    {"cudaMalloc", "cuda"}, {"cuLaunch", "cuda"},
    {"pthread", "thread"}, {"GOMP", "openmp"}, {"__kmpc", "openmp"},
    {"tbb", "tbb"},
    {"zlib", "zlib"}, {"inflate", "zlib"}, {"deflate", "zlib"},
    {"png_", "png"}, {"jpeg", "jpeg"},
    {"lua_", "lua"}, {"luaL_", "lua"},
    {"rapidjson", "json"}, {"cJSON", "json"},
    {"xml", "xml"}, {"xmlRead", "xml"},
    {"Qt5", "qt"}, {"Qt6", "qt"},
    {"boost", "boost"},
};

std::string ModuleNamer::extractLibName(const std::vector<std::string>& libSyms) {
    std::unordered_map<std::string, int> libCount;
    for (const auto& sym : libSyms) {
        for (const auto& [prefix, lib] : kLibPrefixes) {
            if (sym.find(prefix) == 0 || sym.find(prefix) != std::string::npos) {
                libCount[lib]++;
                break;
            }
        }
    }
    if (libCount.empty()) return "";
    return std::max_element(libCount.begin(), libCount.end(),
        [](const auto& a, const auto& b){ return a.second < b.second; })->first;
}

std::string ModuleNamer::name(const std::vector<FunctionMeta>& funcs,
                               int fallbackId) const {
    // 1. Explicit namePrefix field (highest-priority semantic metadata)
    std::unordered_map<std::string, int> prefixCount;
    for (const auto& f : funcs)
        if (!f.namePrefix.empty()) prefixCount[f.namePrefix]++;
    if (!prefixCount.empty()) {
        auto it = std::max_element(prefixCount.begin(), prefixCount.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        if (it->first.size() >= 2) {
            std::string n = it->first;
            while (!n.empty() && n.back() == '_') n.pop_back();
            if (!n.empty()) return n;
        }
    }

    // 2. Library signal (semantic meaning from external calls)
    std::vector<std::string> allLibSyms;
    for (const auto& f : funcs)
        allLibSyms.insert(allLibSyms.end(), f.calledLibSyms.begin(), f.calledLibSyms.end());
    std::string libName = extractLibName(allLibSyms);
    if (!libName.empty()) return libName + "_module";

    // 3. Source file name (structural context from debug info)
    std::unordered_map<std::string, int> fileCount;
    for (const auto& f : funcs)
        if (!f.sourceFile.empty()) fileCount[f.sourceFile]++;
    if (!fileCount.empty()) {
        auto it = std::max_element(fileCount.begin(), fileCount.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        std::string src = it->first;
        // Strip path
        size_t slash = src.rfind('/');
        if (slash == std::string::npos) slash = src.rfind('\\');
        if (slash != std::string::npos) src = src.substr(slash + 1);
        // Strip extension
        size_t dot = src.rfind('.');
        if (dot != std::string::npos) src = src.substr(0, dot);
        if (!src.empty()) return src;
    }

    // 4. Common symbol prefix (last-resort structural heuristic)
    std::vector<std::string> names;
    for (const auto& f : funcs) names.push_back(f.name);
    std::string prefix = longestCommonPrefix(names);
    if (prefix.size() >= 3) return prefix;

    return "module_" + std::to_string(fallbackId);
}

// ─── HeaderInference ──────────────────────────────────────────────────────────

const std::unordered_map<std::string, std::string>&
HeaderInference::symbolHeaderMap() {
    static const std::unordered_map<std::string, std::string> kMap = {
        // C standard library
        {"malloc",      "<stdlib.h>"}, {"free",      "<stdlib.h>"},
        {"realloc",     "<stdlib.h>"}, {"calloc",    "<stdlib.h>"},
        {"exit",        "<stdlib.h>"}, {"abort",     "<stdlib.h>"},
        {"printf",      "<stdio.h>"},  {"fprintf",   "<stdio.h>"},
        {"sprintf",     "<stdio.h>"},  {"snprintf",  "<stdio.h>"},
        {"scanf",       "<stdio.h>"},  {"fopen",     "<stdio.h>"},
        {"fclose",      "<stdio.h>"},  {"fread",     "<stdio.h>"},
        {"fwrite",      "<stdio.h>"},  {"fgets",     "<stdio.h>"},
        {"fputs",       "<stdio.h>"},  {"fflush",    "<stdio.h>"},
        {"memcpy",      "<string.h>"}, {"memset",    "<string.h>"},
        {"memmove",     "<string.h>"}, {"memcmp",    "<string.h>"},
        {"strlen",      "<string.h>"}, {"strcmp",    "<string.h>"},
        {"strcpy",      "<string.h>"}, {"strcat",    "<string.h>"},
        {"strncpy",     "<string.h>"}, {"strncmp",   "<string.h>"},
        {"strstr",      "<string.h>"}, {"strchr",    "<string.h>"},
        {"atoi",        "<stdlib.h>"}, {"atof",      "<stdlib.h>"},
        {"strtol",      "<stdlib.h>"}, {"strtod",    "<stdlib.h>"},
        {"qsort",       "<stdlib.h>"}, {"bsearch",   "<stdlib.h>"},
        {"rand",        "<stdlib.h>"}, {"srand",     "<stdlib.h>"},
        {"time",        "<time.h>"},   {"clock",     "<time.h>"},
        {"localtime",   "<time.h>"},   {"gmtime",    "<time.h>"},
        {"strftime",    "<time.h>"},
        {"sqrt",        "<math.h>"},   {"pow",       "<math.h>"},
        {"sin",         "<math.h>"},   {"cos",       "<math.h>"},
        {"log",         "<math.h>"},   {"exp",       "<math.h>"},
        {"floor",       "<math.h>"},   {"ceil",      "<math.h>"},
        {"fabs",        "<math.h>"},
        {"assert",      "<assert.h>"},
        // POSIX
        {"open",        "<fcntl.h>"},  {"close",     "<unistd.h>"},
        {"read",        "<unistd.h>"}, {"write",     "<unistd.h>"},
        {"socket",      "<sys/socket.h>"}, {"bind",  "<sys/socket.h>"},
        {"connect",     "<sys/socket.h>"}, {"accept","<sys/socket.h>"},
        {"send",        "<sys/socket.h>"}, {"recv",  "<sys/socket.h>"},
        {"getaddrinfo", "<netdb.h>"},
        {"pthread_create", "<pthread.h>"}, {"pthread_join",  "<pthread.h>"},
        {"pthread_mutex_lock", "<pthread.h>"},
        {"sem_init",    "<semaphore.h>"},
        // OpenSSL
        {"SSL_new",     "<openssl/ssl.h>"}, {"SSL_connect",   "<openssl/ssl.h>"},
        {"EVP_DigestInit", "<openssl/evp.h>"}, {"EVP_EncryptInit", "<openssl/evp.h>"},
        {"BIO_new",     "<openssl/bio.h>"},
        // CUDA
        {"cudaMalloc",  "<cuda_runtime.h>"}, {"cudaMemcpy",   "<cuda_runtime.h>"},
        {"cudaLaunchKernel", "<cuda_runtime.h>"},
        {"cuLaunchKernel",   "<cuda.h>"},
        // SQLite
        {"sqlite3_open", "<sqlite3.h>"}, {"sqlite3_exec", "<sqlite3.h>"},
        // libcurl
        {"curl_easy_init", "<curl/curl.h>"}, {"curl_easy_perform", "<curl/curl.h>"},
        // zlib
        {"inflate",     "<zlib.h>"},   {"deflate",   "<zlib.h>"},
        {"inflateInit", "<zlib.h>"},
        // Lua
        {"lua_newstate", "<lua.h>"}, {"luaL_openlibs", "<lualib.h>"},
    };
    return kMap;
}

std::vector<std::string>
HeaderInference::infer(const std::vector<std::string>& libSyms) const {
    const auto& m = symbolHeaderMap();
    std::unordered_set<std::string> seen;
    std::vector<std::string> result;
    for (const auto& sym : libSyms) {
        auto it = m.find(sym);
        if (it != m.end() && !seen.count(it->second)) {
            seen.insert(it->second);
            result.push_back(it->second);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ─── CMakeEmitter ─────────────────────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> kLibToPackage = {
    {"ssl",    "OpenSSL REQUIRED COMPONENTS SSL"},
    {"crypto", "OpenSSL REQUIRED COMPONENTS Crypto"},
    {"cuda",   "CUDA REQUIRED"},
    {"thread", "Threads REQUIRED"},
    {"openmp", "OpenMP REQUIRED"},
    {"zlib",   "ZLIB REQUIRED"},
    {"png",    "PNG REQUIRED"},
    {"sqlite", "SQLite3 REQUIRED"},
    {"curl",   "CURL REQUIRED"},
    {"boost",  "Boost REQUIRED"},
    {"lua",    "Lua REQUIRED"},
    {"qt",     "Qt6 REQUIRED COMPONENTS Core Widgets"},
};

std::string CMakeEmitter::libToFindPackage(const std::string& lib) {
    auto it = kLibToPackage.find(lib);
    return it != kLibToPackage.end() ? it->second : "";
}

std::string CMakeEmitter::emitFindPackages(const ClusterResult& result) const {
    std::unordered_set<std::string> found;
    std::ostringstream os;
    for (const auto& mod : result.modules) {
        for (const auto& h : mod.headers) {
            std::string pkg;
            if (h.find("openssl") != std::string::npos)  pkg = libToFindPackage("ssl");
            else if (h.find("cuda_runtime") != std::string::npos) pkg = libToFindPackage("cuda");
            else if (h == "<cuda.h>")  pkg = libToFindPackage("cuda");
            else if (h == "<pthread.h>") pkg = libToFindPackage("thread");
            else if (h == "<zlib.h>")    pkg = libToFindPackage("zlib");
            else if (h.find("sqlite3") != std::string::npos) pkg = libToFindPackage("sqlite");
            else if (h.find("curl") != std::string::npos)    pkg = libToFindPackage("curl");
            if (!pkg.empty() && !found.count(pkg)) {
                found.insert(pkg);
                os << "find_package(" << pkg << ")\n";
            }
        }
    }
    return os.str();
}

static std::string headerToLinkTarget(const std::string& h) {
    if (h.find("openssl/ssl") != std::string::npos) return "OpenSSL::SSL";
    if (h.find("openssl/evp") != std::string::npos) return "OpenSSL::Crypto";
    if (h.find("openssl")     != std::string::npos) return "OpenSSL::SSL OpenSSL::Crypto";
    if (h.find("cuda_runtime") != std::string::npos) return "CUDA::cudart";
    if (h == "<cuda.h>")      return "CUDA::cuda_driver";
    if (h == "<pthread.h>")   return "Threads::Threads";
    if (h == "<zlib.h>")      return "ZLIB::ZLIB";
    if (h.find("sqlite3") != std::string::npos) return "SQLite::SQLite3";
    if (h.find("curl")    != std::string::npos) return "CURL::libcurl";
    return "";
}

std::string CMakeEmitter::emitModuleTarget(const Module& mod) const {
    std::ostringstream os;
    os << "# Module: " << mod.name;
    if (mod.cohesion > 0.0)
        os << "  (cohesion=" << std::fixed << std::setprecision(2) << mod.cohesion << ")";
    os << "\n";
    os << "add_library(" << mod.name << " STATIC\n";
    for (const auto& fn : mod.functions)
        os << "    # " << fn << "\n";
    os << "    " << mod.name << ".cpp\n";
    os << ")\n";
    os << "target_include_directories(" << mod.name << " PRIVATE include)\n";

    // Link to third-party libraries
    std::unordered_set<std::string> linkTargets;
    for (const auto& h : mod.headers) {
        std::string t = headerToLinkTarget(h);
        if (!t.empty()) linkTargets.insert(t);
    }
    // Link to internal dependencies
    for (const auto& dep : mod.dependencies)
        linkTargets.insert(dep);

    if (!linkTargets.empty()) {
        os << "target_link_libraries(" << mod.name << " PRIVATE\n";
        for (const auto& t : linkTargets)
            os << "    " << t << "\n";
        os << ")\n";
    }
    os << "\n";
    return os.str();
}

std::string CMakeEmitter::emitEntryTarget(const Module& mod,
                                           const ClusterResult& result,
                                           const CMakeConfig& cfg) const {
    std::ostringstream os;
    os << "# Entry-point executable\n";
    os << "add_executable(" << cfg.projectName << "\n";
    os << "    main.cpp\n";
    os << ")\n";
    os << "target_link_libraries(" << cfg.projectName << " PRIVATE\n";
    for (const auto& m : result.modules) {
        if (!m.isEntryModule)
            os << "    " << m.name << "\n";
    }
    os << ")\n\n";

    if (cfg.addInstall) {
        os << "install(TARGETS " << cfg.projectName << "\n";
        os << "    RUNTIME DESTINATION bin\n";
        os << ")\n\n";
    }
    (void)mod;
    return os.str();
}

std::string CMakeEmitter::emit(const ClusterResult& result,
                                const CMakeConfig& cfg) const {
    std::ostringstream os;

    // Prefer the project name embedded in the ClusterResult; fall back to cfg.
    const std::string& projectName = !result.projectName.empty()
                                     ? result.projectName
                                     : cfg.projectName;

    os << "cmake_minimum_required(VERSION " << cfg.minCmakeVersion << ")\n";
    os << "project(" << projectName << " CXX)\n";
    os << "set(CMAKE_CXX_STANDARD " << cfg.cxxStandard << ")\n";
    os << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    // find_package blocks
    std::string pkgs = emitFindPackages(result);
    if (!pkgs.empty()) {
        os << "# Third-party dependencies\n";
        os << pkgs << "\n";
    }

    // Module libraries (non-entry)
    for (const auto& mod : result.modules) {
        if (!mod.isEntryModule)
            os << emitModuleTarget(mod);
    }

    // Entry-point executable
    const Module* entry = result.entryModule();
    if (entry)
        os << emitEntryTarget(*entry, result, cfg);

    // Testing
    if (cfg.addTesting) {
        os << "# Testing\n";
        os << "enable_testing()\n";
        os << "find_package(GTest REQUIRED)\n\n";
        for (const auto& mod : result.modules) {
            if (!mod.isEntryModule) {
                os << "add_executable(" << mod.name << "_tests\n";
                os << "    tests/" << mod.name << "_test.cpp\n";
                os << ")\n";
                os << "target_link_libraries(" << mod.name << "_tests PRIVATE\n";
                os << "    " << mod.name << " GTest::gtest_main\n";
                os << ")\n";
                os << "add_test(NAME " << mod.name << "_tests COMMAND "
                   << mod.name << "_tests)\n\n";
            }
        }
    }

    return os.str();
}

// ─── ModuleClusterer (orchestrator) ───────────────────────────────────────────

ModuleClusterer::ModuleClusterer(LouvainConfig cfg)
    : louvainCfg_(cfg) {}

void ModuleClusterer::applyStringLocalityRefinement(
    const CallGraph& graph,
    std::unordered_map<std::string, int>& communities) const {
    // Group functions that share ≥ 1 string literal reference
    std::unordered_map<std::string, std::vector<std::string>> strToFuncs;
    for (const auto& fn : graph.functions)
        for (const auto& s : fn.referencedStrings)
            strToFuncs[s].push_back(fn.name);

    for (const auto& [s, funcs] : strToFuncs) {
        if (funcs.size() < 2) continue;
        // Vote: assign all funcs the majority community among them
        std::unordered_map<int, int> votes;
        for (const auto& f : funcs) {
            auto it = communities.find(f);
            if (it != communities.end()) votes[it->second]++;
        }
        if (votes.empty()) continue;
        int majCom = std::max_element(votes.begin(), votes.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; })->first;
        for (const auto& f : funcs) {
            auto it = communities.find(f);
            if (it != communities.end()) it->second = majCom;
        }
    }
}

void ModuleClusterer::applyDebugSymbolRefinement(
    const CallGraph& graph,
    std::unordered_map<std::string, int>& communities) const {
    // If debug info has source file, group by file
    std::unordered_map<std::string, std::vector<std::string>> fileToFuncs;
    for (const auto& fn : graph.functions)
        if (!fn.sourceFile.empty())
            fileToFuncs[fn.sourceFile].push_back(fn.name);

    for (const auto& [file, funcs] : fileToFuncs) {
        if (funcs.empty()) continue;
        // All functions in the same source file → same community
        // Use first function's community as canonical
        int canon = -1;
        for (const auto& f : funcs) {
            auto it = communities.find(f);
            if (it != communities.end()) { canon = it->second; break; }
        }
        if (canon < 0) continue;
        for (const auto& f : funcs) {
            auto it = communities.find(f);
            if (it != communities.end()) it->second = canon;
        }
    }
}

Module ModuleClusterer::buildModule(
    int communityId,
    const std::vector<std::string>& funcNames,
    const CallGraph& graph,
    const std::unordered_map<std::string, int>& communities) const {
    Module mod;
    mod.communityId = communityId;
    mod.functions   = funcNames;

    // Collect metadata for functions in this module
    std::vector<FunctionMeta> metas;
    for (const auto& fn : graph.functions)
        if (communities.count(fn.name) && communities.at(fn.name) == communityId)
            metas.push_back(fn);

    // Module name
    ModuleNamer namer;
    mod.name = namer.name(metas, communityId);

    // Entry point
    for (const auto& meta : metas)
        if (meta.isEntryPoint) mod.isEntryModule = true;

    // Infer headers from library calls
    std::vector<std::string> allLibSyms;
    for (const auto& meta : metas)
        allLibSyms.insert(allLibSyms.end(),
                          meta.calledLibSyms.begin(), meta.calledLibSyms.end());
    HeaderInference hdr;
    mod.headers = hdr.infer(allLibSyms);

    return mod;
}

void ModuleClusterer::inferModuleDependencies(
    ClusterResult& result,
    const CallGraph& graph,
    const std::unordered_map<std::string, int>& communities) const {
    // Build: funcName → module name
    std::unordered_map<std::string, std::string> funcToMod;
    for (const auto& mod : result.modules)
        for (const auto& fn : mod.functions)
            funcToMod[fn] = mod.name;

    // For each edge, if caller and callee are in different modules → dependency
    std::unordered_map<std::string, std::unordered_set<std::string>> deps;
    for (const auto& edge : graph.edges) {
        auto itCaller = funcToMod.find(edge.caller);
        auto itCallee = funcToMod.find(edge.callee);
        if (itCaller == funcToMod.end() || itCallee == funcToMod.end()) continue;
        if (itCaller->second != itCallee->second)
            deps[itCaller->second].insert(itCallee->second);
    }

    for (auto& mod : result.modules) {
        auto it = deps.find(mod.name);
        if (it != deps.end())
            mod.dependencies.assign(it->second.begin(), it->second.end());
    }

    // Compute cohesion for each module
    for (auto& mod : result.modules) {
        int internal = 0, external = 0;
        for (const auto& edge : graph.edges) {
            bool callerIn = std::find(mod.functions.begin(), mod.functions.end(), edge.caller) != mod.functions.end();
            bool calleeIn = std::find(mod.functions.begin(), mod.functions.end(), edge.callee) != mod.functions.end();
            if (callerIn && calleeIn) internal++;
            else if (callerIn || calleeIn) external++;
        }
        mod.callEdgesInternal = internal;
        mod.callEdgesExternal = external;
        int total = internal + external;
        mod.cohesion = total > 0 ? static_cast<double>(internal) / total : 0.0;
    }
}

ClusterResult ModuleClusterer::cluster(const CallGraph& graph,
                                        const std::string& projectName) {
    ClusterResult result;
    result.projectName = projectName;

    if (graph.functions.empty()) return result;

    // Run Louvain
    LouvainClusterer louvain(louvainCfg_);
    auto communities = louvain.cluster(graph);
    result.modularity  = louvain.lastModularity();
    result.iterations  = louvain.lastIterations();

    // Apply refinements
    applyStringLocalityRefinement(graph, communities);
    applyDebugSymbolRefinement   (graph, communities);

    // Build community → function list
    std::unordered_map<int, std::vector<std::string>> comFuncs;
    for (const auto& [name, com] : communities)
        comFuncs[com].push_back(name);

    // Build modules
    for (const auto& [comId, funcs] : comFuncs)
        result.modules.push_back(buildModule(comId, funcs, graph, communities));

    // Sort modules: entry first, then by name
    std::sort(result.modules.begin(), result.modules.end(),
        [](const Module& a, const Module& b) {
            if (a.isEntryModule != b.isEntryModule)
                return a.isEntryModule > b.isEntryModule;
            return a.name < b.name;
        });

    // Infer inter-module dependencies
    inferModuleDependencies(result, graph, communities);

    return result;
}

} // namespace retdec::module_cluster
