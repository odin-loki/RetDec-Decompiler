#pragma once
// forge::os::text: FORGE's own Unicode library (UTF, properties, case, normalization, segmentation,
// width, bidi, collation, transliteration, fuzzy matching, regex), legacy encodings and console text.
// Design: docs/text-locale.md.  Built as the forge_text library (os/src/text/). Unicode 16.0.0.
#include <forge/os/os.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::os::text {

using cp_t = char32_t;
inline constexpr cp_t replacement_char = 0xFFFD;
const char* unicode_version();

// ---------------------------------------------------------------- UTF
// Decodes one code point at s[i] and advances i. Invalid input yields U+FFFD and advances by the
// maximal subpart (WHATWG / Unicode 3.9 recommendation), so callers never loop forever.
cp_t decode_utf8(std::string_view s, std::size_t& i, bool* ok = nullptr);
void append_utf8(std::string& out, cp_t c);
void append_utf16(std::u16string& out, cp_t c);
std::string encode_utf8(std::u32string_view s);
std::u32string utf8_to_utf32(std::string_view s);              // lossy (U+FFFD)
result<std::u16string> utf8_to_utf16(std::string_view s);      // strict
result<std::string> utf16_to_utf8(std::u16string_view s);      // strict (unpaired surrogates fail)
std::string utf16_to_utf8_lossy(std::u16string_view s);
result<std::string> utf32_to_utf8(std::u32string_view s);
// Returns npos when valid, else the byte offset of the first invalid sequence.
std::size_t validate_utf8(std::string_view s);
inline bool is_valid_utf8(std::string_view s) { return validate_utf8(s) == std::string_view::npos; }
std::string sanitize_utf8(std::string_view s);
std::size_t count_code_points(std::string_view s);

// ---------------------------------------------------------------- properties
enum class category : std::uint8_t { Cn, Lu, Ll, Lt, Lm, Lo, Mn, Mc, Me, Nd, Nl, No, Pc, Pd, Ps, Pe, Pi, Pf, Po,
                                     Sm, Sc, Sk, So, Zs, Zl, Zp, Cc, Cf, Cs, Co };
category general_category(cp_t c);
const char* category_name(category g);            // "Lu"
std::string_view script(cp_t c);                  // "Latin", "Greek", "Common", ...
int combining_class(cp_t c);
std::string_view east_asian_width(cp_t c);        // N A F H Na W
std::string_view bidi_class_name(cp_t c);         // L R AL EN ...
std::string_view line_break_class(cp_t c);
bool is_letter(cp_t c);
bool is_mark(cp_t c);
bool is_number(cp_t c);
bool is_digit(cp_t c);                            // Nd
bool is_punct(cp_t c);
bool is_symbol(cp_t c);
bool is_alpha(cp_t c);                            // Alphabetic
bool is_upper(cp_t c);
bool is_lower(cp_t c);
bool is_space(cp_t c);                            // White_Space
bool is_cased(cp_t c);
bool is_case_ignorable(cp_t c);
bool is_emoji(cp_t c);
bool is_emoji_presentation(cp_t c);
bool is_extended_pictographic(cp_t c);
bool is_ideographic(cp_t c);
bool is_default_ignorable(cp_t c);
bool is_noncharacter(cp_t c);
bool is_word_char(cp_t c);                        // \w: alpha, mark, Nd, Pc, Join_Control
std::optional<int> digit_value(cp_t c);           // Nd -> 0..9
// Property lookup by name, for regex \p{...}: "L", "Lu", "Letter", "Script=Greek", "Greek", "Alphabetic"...
bool has_property(cp_t c, std::string_view name, bool* known = nullptr);

// ---------------------------------------------------------------- case
cp_t simple_upper(cp_t c);
cp_t simple_lower(cp_t c);
cp_t simple_title(cp_t c);
cp_t simple_fold(cp_t c);
// Full mappings (SpecialCasing, Final_Sigma). lang: BCP 47 language ("tr", "az", "lt") for tailoring.
std::string to_upper(std::string_view s, std::string_view lang = {});
std::string to_lower(std::string_view s, std::string_view lang = {});
std::string to_title(std::string_view s, std::string_view lang = {});   // word-boundary title casing
std::string case_fold(std::string_view s, bool turkic = false);
bool equals_ignore_case(std::string_view a, std::string_view b);

// ---------------------------------------------------------------- normalization (UAX 15)
enum class norm_form { nfc, nfd, nfkc, nfkd };
std::string normalize(std::string_view s, norm_form f = norm_form::nfc);
std::u32string normalize32(std::u32string_view s, norm_form f = norm_form::nfc);
bool is_normalized(std::string_view s, norm_form f = norm_form::nfc);

