#include "retdec/neural/decompile_hook.h"
#include "retdec/neural/gates.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/refiner.h"

#include "retdec/common/function.h"
#include "retdec/common/semantic_detection.h"
#include "retdec/common/storage.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef RETDEC_HAS_TREE_SITTER
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-c.h>
#endif

namespace retdec::neural {

namespace {

bool envEnabled(const char* name)
{
	const char* v = std::getenv(name);
	return v && v[0] != '\0' && v[0] != '0';
}

std::string modelPathFromEnv()
{
	const char* p = std::getenv("RETDEC_NEURAL_MODEL");
	return p ? std::string(p) : std::string();
}

int envInt(const char* name, int fallback)
{
	const char* v = std::getenv(name);
	if (!v || !v[0]) return fallback;
	while (*v == ' ' || *v == '\t') ++v;
	int n = 0;
	const auto r = std::from_chars(v, v + std::strlen(v), n);
	if (r.ec != std::errc{}) return fallback;
	return n;
}

int tierMaxFromEnv()
{
	const int v = envInt("RETDEC_NEURAL_TIER_MAX", 3);
	return v < 1 ? 1 : (v > 5 ? 5 : v);
}

float envFloat(const char* name, float fallback)
{
	const char* v = std::getenv(name);
	if (!v || !v[0]) return fallback;
	char* end = nullptr;
	const float x = std::strtof(v, &end);
	if (end == v) return fallback;
	return x;
}

void writeSidecar(const std::string& basePath, const std::string& refined, const std::string& manifest)
{
	if (basePath.empty()) return;
	const std::string refinedPath = basePath + ".refined.c";
	const std::string manifestPath = basePath + ".refinement-manifest.json";
	std::ofstream(refinedPath) << refined;
	std::ofstream(manifestPath) << manifest;
}

std::string jsonEscape(const std::string& s)
{
	std::ostringstream oss;
	for (char c: s)
	{
		switch (c)
		{
		case '"': oss << "\\\""; break;
		case '\\': oss << "\\\\"; break;
		case '\n': oss << "\\n"; break;
		case '\r': oss << "\\r"; break;
		default: oss << c; break;
		}
	}
	return oss.str();
}

void appendStorageFields(std::ostringstream& oss, const retdec::common::Storage& st)
{
	if (st.isRegister())
	{
		const std::string n = st.getRegisterName();
		if (!n.empty()) oss << ",\"register\":\"" << jsonEscape(n) << '"';
		return;
	}
	if (st.isStack())
	{
		oss << ",\"stack_offset\":" << st.getStackOffset();
		return;
	}
	if (st.isMemory() && st.getAddress().isDefined())
		oss << ",\"address\":\"" << jsonEscape(st.getAddress().toHexPrefixString()) << '"';
}

#ifdef RETDEC_HAS_TREE_SITTER

struct CFunctionSpan
{
	std::string name;
	std::uint32_t start = 0;
	std::uint32_t end = 0;
};

std::string nodeText(const std::string& src, TSNode n)
{
	const uint32_t a = ts_node_start_byte(n);
	const uint32_t b = ts_node_end_byte(n);
	if (a > b || b > src.size()) return {};
	return src.substr(a, b - a);
}

std::string declaratorName(TSNode n, const std::string& src)
{
	if (ts_node_is_null(n)) return {};
	if (std::strcmp(ts_node_type(n), "identifier") == 0) return nodeText(src, n);
	TSNode inner = ts_node_child_by_field_name(n, "declarator", 10);
	if (!ts_node_is_null(inner))
	{
		const std::string name = declaratorName(inner, src);
		if (!name.empty()) return name;
	}
	const uint32_t nch = ts_node_child_count(n);
	for (uint32_t i = 0; i < nch; ++i)
	{
		const std::string name = declaratorName(ts_node_child(n, i), src);
		if (!name.empty()) return name;
	}
	return {};
}

void collectFunctionSpans(TSNode n, const std::string& src, std::vector<CFunctionSpan>& out)
{
	if (ts_node_is_error(n)) return;
	if (std::strcmp(ts_node_type(n), "function_definition") == 0)
	{
		TSNode decl = ts_node_child_by_field_name(n, "declarator", 10);
		CFunctionSpan span;
		span.name = declaratorName(decl, src);
		span.start = ts_node_start_byte(n);
		span.end = ts_node_end_byte(n);
		if (!span.name.empty() && span.end > span.start) out.push_back(std::move(span));
		return;
	}
	const uint32_t nch = ts_node_child_count(n);
	for (uint32_t i = 0; i < nch; ++i)
		collectFunctionSpans(ts_node_child(n, i), src, out);
}

std::vector<CFunctionSpan> extractCFunctionSpans(const std::string& src)
{
	std::vector<CFunctionSpan> out;
	TSParser* p = ts_parser_new();
	if (!p) return out;
	if (!ts_parser_set_language(p, tree_sitter_c()))
	{
		ts_parser_delete(p);
		return out;
	}
	TSTree* tree = ts_parser_parse_string(p, nullptr, src.data(), static_cast<uint32_t>(src.size()));
	if (!tree)
	{
		ts_parser_delete(p);
		return out;
	}
	TSNode root = ts_tree_root_node(tree);
	if (!ts_node_is_null(root))
		collectFunctionSpans(root, src, out);
	ts_tree_delete(tree);
	ts_parser_delete(p);
	return out;
}

#endif

struct RefinePassResult
{
	std::string source;
	std::string manifest;
	bool accepted = false;
};

RefinePassResult runTieredRefine(Refiner& refiner, std::string current, const std::string& semanticJson)
{
	static const RefinementTier kTiers[] = {
		RefinementTier::Naming,
		RefinementTier::Comments,
		RefinementTier::StructFields,
		RefinementTier::IdiomRecovery,
		RefinementTier::FullRewrite,
	};

	const int tierMax = tierMaxFromEnv();
	RefinePassResult result;
	result.source = current;
	result.manifest = R"({"accepted":false,"reason":"no tier ran"})";
	bool compileRetryUsed = false;
	const bool requireCompile = envEnabled("RETDEC_NEURAL_REQUIRE_COMPILE");

	for (int i = 0; i < tierMax && i < 5; ++i)
	{
		RefinementRequest req;
		req.functionSource = current;
		req.tier = kTiers[i];
		req.semanticContextJson = semanticJson;
		req.generation.reuseKvPrefix = envEnabled("RETDEC_NEURAL_REUSE_KV") && (i > 0);
		const bool conservativeTier =
			(req.tier == RefinementTier::Naming || req.tier == RefinementTier::Comments
			 || req.tier == RefinementTier::StructFields);
		req.generation.temperature = envFloat("RETDEC_NEURAL_TEMPERATURE", conservativeTier ? 0.0f : 0.6f);
		req.generation.topP = envFloat("RETDEC_NEURAL_TOP_P", 0.95f);
		const int topK = envInt("RETDEC_NEURAL_TOP_K", 20);
		if (topK > 0) req.generation.topK = topK;
		if (std::isfinite(req.generation.temperature))
		{
			if (req.generation.temperature < 0.0f)
				req.generation.temperature = 0.0f;
			else if (req.generation.temperature > 2.0f)
				req.generation.temperature = 2.0f;
		}
		if (std::isfinite(req.generation.topP))
		{
			if (req.generation.topP < 0.0f)
				req.generation.topP = 0.0f;
			else if (req.generation.topP > 1.0f)
				req.generation.topP = 1.0f;
		}
		req.generation.minP = 0.0f;
		req.generation.thinkingMode = envEnabled("RETDEC_NEURAL_THINKING");
		if (req.tier == RefinementTier::Naming)
		{
			req.generation.grammarGbnf = namingRenameMapGbnf();
			req.generation.grammarRoot = "root";
		}
		const int n = envInt("RETDEC_NEURAL_MAX_TOKENS", 0);
		if (n > 0) req.generation.maxTokens = n;

		const auto resp = refiner.refine(req);
		result.manifest = resp.manifestJson;
		if (resp.accepted)
		{
			current = resp.refinedSource;
			result.accepted = true;
		}

		const bool compileReject = !resp.accepted
								&& (resp.manifestJson.find("compile_syntax") != std::string::npos
									|| resp.manifestJson.find("compile=fail") != std::string::npos);
		if (compileRetryUsed) continue;
		if (!compileReject && !requireCompile) continue;

		std::string diags;
		const std::string attempt = resp.refinedSource.empty() ? current : resp.refinedSource;
		const bool compiles = compileSyntaxOnly(attempt, diags);
		if (resp.accepted && compiles) continue;

		compileRetryUsed = true;
		RefinementRequest retry = req;
		retry.functionSource = current;
		retry.tier = RefinementTier::FullRewrite;
		retry.generation.reuseKvPrefix = false;
		retry.compilerDiagnostics = diags.empty() ? std::string("cc -fsyntax-only failed") : diags;

		const auto retryResp = refiner.refine(retry);
		result.manifest = retryResp.manifestJson;
		if (retryResp.accepted && compileSyntaxOnly(retryResp.refinedSource))
		{
			current = retryResp.refinedSource;
			result.accepted = true;
		}
		else if (retryResp.accepted)
		{
			result.manifest = R"({"accepted":false,"reason":"compile_syntax"})";
		}
	}

	result.source = current;
	return result;
}

} // namespace

void appendJsonStringArray(std::ostringstream& oss, const char* key, const std::set<std::string>& values)
{
	if (values.empty()) return;
	oss << ",\"" << key << "\":[";
	bool first = true;
	for (const auto& v: values)
	{
		if (!first) oss << ',';
		first = false;
		oss << '"' << jsonEscape(v) << '"';
	}
	oss << ']';
}

void buildCallGraph(
	const retdec::config::Config& config,
	std::map<std::string, std::set<std::string>>& callersOf,
	std::map<std::string, std::set<std::string>>& calleesOf)
{
	for (const auto& callee: config.functions)
	{
		for (const auto& site: callee.codeReferences)
		{
			if (!site.isDefined()) continue;
			for (const auto& caller: config.functions)
			{
				if (caller.getName() == callee.getName()) continue;
				if (!caller.getStart().isDefined() || !caller.getEnd().isDefined()) continue;
				if (!caller.contains(site)) continue;
				callersOf[callee.getName()].insert(caller.getName());
				calleesOf[caller.getName()].insert(callee.getName());
			}
		}
	}
}

std::string serializeSemanticContext(const retdec::config::Config& config)
{
	std::map<std::string, std::set<std::string>> callersOf;
	std::map<std::string, std::set<std::string>> calleesOf;
	buildCallGraph(config, callersOf, calleesOf);

	std::ostringstream oss;
	oss << "{\"functions\":[";
	bool firstFn = true;
	for (const auto& fn: config.functions)
	{
		const bool hasDetections = !fn.semanticDetections.empty();
		const bool hasCrypto = !fn.usedCryptoConstants.empty();
		const bool hasDemangled = !fn.getDemangledName().empty();
		const bool hasDecl = !fn.getDeclarationString().empty();
		const bool hasCallGraph = callersOf.count(fn.getName()) || calleesOf.count(fn.getName());
		if (!hasDetections && !hasCrypto && !hasDemangled && !hasDecl && !hasCallGraph) continue;
		if (!firstFn) oss << ',';
		firstFn = false;
		oss << "{\"name\":\"" << jsonEscape(fn.getName()) << '"';
		if (hasDemangled) oss << ",\"demangled\":\"" << jsonEscape(fn.getDemangledName()) << '"';
		if (fn.getStart().isDefined()) oss << ",\"start\":\"" << jsonEscape(fn.getStart().toHexPrefixString()) << '"';
		if (fn.getEnd().isDefined()) oss << ",\"end\":\"" << jsonEscape(fn.getEnd().toHexPrefixString()) << '"';
		if (hasDecl) oss << ",\"declaration\":\"" << jsonEscape(fn.getDeclarationString()) << '"';
		if (!fn.getRealName().empty() && fn.getRealName() != fn.getName())
			oss << ",\"real_name\":\"" << jsonEscape(fn.getRealName()) << '"';
		if (!fn.getSourceFileName().empty()) oss << ",\"source_file\":\"" << jsonEscape(fn.getSourceFileName()) << '"';
		if (fn.getStartLine().isDefined()) oss << ",\"start_line\":" << fn.getStartLine().getValue();
		if (fn.getEndLine().isDefined()) oss << ",\"end_line\":" << fn.getEndLine().getValue();
		if (!fn.getWrappedFunctionName().empty())
			oss << ",\"wrapped\":\"" << jsonEscape(fn.getWrappedFunctionName()) << '"';
		if (fn.isFromDebug()) oss << ",\"from_debug\":true";
		if (fn.isConstructor()) oss << ",\"constructor\":true";
		if (fn.isDestructor()) oss << ",\"destructor\":true";
		if (fn.isVirtual()) oss << ",\"virtual\":true";
		if (fn.isVariadic()) oss << ",\"variadic\":true";
		if (fn.isExported()) oss << ",\"exported\":true";
		if (fn.isThumb()) oss << ",\"thumb\":true";
		if (fn.isSyscall()) oss << ",\"syscall\":true";
		if (fn.isIdiom()) oss << ",\"idiom\":true";
		if (fn.isStaticallyLinked()) oss << ",\"statically_linked\":true";
		if (fn.isDynamicallyLinked()) oss << ",\"dynamically_linked\":true";
		if (fn.callingConvention.isKnown())
		{
			std::ostringstream cc;
			cc << fn.callingConvention;
			oss << ",\"calling_convention\":\"" << jsonEscape(cc.str()) << '"';
		}
		if (fn.returnType.isDefined()) oss << ",\"return_type\":\"" << jsonEscape(fn.returnType.getId()) << '"';
		if (fn.returnStorage.isDefined())
		{
			oss << ",\"return_storage\":{";
			std::ostringstream body;
			appendStorageFields(body, fn.returnStorage);
			const std::string fields = body.str();
			if (!fields.empty() && fields.front() == ',') oss << fields.substr(1);
			oss << '}';
		}
		if (fn.frameBaseStorage.isDefined())
		{
			oss << ",\"frame_base\":{";
			std::ostringstream body;
			appendStorageFields(body, fn.frameBaseStorage);
			const std::string fields = body.str();
			if (!fields.empty() && fields.front() == ',') oss << fields.substr(1);
			oss << '}';
		}
		if (!fn.parameters.empty())
		{
			oss << ",\"parameters\":[";
			bool firstP = true;
			for (const auto& p: fn.parameters)
			{
				if (!firstP) oss << ',';
				firstP = false;
				oss << "{\"name\":\"" << jsonEscape(p.getName()) << '"';
				if (p.type.isDefined()) oss << ",\"type\":\"" << jsonEscape(p.type.getId()) << '"';
				if (!p.getRealName().empty() && p.getRealName() != p.getName())
					oss << ",\"real_name\":\"" << jsonEscape(p.getRealName()) << '"';
				if (p.isFromDebug()) oss << ",\"from_debug\":true";
				appendStorageFields(oss, p.getStorage());
				oss << '}';
			}
			oss << ']';
		}
		if (hasCrypto)
		{
			oss << ",\"used_crypto\":[";
			bool firstC = true;
			for (const auto& c: fn.usedCryptoConstants)
			{
				if (!firstC) oss << ',';
				firstC = false;
				oss << '"' << jsonEscape(c) << '"';
			}
			oss << ']';
		}
		oss << ",\"detections\":[";
		bool firstDet = true;
		for (const auto& d: fn.semanticDetections)
		{
			if (!firstDet) oss << ',';
			firstDet = false;
			oss << "{\"kind\":\"" << jsonEscape(d.kind) << "\",\"label\":\"" << jsonEscape(d.label)
				<< "\",\"confidence\":" << d.confidence;
			if (!d.detail.empty()) oss << ",\"detail\":\"" << jsonEscape(d.detail) << '"';
			if (!d.cHint.empty()) oss << ",\"cHint\":\"" << jsonEscape(d.cHint) << '"';
			oss << '}';
		}
		oss << ']';
		const auto callersIt = callersOf.find(fn.getName());
		if (callersIt != callersOf.end()) appendJsonStringArray(oss, "callers", callersIt->second);
		const auto calleesIt = calleesOf.find(fn.getName());
		if (calleesIt != calleesOf.end()) appendJsonStringArray(oss, "callees", calleesIt->second);
		oss << '}';
	}
	oss << "],\"classes\":[";
	bool firstCl = true;
	for (const auto& cl: config.classes)
	{
		if (cl.getName().empty() && cl.getDemangledName().empty()) continue;
		if (!firstCl) oss << ',';
		firstCl = false;
		oss << "{\"name\":\"" << jsonEscape(cl.getName()) << '"';
		if (!cl.getDemangledName().empty()) oss << ",\"demangled\":\"" << jsonEscape(cl.getDemangledName()) << '"';
		if (!cl.getSuperClasses().empty())
		{
			oss << ",\"super_classes\":[";
			bool firstS = true;
			for (const auto& s: cl.getSuperClasses())
			{
				if (!firstS) oss << ',';
				firstS = false;
				oss << '"' << jsonEscape(s) << '"';
			}
			oss << ']';
		}
		appendJsonStringArray(oss, "constructors", cl.constructors);
		appendJsonStringArray(oss, "destructors", cl.destructors);
		appendJsonStringArray(oss, "methods", cl.methods);
		appendJsonStringArray(oss, "virtual_methods", cl.virtualMethods);
		appendJsonStringArray(oss, "virtual_tables", cl.virtualTables);
		oss << '}';
	}
	oss << "],\"vtables\":[";
	bool firstVt = true;
	for (const auto& vt: config.vtables)
	{
		if (vt.getName().empty() && vt.items.empty()) continue;
		if (!firstVt) oss << ',';
		firstVt = false;
		oss << "{\"name\":\"" << jsonEscape(vt.getName()) << '"';
		if (vt.getAddress().isDefined())
			oss << ",\"address\":\"" << jsonEscape(vt.getAddress().toHexPrefixString()) << '"';
		if (!vt.items.empty())
		{
			oss << ",\"targets\":[";
			bool firstT = true;
			for (const auto& item: vt.items)
			{
				if (item.getTargetFunctionName().empty() && !item.getAddress().isDefined()
					&& !item.getTargetFunctionAddress().isDefined())
					continue;
				if (!firstT) oss << ',';
				firstT = false;
				oss << '{';
				bool firstF = true;
				if (!item.getTargetFunctionName().empty())
				{
					oss << "\"name\":\"" << jsonEscape(item.getTargetFunctionName()) << '"';
					firstF = false;
				}
				if (item.getAddress().isDefined())
				{
					if (!firstF) oss << ',';
					firstF = false;
					oss << "\"address\":\"" << jsonEscape(item.getAddress().toHexPrefixString()) << '"';
				}
				if (item.getTargetFunctionAddress().isDefined())
				{
					if (!firstF) oss << ',';
					firstF = false;
					oss << "\"target\":\"" << jsonEscape(item.getTargetFunctionAddress().toHexPrefixString()) << '"';
				}
				if (item.isThumb())
				{
					if (!firstF) oss << ',';
					oss << "\"thumb\":true";
				}
				oss << '}';
			}
			oss << ']';
		}
		oss << '}';
	}
	oss << "],\"patterns\":[";
	bool firstPat = true;
	for (const auto& pat: config.patterns)
	{
		if (pat.getName().empty() && pat.getYaraRuleName().empty()) continue;
		if (!firstPat) oss << ',';
		firstPat = false;
		oss << "{\"name\":\"" << jsonEscape(pat.getName()) << '"';
		if (!pat.getYaraRuleName().empty()) oss << ",\"yara_rule\":\"" << jsonEscape(pat.getYaraRuleName()) << '"';
		const char* kind = "other";
		if (pat.isTypeCrypto())
			kind = "crypto";
		else if (pat.isTypeMalware())
			kind = "malware";
		oss << ",\"type\":\"" << kind << '"';
		if (pat.isEndianLittle())
			oss << ",\"endian\":\"little\"";
		else if (pat.isEndianBig())
			oss << ",\"endian\":\"big\"";
		if (!pat.matches.empty())
		{
			oss << ",\"matches\":[";
			bool firstM = true;
			for (const auto& m: pat.matches)
			{
				if (!firstM) oss << ',';
				firstM = false;
				oss << '{';
				bool firstF = true;
				if (m.isOffsetDefined())
				{
					oss << "\"offset\":\"" << jsonEscape(m.getOffset().toHexPrefixString()) << '"';
					firstF = false;
				}
				if (m.isAddressDefined())
				{
					if (!firstF) oss << ',';
					oss << "\"address\":\"" << jsonEscape(m.getAddress().toHexPrefixString()) << '"';
					firstF = false;
				}
				if (m.isSizeDefined())
				{
					if (!firstF) oss << ',';
					oss << "\"size\":" << *m.getSize();
					firstF = false;
				}
				if (m.isEntrySizeDefined())
				{
					if (!firstF) oss << ',';
					oss << "\"entry_size\":" << *m.getEntrySize();
					firstF = false;
				}
				if (m.isTypeIntegral())
				{
					if (!firstF) oss << ',';
					oss << "\"type\":\"integral\"";
				}
				else if (m.isTypeFloatingPoint())
				{
					if (!firstF) oss << ',';
					oss << "\"type\":\"floating_point\"";
				}
				oss << '}';
			}
			oss << ']';
		}
		oss << '}';
	}
	oss << "],\"tools\":[";
	bool firstTool = true;
	for (const auto& tool: config.tools)
	{
		if (tool.getName().empty() && tool.getType().empty()) continue;
		if (!firstTool) oss << ',';
		firstTool = false;
		oss << "{\"name\":\"" << jsonEscape(tool.getName()) << '"';
		if (!tool.getType().empty()) oss << ",\"type\":\"" << jsonEscape(tool.getType()) << '"';
		if (!tool.getVersion().empty()) oss << ",\"version\":\"" << jsonEscape(tool.getVersion()) << '"';
		if (tool.getPercentage() != 0.0) oss << ",\"percentage\":" << tool.getPercentage();
		if (tool.isFromHeuristics()) oss << ",\"heuristics\":true";
		if (tool.getTotalSignificantNibbles() != 0)
			oss << ",\"totalSignificantNibbles\":" << tool.getTotalSignificantNibbles();
		if (tool.getIdenticalSignificantNibbles() != 0)
			oss << ",\"identicalSignificantNibbles\":" << tool.getIdenticalSignificantNibbles();
		oss << '}';
	}
	oss << "],\"languages\":[";
	bool firstLang = true;
	for (const auto& lang: config.languages)
	{
		if (lang.getName().empty()) continue;
		if (!firstLang) oss << ',';
		firstLang = false;
		oss << "{\"name\":\"" << jsonEscape(lang.getName()) << '"';
		if (lang.isBytecode()) oss << ",\"bytecode\":true";
		if (lang.isModuleCountSet()) oss << ",\"module_count\":" << lang.getModuleCount();
		oss << '}';
	}
	oss << ']';
	if (config.architecture.isKnown())
	{
		oss << ",\"architecture\":{\"name\":\"" << jsonEscape(config.architecture.getName()) << '"';
		oss << ",\"bit_size\":" << config.architecture.getBitSize();
		if (config.architecture.isEndianKnown())
			oss << ",\"endian\":\"" << (config.architecture.isEndianLittle() ? "little" : "big") << '"';
		oss << '}';
	}
	if (config.fileFormat.isKnown())
	{
		oss << ",\"file_format\":\"" << jsonEscape(config.fileFormat.getName()) << '"';
	}
	if (config.fileFormat.getFileClassBits() != 0)
		oss << ",\"file_class_bits\":" << config.fileFormat.getFileClassBits();
	if (config.fileType.isKnown())
	{
		const char* kind = "unknown";
		if (config.fileType.isShared())
			kind = "shared";
		else if (config.fileType.isArchive())
			kind = "archive";
		else if (config.fileType.isObject())
			kind = "object";
		else if (config.fileType.isExecutable())
			kind = "executable";
		oss << ",\"file_type\":\"" << kind << '"';
	}
	oss << '}';
	return oss.str();
}

std::vector<std::string> orderFunctionsCalleeFirst(
	const std::map<std::string, std::set<std::string>>& calleesOf,
	const std::map<std::string, std::uint64_t>& startAddr)
{
	std::set<std::string> names;
	for (const auto& kv: startAddr)
		names.insert(kv.first);
	for (const auto& kv: calleesOf)
	{
		names.insert(kv.first);
		for (const auto& c: kv.second)
			names.insert(c);
	}

	auto addrOf = [&](const std::string& n) -> std::uint64_t {
		const auto it = startAddr.find(n);
		return it == startAddr.end() ? 0 : it->second;
	};
	auto before = [&](const std::string& a, const std::string& b) {
		const std::uint64_t aa = addrOf(a);
		const std::uint64_t bb = addrOf(b);
		if (aa != bb) return aa < bb;
		return a < b;
	};

	std::map<std::string, int> indeg;
	std::map<std::string, std::vector<std::string>> succ;
	for (const auto& n: names)
		indeg[n] = 0;
	for (const auto& kv: calleesOf)
	{
		if (!names.count(kv.first)) continue;
		for (const auto& callee: kv.second)
		{
			if (callee == kv.first || !names.count(callee)) continue;
			succ[callee].push_back(kv.first);
			++indeg[kv.first];
		}
	}

	std::set<std::string, decltype(before)> ready(before);
	std::set<std::string> remaining(names.begin(), names.end());
	for (const auto& n: names)
	{
		if (indeg[n] == 0) ready.insert(n);
	}

	std::vector<std::string> order;
	order.reserve(names.size());
	while (order.size() < names.size())
	{
		if (ready.empty())
		{
			std::string pick;
			for (const auto& n: remaining)
			{
				if (pick.empty() || before(n, pick)) pick = n;
			}
			if (pick.empty()) break;
			ready.insert(pick);
		}
		const std::string n = *ready.begin();
		ready.erase(ready.begin());
		if (!remaining.erase(n)) continue;
		order.push_back(n);
		const auto sit = succ.find(n);
		if (sit == succ.end()) continue;
		for (const auto& caller: sit->second)
		{
			if (!remaining.count(caller)) continue;
			if (--indeg[caller] == 0) ready.insert(caller);
		}
	}
	return order;
}

std::string appendRefinedCalleesJson(
	const std::string& semanticJson,
	const std::map<std::string, std::string>& refinedByName,
	const std::set<std::string>& calleeNames)
{
	std::ostringstream extra;
	extra << "\"refined_callees\":[";
	bool first = true;
	for (const auto& name: calleeNames)
	{
		const auto it = refinedByName.find(name);
		if (it == refinedByName.end()) continue;
		if (!first) extra << ',';
		first = false;
		extra << "{\"name\":\"" << jsonEscape(name) << "\",\"source\":\"" << jsonEscape(it->second) << "\"}";
	}
	extra << ']';
	if (first) return semanticJson;
	if (semanticJson.empty()) return std::string("{") + extra.str() + '}';
	if (semanticJson.back() != '}') return semanticJson;
	return semanticJson.substr(0, semanticJson.size() - 1) + ',' + extra.str() + '}';
}

std::vector<std::string> extractCFunctionNames(const std::string& src)
{
	std::vector<std::string> names;
#ifdef RETDEC_HAS_TREE_SITTER
	for (const auto& span: extractCFunctionSpans(src))
		names.push_back(span.name);
#else
	(void)src;
#endif
	return names;
}

void maybeRefineDecompilerOutput(retdec::config::Config& config, std::string* outString)
{
	std::string fileBuf;
	if (!outString || outString->empty())
	{
		const std::string path = config.parameters.getOutputFile();
		if (path.empty()) return;
		std::ifstream in(path);
		if (!in) return;
		fileBuf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		if (fileBuf.empty()) return;
		outString = &fileBuf;
	}
	if (!envEnabled("RETDEC_NEURAL_REFINE")) return;

#ifndef RETDEC_NEURAL_OFFLINE_ONLY
	if (envEnabled("RETDEC_NO_NETWORK"))
	{
		if (!envEnabled("RETDEC_NEURAL_ALLOW_NETWORK")) return;
	}
#endif

	const std::string model = modelPathFromEnv();
	if (model.empty()) return;

	std::unique_ptr<Inference> backend;
	if (envEnabled("RETDEC_NEURAL_FORCE_MOCK"))
	{
#if defined(NDEBUG) && !defined(RETDEC_NEURAL_ALLOW_MOCK)
		std::fprintf(stderr, "retdec-neural: mock inference is disabled in release builds\n");
		return;
#else
		backend = createMockInference();
#endif
	}
	else
	{
		backend = createLlamaInference();
		if (!backend)
		{
#if defined(NDEBUG) && !defined(RETDEC_NEURAL_ALLOW_MOCK)
			std::fprintf(stderr, "retdec-neural: no llama.cpp backend; mock fallback disabled in release\n");
			return;
#else
			backend = createMockInference();
#endif
		}
	}
	int ctxLen = 4096;
	const int v = envInt("RETDEC_NEURAL_CTX", 0);
	if (v >= 512) ctxLen = v;
	if (!backend->loadModel(model, ctxLen))
	{
		std::fprintf(stderr, "retdec-neural: failed to load GGUF: %s\n", model.c_str());
		const std::string outPath = config.parameters.getOutputFile();
		if (!outPath.empty())
		{
			std::ofstream(outPath + ".refinement-manifest.json")
				<< R"({"accepted":false,"reason":"failed to load GGUF"})";
		}
		return;
	}

	Refiner refiner(std::move(backend));
	const std::string semanticJson = serializeSemanticContext(config);
	std::string current = *outString;
	std::string lastManifest = R"({"accepted":false,"reason":"no tier ran"})";
	bool anyAccepted = false;

#ifdef RETDEC_HAS_TREE_SITTER
	const auto spans = extractCFunctionSpans(current);
	if (spans.size() >= 2)
	{
		std::map<std::string, std::set<std::string>> callersOf;
		std::map<std::string, std::set<std::string>> calleesOf;
		buildCallGraph(config, callersOf, calleesOf);

		std::map<std::string, CFunctionSpan> byName;
		std::map<std::string, std::uint64_t> startAddr;
		for (const auto& span: spans)
		{
			if (byName.count(span.name)) continue;
			byName[span.name] = span;
			startAddr[span.name] = span.start;
		}
		for (const auto& fn: config.functions)
		{
			if (!byName.count(fn.getName()) || !fn.getStart().isDefined()) continue;
			startAddr[fn.getName()] = fn.getStart().getValue();
		}

		std::map<std::string, std::set<std::string>> localCallees;
		for (const auto& kv: calleesOf)
		{
			if (!byName.count(kv.first)) continue;
			for (const auto& c: kv.second)
			{
				if (byName.count(c)) localCallees[kv.first].insert(c);
			}
		}

		const auto order = orderFunctionsCalleeFirst(localCallees, startAddr);
		std::map<std::string, std::string> refinedByName;
		std::vector<std::pair<CFunctionSpan, std::string>> replacements;

		for (const auto& name: order)
		{
			const auto sit = byName.find(name);
			if (sit == byName.end()) continue;
			const auto& span = sit->second;
			if (span.end > current.size() || span.start >= span.end) continue;
			const std::string src = current.substr(span.start, span.end - span.start);
			const auto calIt = localCallees.find(name);
			const std::set<std::string> calleeNames =
				calIt == localCallees.end() ? std::set<std::string>{} : calIt->second;
			const std::string sem = appendRefinedCalleesJson(semanticJson, refinedByName, calleeNames);
			const auto pass = runTieredRefine(refiner, src, sem);
			lastManifest = pass.manifest;
			if (pass.accepted)
			{
				refinedByName[name] = pass.source;
				replacements.push_back({span, pass.source});
				anyAccepted = true;
			}
		}

		std::sort(
			replacements.begin(),
			replacements.end(),
			[](const auto& a, const auto& b) { return a.first.start > b.first.start; });
		for (const auto& r: replacements)
			current.replace(r.first.start, r.first.end - r.first.start, r.second);
	}
	else
#endif
	{
		const auto pass = runTieredRefine(refiner, current, semanticJson);
		current = pass.source;
		lastManifest = pass.manifest;
		anyAccepted = pass.accepted;
	}

	const std::string outPath = config.parameters.getOutputFile();
	if (anyAccepted)
	{
		writeSidecar(outPath, current, lastManifest);
	}
	else if (!outPath.empty())
	{
		std::ofstream(outPath + ".refinement-manifest.json") << lastManifest;
		std::fprintf(stderr, "retdec-neural: no tier accepted; %s\n", lastManifest.c_str());
	}
}

} // namespace retdec::neural
