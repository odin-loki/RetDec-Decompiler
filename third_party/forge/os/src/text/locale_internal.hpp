// Internal helpers shared by the forge::os::locale sources.
#pragma once
#include <forge/os/locale.hpp>
#include <forge/os/text.hpp>
#include <string>
#include <string_view>

namespace forge::os::locale::detail {
// Resolves "" to current() and any tag to a bundled locale ("en-NZ" -> "en-GB").
std::string resolve(std::string_view locale, category c = category::messages);
// CLDR value for key along the bundled fallback chain (locale, its language, en); supplemental keys live in root.
std::string_view get(std::string_view bundled, std::string_view key);
std::optional<std::string_view> find(std::string_view bundled, std::string_view key);
std::vector<std::string> cldr_keys_with_prefix(std::string_view prefix);   // every bundled key, sorted
std::optional<std::string_view> supp(std::string_view key);
std::string region_of(std::string_view locale);             // explicit or likely region ("de" -> "DE")
std::string replace_all(std::string s, std::string_view from, std::string_view to);
std::string sub(std::string_view pattern, std::string_view a, std::string_view b = {});   // {0} {1}
// Number core shared by numbers/units/messages.
struct pattern_info { std::string prefix, suffix, neg_prefix, neg_suffix; int min_int = 1, min_frac = 0, max_frac = 3, group1 = 0, group2 = 0; bool has_neg = false; };
pattern_info parse_number_pattern(std::string_view p);
std::string format_with_pattern(double v, const pattern_info& pi, std::string_view bundled, const number_options& o, std::string_view numbering, bool user_syms = false);
std::string numbering_for(std::string_view bundled, std::string_view requested, std::string_view locale_tag);
// OS user settings that override the locale's CLDR defaults (empty = not customised / not available).
struct os_overrides { std::string fw, hc, decimal, group; };   // fw "mon".."sun", hc "h11".."h24"
const os_overrides& user_overrides();
// The user's decimal/group separator when formatting with the default locale (no set_current / android_set_locale).
std::optional<std::string> user_symbol(std::string_view name);
#if defined(__APPLE__)
void apple_user_overrides(std::string& first_weekday, std::string& time_format, std::string& decimal, std::string& group);
std::string apple_system_locale();
std::vector<std::string> apple_preferred_languages();
std::string apple_local_zone();
#endif
}  // namespace forge::os::locale::detail