// ---------------------------------------------------------------- segmentation (UAX 29, UAX 14)
// Boundaries are byte offsets into the UTF-8 input, always including 0 and size().
std::vector<std::size_t> grapheme_breaks(std::string_view s);
std::vector<std::size_t> word_breaks(std::string_view s);
std::vector<std::size_t> sentence_breaks(std::string_view s);
struct line_break { std::size_t pos; bool mandatory; };
std::vector<line_break> line_breaks(std::string_view s);        // opportunities after position 0
std::vector<std::string_view> graphemes(std::string_view s);
std::vector<std::string_view> sentences(std::string_view s);
// Words: segments that contain a letter, number or ideograph (spaces and punctuation dropped).
std::vector<std::string_view> words(std::string_view s);
std::size_t next_grapheme(std::string_view s, std::size_t pos);  // byte offset of the next boundary
std::size_t prev_grapheme(std::string_view s, std::size_t pos);
std::size_t count_graphemes(std::string_view s);
// The same algorithms over code point arrays (used by the conformance tests).
std::vector<std::size_t> grapheme_breaks32(std::u32string_view s);
std::vector<std::size_t> word_breaks32(std::u32string_view s);
std::vector<std::size_t> sentence_breaks32(std::u32string_view s);
std::vector<line_break> line_breaks32(std::u32string_view s);

// ---------------------------------------------------------------- display width
struct width_options { bool ambiguous_wide = false; };      // East Asian Ambiguous = 2 (CJK terminals)
int width(cp_t c, width_options o = {});                    // -1 for C0/C1 controls
int display_width(std::string_view s, width_options o = {}); // per grapheme; controls count 0

// ---------------------------------------------------------------- bidi (UAX 9)
enum class direction { ltr, rtl, auto_ };
struct bidi_result {
    std::vector<std::uint8_t> levels;   // one per code point; 0xFF = removed by X9 (BN, embeddings)
    int paragraph_level = 0;
};
bidi_result bidi_levels32(std::u32string_view s, direction base = direction::auto_);
// Visual order for one line: indices (into the code point array) in display order. L1 applied.
std::vector<std::size_t> bidi_reorder32(std::u32string_view s, const bidi_result& r);
std::string bidi_visual(std::string_view s, direction base = direction::auto_);   // reordered, mirrored
direction base_direction(std::string_view s);              // first strong character (P2), ltr if none
cp_t mirror(cp_t c);

// ---------------------------------------------------------------- collation (UTS 10, DUCET)
enum class strength { primary = 1, secondary, tertiary, quaternary, identical };
struct collation_options {
    strength level = strength::tertiary;
    bool shifted = false;          // alternate=shifted (ignore punctuation/space at L1-L3)
    bool numeric = false;          // "file9" < "file10"
    bool upper_first = false;      // case-first upper
    bool backwards_secondary = false;   // French (Canada) accent ordering
};
class collator {
public:
    // locale: "" / "root" = DUCET; tailorings for sv, fi, da, nb, nn, no, de-phonebook (de-u-co-phonebk),
    // es, tr, az, pl, cs, sk, hr, et, lt, lv, hu, is, ro, cy; fr-CA uses backwards secondary.
    explicit collator(std::string_view locale = {}, collation_options o = {});
    ~collator();
    collator(collator&&) noexcept;
    collator& operator=(collator&&) noexcept;
    int compare(std::string_view a, std::string_view b) const;     // <0, 0, >0
    std::string sort_key(std::string_view s) const;                // memcmp-comparable bytes
    std::string sort_key32(std::u32string_view s) const;
    bool operator()(std::string_view a, std::string_view b) const { return compare(a, b) < 0; }
    const collation_options& options() const;
private:
    struct impl; std::unique_ptr<impl> p_;
};
void sort(std::vector<std::string>& v, std::string_view locale = {}, collation_options o = {});

// ---------------------------------------------------------------- transliteration
std::string to_ascii(std::string_view s, char unknown = '?');     // "Ærøskøbing Straße" -> "AEroskobing Strasse"
std::string remove_marks(std::string_view s);                     // NFD minus Mn, recomposed
// lower-case ASCII slug: "Héllo, Wörld!" -> "hello-world"
std::string slugify(std::string_view s, char sep = '-', std::size_t max_len = 0);

// ---------------------------------------------------------------- fuzzy matching
// All metrics run over grapheme clusters, so "é" (e + U+0301) is one unit.
std::size_t levenshtein(std::string_view a, std::string_view b);
std::size_t damerau_levenshtein(std::string_view a, std::string_view b);   // optimal string alignment
double similarity(std::string_view a, std::string_view b);                  // 1 - lev / max(len)
double jaro_winkler(std::string_view a, std::string_view b);
// Editor-style subsequence match ("fzf"): case-insensitive, bonuses for word starts/consecutive runs.
// Returns nullopt if `pattern` is not a subsequence of `candidate`. Optional matched byte positions.
std::optional<int> fuzzy_score(std::string_view pattern, std::string_view candidate, std::vector<std::size_t>* positions = nullptr);
struct fuzzy_hit { std::size_t index; int score; };
std::vector<fuzzy_hit> fuzzy_filter(std::string_view pattern, std::span<const std::string> items, std::size_t limit = 0);

