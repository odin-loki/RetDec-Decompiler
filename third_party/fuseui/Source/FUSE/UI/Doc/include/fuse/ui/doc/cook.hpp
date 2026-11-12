// UI8 cookers: `.fuseui` documents and `.fusestyle` sheets to validated binary blobs, plus the widget-registration
// list a set of documents needs (emitted as C++ so a shipping build registers only those classes).
//
//   cooked document (.fuseuic): JSON tree in binary form + every binding compiled to bytecode (variables by name,
//                               resolved against the Model at load) + the document's style sheet cooked + the classes
//   cooked style    (.fusestylec): parsed rules (selectors, declarations, var() text) and :root tokens
//
// Loading a cooked blob does no JSON, CSS or expression parsing (Expr::compileCount() does not move). Every blob is
// bounds- and structure-checked on load (fuzzed by fuse_ui_markup_fuzz); a bad blob is an error, never a crash.
#pragma once

#include "fuse/ui/doc/document.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::doc {

inline constexpr u32 kCookedVersion = 1;

struct CookInfo {
    u32 nodes = 0, bindings = 0, instructions = 0;
    std::vector<std::string> classes; ///< widget classes the document creates (sorted, unique)
};

/// `.fuseui` text -> cooked document. Fails on JSON errors, bad bindings or a bad style sheet.
std::expected<std::vector<u8>, DocError> cookDocument(std::string_view json, CookInfo* info = nullptr);
/// `.fusestyle` (CSS text) -> cooked sheet. Diagnostics (skipped declarations) as for StyleSheet::parse.
std::expected<std::vector<u8>, std::string> cookStyle(std::string_view css, std::vector<StyleDiagnostic>* diags = nullptr);
std::expected<StyleSheet, std::string> loadCookedStyle(std::span<const u8> cooked);
/// Classes recorded in a cooked document.
std::expected<std::vector<std::string>, std::string> cookedClasses(std::span<const u8> cooked);
bool isCookedDocument(std::span<const u8> bytes);
bool isCookedStyle(std::span<const u8> bytes);

/// Widget classes a set of documents needs, with the classes those widgets create internally (combobox -> popup,
/// menu, menuitem; ...). Sorted, unique; core classes (panel, label) are included when used.
std::vector<std::string> registrationList(const std::vector<std::vector<std::string>>& perDocumentClasses);
/// C++ source for `void <fn>(fuse::ui::UiContext&)` that registers exactly `classes` (widgets::installClasses).
std::string registrationSource(const std::vector<std::string>& classes, std::string_view fn = "fuse_ui_register_cooked");

} // namespace fuse::ui::doc
