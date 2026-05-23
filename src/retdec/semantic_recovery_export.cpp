/**
 * @file src/retdec/semantic_recovery_export.cpp
 * @brief Export post-pipeline semantic detections to config JSON and decompiled C.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/retdec/semantic_recovery_export.h"

#include <fstream>
#include <map>
#include <sstream>

namespace retdec {
namespace analysis {

namespace {

common::SemanticDetection makeDetection(
		const std::string& kind,
		const std::string& label,
		float confidence,
		const std::string& detail = {})
{
	common::SemanticDetection d;
	d.kind = kind;
	d.label = label;
	d.confidence = confidence;
	d.detail = detail;
	return d;
}

void appendDetection(
		SemanticDetectionMap& map,
		const std::string& fnName,
		common::SemanticDetection detection)
{
	if (fnName.empty())
	{
		return;
	}
	map[fnName].push_back(std::move(detection));
}

void collectConcurrencyDetections(
		const concurrency_detect::ConcurrencyModel& cm,
		SemanticDetectionMap& map)
{
	auto addFn = [&](const std::string& fnName,
	                 const std::string& label,
	                 float confidence,
	                 const std::string& detail) {
		appendDetection(map, fnName,
				makeDetection("concurrency", label, confidence, detail));
	};

	for (const auto& t : cm.threads)
	{
		addFn(t.funcName, "thread", 0.75f, t.threadFunc);
	}
	for (const auto& l : cm.locks)
	{
		addFn(l.funcName, "mutex", 0.75f, "");
	}
	for (const auto& a : cm.atomics)
	{
		addFn(a.funcName, "atomic", 0.70f, a.varName);
	}
	for (const auto& c : cm.condVars)
	{
		addFn(c.funcName, "condition_variable", 0.70f, "");
	}
	for (const auto& s : cm.spinlocks)
	{
		addFn(s.funcName, "spinlock", 0.65f, "");
	}
}

void injectSemanticCommentsIntoLines(
		std::vector<std::string>& lines,
		const config::Config& config)
{
	const std::string& outputLang = config.parameters.getOutputLang();
	std::map<int, std::vector<std::string>> inserts;
	for (const auto& fn : config.functions)
	{
		if (fn.semanticDetections.empty() || !fn.getStartLine().isDefined())
		{
			continue;
		}

		const int line = static_cast<int>(fn.getStartLine().getValue());
		if (line <= 0)
		{
			continue;
		}

		for (const auto& d : fn.semanticDetections)
		{
			const std::string comment = "// " + d.commentLine(outputLang);
			bool duplicate = false;
			if (line > 1 && line - 2 < static_cast<int>(lines.size()))
			{
				const auto& prev = lines[static_cast<std::size_t>(line - 2)];
				if (prev.find(comment) != std::string::npos)
				{
					duplicate = true;
				}
			}
			if (!duplicate)
			{
				inserts[line].push_back(comment);
			}
		}
	}

	if (inserts.empty())
	{
		return;
	}

	for (auto it = inserts.rbegin(); it != inserts.rend(); ++it)
	{
		const int idx = it->first - 1;
		if (idx < 0 || idx > static_cast<int>(lines.size()))
		{
			continue;
		}
		for (auto cit = it->second.rbegin(); cit != it->second.rend(); ++cit)
		{
			lines.insert(lines.begin() + idx, *cit);
		}
	}
}

} // anonymous namespace

SemanticDetectionMap buildSemanticDetectionMap(
		const container_detect::ContainerDetector::DetectionMap& containers,
		const std::vector<std::pair<std::string, algo_recover::AlgorithmResult>>& algos,
		const sort_detect::SortDetector::DetectionMap& sorts,
		const concurrency_detect::ConcurrencyModel& concurrency,
		const std::string& outputLang)
{
	SemanticDetectionMap map;
	const bool emitCHints = common::isCOutputLang(outputLang);

	for (const auto& [fnName, result] : containers)
	{
		const std::string label = !result.emittedType.empty()
				? result.emittedType
				: result.kindName();
		auto detection = makeDetection("container", label, result.confidence,
				result.toString());
		if (emitCHints)
		{
			detection.cHint = result.cHint();
			detection.cElemBytes = result.elementType.byteWidth;
		}
		appendDetection(map, fnName, std::move(detection));
	}

	for (const auto& [fnName, result] : algos)
	{
		appendDetection(map, fnName,
				makeDetection("algorithm", result.kindName(), result.confidence,
						result.toString()));
	}

	for (const auto& [fnName, result] : sorts)
	{
		appendDetection(map, fnName,
				makeDetection("sort", result.algorithmName(), result.confidence,
						result.toString()));
	}

	collectConcurrencyDetections(concurrency, map);
	return map;
}

void mergeSemanticDetectionsIntoConfig(
		config::Config& config,
		const SemanticDetectionMap& detections)
{
	for (const auto& [fnName, dets] : detections)
	{
		if (dets.empty())
		{
			continue;
		}

		common::Function key(fnName);
		auto it = config.functions.find(key);
		if (it == config.functions.end())
		{
			continue;
		}

		common::Function fn = *it;
		config.functions.erase(it);
		fn.semanticDetections = dets;
		config.functions.insert(std::move(fn));
	}
}

void injectSemanticCommentsIntoOutput(
		const config::Config& config,
		std::string* outString)
{
	std::vector<std::string> lines;
	const std::string& outPath = config.parameters.getOutputFile();

	if (!outPath.empty())
	{
		std::ifstream in(outPath);
		if (in)
		{
			std::string line;
			while (std::getline(in, line))
			{
				lines.push_back(line);
			}
		}
	}
	else if (outString && !outString->empty())
	{
		std::istringstream iss(*outString);
		std::string line;
		while (std::getline(iss, line))
		{
			lines.push_back(line);
		}
	}

	if (lines.empty())
	{
		return;
	}

	injectSemanticCommentsIntoLines(lines, config);

	if (!outPath.empty())
	{
		std::ofstream out(outPath, std::ios::trunc);
		for (std::size_t i = 0; i < lines.size(); ++i)
		{
			out << lines[i];
			if (i + 1 < lines.size())
			{
				out << '\n';
			}
		}
	}

	if (outString)
	{
		std::ostringstream oss;
		for (std::size_t i = 0; i < lines.size(); ++i)
		{
			oss << lines[i];
			if (i + 1 < lines.size())
			{
				oss << '\n';
			}
		}
		*outString = oss.str();
	}
}

void exportSemanticRecovery(
		config::Config& config,
		const SemanticDetectionMap& detections,
		std::string* outString)
{
	if (detections.empty())
	{
		return;
	}

	mergeSemanticDetectionsIntoConfig(config, detections);

	if (!config.parameters.getOutputConfigFile().empty())
	{
		config.generateJsonFile();
	}

	injectSemanticCommentsIntoOutput(config, outString);
}

} // namespace analysis
} // namespace retdec