// ---------------------------------------------------------------- regex
// Backtracking regex over code points with Unicode classes. Syntax: . [] [^] ranges, \p{..} \P{..},
// \d \w \s \D \W \S \b \B \X (grapheme) ^ $ \A \z, (...) (?:...) (?<name>...) (?=..) (?!..), | * + ? {m,n}
// lazy variants, backrefs \1, escapes \n \t \x{..} \u....  Flags: i (case fold), m (multiline), s (dotall), x.
struct regex_match {
    std::vector<std::pair<std::size_t, std::size_t>> groups;  // byte [begin, end); npos when unset; [0] = whole
    std::vector<std::string> names;                          // group names ("" for unnamed), same size
    std::string_view str(std::string_view subject, std::size_t g = 0) const;
    std::size_t begin() const { return groups.empty() ? std::string_view::npos : groups[0].first; }
    std::size_t end() const { return groups.empty() ? std::string_view::npos : groups[0].second; }
    std::optional<std::size_t> group_index(std::string_view name) const;
};
class regex {
public:
    static result<regex> compile(std::string_view pattern, std::string_view flags = {});
    regex(); ~regex(); regex(regex&&) noexcept; regex& operator=(regex&&) noexcept;
    regex(const regex&); regex& operator=(const regex&);
    std::optional<regex_match> match(std::string_view s) const;                 // whole string
    std::optional<regex_match> search(std::string_view s, std::size_t from = 0) const;
    std::vector<regex_match> find_all(std::string_view s) const;
    // Replacement: $0..$9, ${name}, $$.
    std::string replace(std::string_view s, std::string_view repl, bool all = true) const;
    std::vector<std::string> split(std::string_view s) const;
    std::size_t group_count() const;
    // Backtracking budget per search (default 1e7 steps); exceeding it fails the match and sets this flag.
    void set_step_limit(std::uint64_t n);
    bool step_limit_hit() const;
    struct impl;   // engine state (opaque)
private:
    std::shared_ptr<impl> p_;
};

// ---------------------------------------------------------------- legacy encodings
enum class on_error { fail, replace, skip };
// Canonical names (WHATWG labels accepted case-insensitively: "latin1", "cp1252", "sjis", "gb2312"...).
std::vector<std::string> charsets();
std::optional<std::string> canonical_charset(std::string_view label);
result<std::string> decode(std::string_view bytes, std::string_view charset, on_error e = on_error::replace);
result<std::string> encode(std::string_view utf8, std::string_view charset, on_error e = on_error::fail);
struct bom_info { std::string charset; std::size_t length = 0; };   // empty charset if none
bom_info detect_bom(std::string_view bytes);
struct charset_guess { std::string charset; double confidence = 0; };
// BOM, then strict UTF-8, then UTF-16 without BOM (NUL pattern), then byte statistics over
// Shift_JIS / EUC-JP / GBK / Big5 / EUC-KR / windows-125x / KOI8-R. hint_lang biases the guess.
charset_guess sniff_charset(std::string_view bytes, std::string_view hint_lang = {});
// Decode with a sniffed charset: never fails (falls back to windows-1252).
std::string to_utf8_auto(std::string_view bytes, std::string* used_charset = nullptr);

// ---------------------------------------------------------------- console text
struct term_size { int cols = 80, rows = 24; };
term_size terminal_size(int fd = 1);       // ioctl / GetConsoleScreenBufferInfo, $COLUMNS/$LINES, 80x24
bool is_terminal(int fd = 1);
status enable_vt(int fd = 1);              // Windows: ENABLE_VIRTUAL_TERMINAL_PROCESSING; no-op elsewhere
enum class color_level { none, basic, ansi256, truecolor };
color_level color_support(int fd = 1);     // NO_COLOR, FORCE_COLOR, CLICOLOR_FORCE, COLORTERM, TERM, isatty
struct rgb { std::uint8_t r = 0, g = 0, b = 0; };
struct text_style {
    std::optional<rgb> fg, bg;
    int fg16 = -1, bg16 = -1;              // 0..15 basic palette
    bool bold = false, dim = false, italic = false, underline = false, blink = false, reverse = false, strike = false;
    std::string link;                      // OSC 8 hyperlink
};
// Wraps s in SGR codes downgraded to `level` (truecolor -> 256 -> 16 -> none).
std::string style(std::string_view s, const text_style& st, color_level level = color_level::truecolor);
// Named colours ("red", "bright_blue", "#ff8800", "208") for scripts.
std::optional<text_style> parse_style(std::string_view spec);   // "bold red on #202020 underline"
std::string strip_ansi(std::string_view s);
int display_width_ansi(std::string_view s, width_options o = {});
struct wrap_options { int width = 80; std::string indent, subsequent_indent; bool break_long_words = true; width_options w{}; };
std::vector<std::string> wrap_lines(std::string_view s, const wrap_options& o);
std::string wrap(std::string_view s, const wrap_options& o);
std::string truncate(std::string_view s, int max_width, std::string_view ellipsis = "\u2026", width_options o = {});
enum class align { left, right, center };
std::string pad(std::string_view s, int w, align a = align::left, char32_t fill = U' ', width_options o = {});

}  // namespace forge::os::text
