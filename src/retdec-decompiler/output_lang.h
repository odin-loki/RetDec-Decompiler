/**
 * @file src/retdec-decompiler/output_lang.h
 * @brief CLI / config output language parsing and native HLL routing.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#pragma once

#include "managed_decompiler.h"

#include <string>

namespace retdec {
namespace decompiler {

/** Canonical output language ids accepted by `--output-lang`. */
enum class OutputLangId {
	C,
	Cpp,
	Python,
	CSharp,
	Java,
	Wat,
	Unknown
};

/** Parse CLI/config token (e.g. c, cpp, csharp). Throws on invalid value. */
OutputLangId parseOutputLang(const std::string& token);

/** CLI spelling for @p id (e.g. "cpp"). Empty when Unknown. */
const char* outputLangCliName(OutputLangId id);

/** Default file extension including dot (e.g. ".c", ".cpp", ".py"). */
const char* outputLangFileExtension(OutputLangId id);

/** HLL writer factory id for the native LLVM pipeline (always "c" today). */
const char* nativeTargetHllId(OutputLangId id);

/** Default output language for a managed format (Unknown → C). */
OutputLangId defaultOutputLangForManaged(ManagedFormat fmt);

/** True when @p lang matches the managed emitter for @p fmt (or fmt is Unknown). */
bool isOutputLangCompatibleWithManaged(OutputLangId lang, ManagedFormat fmt);

/** Apply native TargetHLL from @p lang; logs when falling back to C. */
void applyNativeOutputLanguage(OutputLangId lang);

} // namespace decompiler
} // namespace retdec
