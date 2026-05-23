/**
 * @file include/retdec/common/semantic_detection.h
 * @brief Post-pipeline semantic recovery detection record.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_COMMON_SEMANTIC_DETECTION_H
#define RETDEC_COMMON_SEMANTIC_DETECTION_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <string>

namespace retdec {
namespace common {

inline bool isCppOutputLang(const std::string& outputLang)
{
	std::string t = outputLang;
	std::transform(t.begin(), t.end(), t.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return t == "cpp" || t == "c++" || t == "cxx";
}

inline bool isCOutputLang(const std::string& outputLang)
{
	if (outputLang.empty())
	{
		return true;
	}
	std::string t = outputLang;
	std::transform(t.begin(), t.end(), t.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return t == "c";
}

struct SemanticDetection {
	std::string kind;       ///< e.g. "container", "sort", "algorithm", "concurrency"
	std::string label;      ///< e.g. "std::vector", "introsort"
	float confidence = 0.0f;
	std::string detail;
	std::string cHint;      ///< C-layout hint when output is C (e.g. "vector_like_3ptr")
	uint8_t cElemBytes = 0; ///< element size for C-friendly comment text (not serialized)

	std::string commentLine(const std::string& outputLang = {}) const
	{
		if (isCppOutputLang(outputLang))
		{
			return cppStyleCommentLine();
		}
		if (!cHint.empty())
		{
			return cContainerCommentLine();
		}
		return cppStyleCommentLine();
	}

private:
	std::string cppStyleCommentLine() const
	{
		std::ostringstream oss;
		oss << "[RetDec] " << label << " detected (confidence "
		    << std::fixed << std::setprecision(2) << confidence << ')';
		return oss.str();
	}

	std::string cContainerCommentLine() const
	{
		std::ostringstream oss;
		oss << "[RetDec] " << cHintPhrase(cHint);
		if (cElemBytes > 0)
		{
			oss << ", elem " << static_cast<unsigned>(cElemBytes) << " bytes";
		}
		oss << "; STL: " << label << ')';
		return oss.str();
	}

	static std::string cHintPhrase(const std::string& hint)
	{
		if (hint == "vector_like_3ptr") return "vector-like container (3-pointer";
		if (hint == "list_like_dllist") return "list-like container (doubly-linked";
		if (hint == "map_like_rbtree") return "map-like container (red-black tree";
		if (hint == "set_like_rbtree") return "set-like container (red-black tree";
		if (hint == "unordered_map_like_hash") return "hash-map-like container (bucket chain";
		if (hint == "unordered_set_like_hash") return "hash-set-like container (bucket chain";
		if (hint == "string_like_sso") return "string-like object (SSO/heap";
		if (hint == "shared_ptr_like_2ptr") return "shared-ownership pointer (2-pointer";
		if (hint == "unique_ptr_like_1ptr") return "unique-ownership pointer (1-pointer";
		if (hint == "weak_ptr_like_2ptr") return "weak-ownership pointer (2-pointer";
		if (hint == "deque_like_chunked") return "deque-like container (chunked";
		if (hint == "array_like_fixed") return "fixed-size array-like object (inline";
		return "container-like structure (layout hint: " + hint;
	}
};

} // namespace common
} // namespace retdec

#endif
