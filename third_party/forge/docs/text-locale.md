# text and locale: Unicode, encodings and localisation

## Concepts
FORGE has its own clean-room Unicode, encoding and locale library instead of ICU.

- **Headers:** `os/include/forge/os/text.hpp` (`forge::os::text`) and `os/include/forge/os/locale.hpp` (`forge::os::locale`). Sources are in `os/src/text/`. Link the static library `forge_text`.
- **Data:** Unicode 16.0.0 (UCD and DUCET) and CLDR 46. No data files at run time: the tables are compiled in.
- **Errors:** fallible calls return `os::result<T>` / `os::status`. Nothing throws except `forge::mem` running out of budget.
- **Memory:** collators and the reverse tables of the legacy encoders take a `forge::mem` lease on the host budget (tag `"text"`), so they count against `FORGE_MEM_LIMIT` ([memory.md](memory.md)).
- **Scripts:** the `forge::text::` and `forge::locale::` built-ins ([Script built-ins](#script-built-ins)).

## Data
| Data | Source | Used for |
|---|---|---|
| UnicodeData, DerivedGeneralCategory, Scripts, PropList, DerivedCoreProperties, emoji-data | UCD 16.0.0 | properties, case, decomposition |
| SpecialCasing, CaseFolding | UCD | full case mapping and folding |
| DerivedNormalizationProps | UCD | composition exclusions |
| Grapheme/Word/SentenceBreakProperty, LineBreak, EastAsianWidth | UCD | segmentation, width |
| DerivedBidiClass, BidiBrackets, BidiMirroring | UCD | UAX 9 |
| allkeys.txt | DUCET 16.0.0 | collation |
| numbers, currencies, gregorian calendar, date fields, list patterns, units, plurals, ordinals, week and measurement data | CLDR 46 (cldr-json) | formatting |
| WHATWG Encoding Standard indexes, Unicode CP437/CP850 mappings | WHATWG, Unicode | legacy encodings |

Generators: `tools/unicode/gen_ucd.py` → `ucd_data.inc` (~1.7 MB), `tools/cldr/gen_cldr.py` (with `tools/cldr/fetch.sh`) → `cldr_data.inc` (~2.3 MB), `tools/unicode/gen_encodings.py` → `encoding_data.inc`. The generated files are committed, so offline and cross builds work. Properties are a two-stage trie (code point >> 7 selects a 128-entry block; identical blocks are shared); mappings are sorted arrays searched by binary search.

## API reference: `forge::os::text`
| Area | API |
|---|---|
| UTF | `decode_utf8(s, i)` (invalid input → U+FFFD, advancing by the maximal subpart), `append_utf8`, `append_utf16`, `encode_utf8`, `utf8_to_utf32` (lossy), `utf8_to_utf16` / `utf16_to_utf8` / `utf32_to_utf8` (strict), `utf16_to_utf8_lossy`, `validate_utf8` (offset of the first bad byte, or npos), `is_valid_utf8`, `sanitize_utf8`, `count_code_points` |
| Properties | `general_category`, `category_name`, `script`, `combining_class`, `east_asian_width`, `bidi_class_name`, `line_break_class`, `is_letter/mark/number/digit/punct/symbol/alpha/upper/lower/space/cased/emoji/...`, `is_word_char`, `digit_value`, `has_property(c, "Script=Greek")` |
| Case | `to_upper`, `to_lower`, `to_title` (full mappings, Final_Sigma, `lang` tailoring for tr, az, lt), `case_fold(s, turkic)`, `simple_upper/lower/title/fold`, `equals_ignore_case` |
| Normalization (UAX 15) | `normalize(s, nfc/nfd/nfkc/nfkd)`, `normalize32`, `is_normalized` |
| Segmentation (UAX 29, UAX 14) | `grapheme_breaks`, `word_breaks`, `sentence_breaks` (byte offsets incl. 0 and size), `line_breaks` (with mandatory flag), `graphemes`, `words` (segments with a letter, number or ideograph), `sentences`, `next_grapheme`, `prev_grapheme`, `count_graphemes`, `*_breaks32` over code points |
| Width | `width(cp)` (-1 for controls), `display_width(s)` per grapheme (wide/fullwidth and emoji presentation = 2, marks = 0); `width_options.ambiguous_wide` |
| Bidi (UAX 9) | `bidi_levels32(s, base)`, `bidi_reorder32` (visual order for one line, L1 applied), `bidi_visual(s)` (reordered and mirrored), `base_direction`, `mirror` |
| Collation (UTS 10) | `collator(locale, collation_options{level, shifted, numeric, upper_first, backwards_secondary})`, `compare`, `sort_key` (memcmp-comparable), `sort(v, locale)`. Tailorings: sv, fi, da, nb, nn, no, de phonebook (`de-u-co-phonebk`), es, tr, az, pl, cs, sk, hr, et, lt, lv, hu, is, ro, cy; fr-CA uses backwards secondary |
| Transliteration | `to_ascii` ("Ærøskøbing Straße" → "AEroskobing Strasse"), `remove_marks`, `slugify(s, sep, max_len)` |
| Fuzzy | `levenshtein`, `damerau_levenshtein` (optimal string alignment), `similarity`, `jaro_winkler`, `fuzzy_score` (editor-style subsequence match with positions), `fuzzy_filter`; all over graphemes |
| Regex | `regex::compile(pattern, flags)`; `match`, `search`, `find_all`, `replace` (`$0..$9`, `${name}`, `$$`), `split`. Syntax: classes, `\p{..}` / `\P{..}`, `\d \w \s \b \B \X` with Unicode semantics, `^ $ \A \z`, groups, named groups, lookahead, backrefs, greedy and lazy quantifiers; flags `i m s x`. Backtracking is capped per search (default 1e7 steps; `set_step_limit`, `step_limit_hit`) |
| Encodings | `decode(bytes, charset, on_error)`, `encode(utf8, charset, on_error)` with `fail`, `replace` or `skip`; `charsets()`, `canonical_charset` (WHATWG labels), `detect_bom`, `sniff_charset` (BOM, strict UTF-8, BOM-less UTF-16, then byte statistics), `to_utf8_auto` |
| Console | `terminal_size`, `is_terminal` (Windows: consoles and mintty's pty pipes), `enable_vt` (Windows: VT processing, and the console output code page set to UTF-8 once and restored at exit — the code page belongs to the whole console, parent shell included), `color_support` (`NO_COLOR`, `FORCE_COLOR`, `CLICOLOR_FORCE`, `COLORTERM`, `TERM`, isatty; on Windows with no `TERM`: truecolor in Windows Terminal, mintty or a console with VT processing on, otherwise none), `style` (16/256/truecolor, bold, italic, underline, OSC 8 links with C0/C1 controls removed from the URI, downgraded to the level; `color_level::none` emits no escapes at all, links included), `parse_style("bold red on #202020")`, `strip_ansi`, `display_width_ansi`, `wrap`, `wrap_lines`, `truncate` (ellipsis at a grapheme boundary), `pad(s, w, align)` |

Charsets: UTF-8, UTF-16LE/BE, UTF-32LE/BE, US-ASCII, ISO-8859-1 to -8, -10 and -13 to -16 (-11 is an alias of windows-874 and -9 / Latin-5 of windows-1254, as in WHATWG), windows-874 and 1250 to 1258, KOI8-R, KOI8-U, IBM866, IBM437, IBM850, macintosh, Shift_JIS, EUC-JP, ISO-2022-JP, GBK (GB2312), GB18030, Big5, EUC-KR.

## API reference: `forge::os::locale`
| Area | API |
|---|---|
| Detection | `system_locale()`, `system_locale(category)`, `preferred_languages()`, `current()` / `current(category)` / `set_current()`, `android_set_locale(locale, preferred)`. `category::messages`, `time`, `numeric`: number, unit and currency formatting with the default locale use `numeric`, dates and week data use `time` |
| Tags (BCP 47) | `parse_tag` (accepts POSIX `en_AU.UTF-8@euro`, Windows and Apple forms), `canonicalize`, `maximize` / `minimize` (likely subtags), `fallback_chain` (`zh-Hant-TW` → `zh-Hant` → `zh` → root), `best_bundled`, `bundled_locales`, `negotiate`, `display_name` |
| Region data | `first_day_of_week`, `min_days_in_first_week`, `weekend`, `measurement_system`, `paper_size`, `preferred_hour_cycle`, `currency_for_region` |
| Plurals | `plural_category(number, locale, ordinal)` (CLDR rules evaluated at run time, so every CLDR language is covered; `"1.0"` keeps its visible fraction digits), `plural_categories` |
| Numbers | `format_number`, `format_integer`, `format_percent`, `format_permille`, `format_scientific`, `format_compact` (1.2K / 1.2 thousand), `format_currency` (symbol, narrow, code or name; accounting), `format_currency_compact`, `format_bytes`, `parse_number`, `parse_currency`, `currency_symbol`, `currency_name`, `currency_digits`, `number_symbol`, `localize_digits`. `number_options`: digits, significant digits, grouping, sign display, rounding mode, numbering system (`latn`, `native`, `arab`, `deva`, `thai`, ...) |
| Lists and units | `format_list` (and / or / unit; wide / short / narrow), `format_unit(v, "kilometer-per-hour", locale, width)`, `unit_display_name`, `convert_unit` (length, mass, temperature, volume, speed, area, duration, digital, pressure, energy) |
| Time zones | `local_zone`, `zone_names`, `zone_at(zone, t)` (offset, DST, abbreviation), `from_local`, `tz_backend()` |
| Dates and times | `datetime`, `from_time`, `now`, `format_date` / `format_time` / `format_datetime` (full, long, medium, short), `format_pattern` (LDML), `format_skeleton`, `pattern_for_skeleton`, `parse_pattern`, `parse_iso8601`, `format_iso8601`, `month_name`, `weekday_name`, `day_period`, `format_relative` ("in 3 days", "yesterday"), `format_relative_time`, `format_duration` |
| Calendars | Gregorian, ISO 8601 weeks (`to_iso_week`, `week_of_year`), Buddhist, ROC, Japanese eras, Islamic civil and tabular, Hebrew, Persian: `from_gregorian`, `to_gregorian`, `months_in_year`, `days_in_month`, `calendar_month_name`, `era_name` |
| Messages | `format_message(pattern, args, locale)`: `{name}`, `{n, number[, integer|percent|currency/CODE|compact]}`, `{d, date|time, style}`, `{n, plural, offset:k =0 {..} one {# item} other {..}}`, `{n, selectordinal, ..}`, `{g, select, ..}`, nesting, apostrophe quoting |
| Catalogs | `catalog::load` (`.mo` either endianness, or `.po`), `parse_mo`, `parse_po`, `find(dir, domain, locale)` (`<dir>/<locale>/LC_MESSAGES/<domain>.mo` along the fallback chain), `gettext`, `ngettext`, `pgettext`, `npgettext`, `lookup`, `header`, `merge`; `eval_plural_expr` for `Plural-Forms` |

### Locale detection per platform
| Platform | `system_locale()` | `preferred_languages()` |
|---|---|---|
| Linux, FreeBSD | `LC_ALL`, `LC_MESSAGES` (`LC_TIME` / `LC_NUMERIC` for those categories), `LANG`, then the current `setlocale` locale of the category | `LANGUAGE`, then the system locale |
| macOS, iOS | the same variables, then `CFLocaleCopyCurrent` (`locale_apple.mm`) with the user's Language & Region changes | `LANGUAGE`, then `CFLocaleCopyPreferredLanguages` |
| Android | a value pushed by `android_set_locale`, the variables, then `persist.sys.locale` / `ro.product.locale` | pushed list, `LANGUAGE`, system locale |
| Windows | `GetUserDefaultLocaleName` with the user's Region changes | `GetUserPreferredUILanguages` |

`"C"`, `"POSIX"` and `"C.UTF-8"` are skipped; the fallback is `"en"`. When the locale comes from the OS rather than the environment, settings the user changed away from the locale's own data (Windows `LOCALE_NOUSEROVERRIDE` comparison, Apple: against a plain `CFLocale` of the same identifier) are honoured: a 24-hour clock and the first weekday become `-u-hc-*` / `-u-fw-*` keywords of `system_locale(category::time)`, and the decimal and grouping separators are used by number formatting with the default locale (not after `set_current`).

### Time zone backends
| Build | `tz_backend()` |
|---|---|
| MSVC | `chrono-tzdb` (`std::chrono::tzdb`) |
| Others | `tzif:<dir>`: TZif v1–v4 files plus the POSIX TZ footer, from `$TZDIR`, `/usr/share/zoneinfo`, `/usr/lib/zoneinfo`, `/usr/share/lib/zoneinfo`, `/var/db/timezone/zoneinfo` or `/etc/zoneinfo` |
| Android | `tzdata:<file>`: the single-file bundle (`/apex/com.android.tzdata/etc/tz/tzdata`, `/data/misc/zoneinfo/current/tzdata`, `/system/usr/share/zoneinfo/tzdata`; also `$TZDIR/tzdata` anywhere), indexed and read zone by zone |
| MinGW without zoneinfo | `windows-registry`: `local_zone()` maps the Windows zone (`GetDynamicTimeZoneInformation`) to IANA with a CLDR windowsZones table (an unmapped key is returned as is), and `zone_at` evaluates the current rules from the registry `TZI` value (no historical transitions; abbreviations are numeric, "+11") |
| Nothing found | `utc-only`; bare POSIX TZ strings and `Etc/GMT±N` still work |

Zone names must be IANA-style (letters, digits, `_ + - /`, no empty, `.` or `..` component); zone files are read only if they are regular files of at most 1 MiB, opened non-blocking. An absolute path is accepted only when it is what `$TZ` names (`TZ=:/path/to/zone`). Parsed zones are cached (64 entries; failures are not cached).

## Bundled locales
CLDR formatting data for 52 locales plus root: en, en-GB, en-AU, en-IN, en-CA, de, de-CH, fr, fr-CA, es, es-MX, it, pt, pt-PT, nl, sv, da, nb, fi, pl, cs, ru, uk, tr, el, he, ar, fa, hi, bn, th, vi, id, ja, ko, zh, zh-Hant, ro, hu, sk, bg, hr, ga, cy, ms, sw, et, lt, lv, sl, sr, ca. Other locales use `best_bundled` (for example en-NZ → en-GB, es-AR → es-MX), then their language, then root. Plural rules cover every CLDR language.

## Script built-ins
`script/src/text.cpp` registers 79 built-ins (see `examples/scripts/text_locale.cpp`):

- `forge::text::` upper, lower, title, fold, equals_ignore_case, normalize, is_normalized, sanitize, length, code_points, width, graphemes, words, sentences, wrap, truncate, pad, strip_ansi, style, slugify, to_ascii, remove_marks, levenshtein, similarity, jaro_winkler, fuzzy_filter, compare, sort, sort_key, regex (a handle), decode, encode, detect, charsets, bidi_visual, direction, has_property, props.
- `forge::locale::` current, set_current, system, preferred, bundled, canonicalize, maximize, minimize, negotiate, display_name, plural, number, percent, compact, currency, currency_for_region, bytes, parse_number, list, unit, convert, date, time, datetime, pattern, skeleton, parse_date, to_date, iso, now, relative, ago, duration, zone, zones, local_zone, calendar, message, first_day, hour_cycle, catalog (a handle; loading needs the `fs` capability).

## Tests
| Test | Covers |
|---|---|
| `test_text` | UTF, properties, case, width, transliteration, fuzzy, regex, encodings, console helpers |
| `test_locale` | formatting, parsing, plurals, messages, catalogs, calendars, detection |
| `test_text_conformance` | NormalizationTest, Grapheme/Word/Sentence/LineBreakTest, BidiCharacterTest, CollationTest (shifted and non-ignorable). Every suite must pass 100%. Needs the files in `$FORGE_UCD_DIR` or `tests/data/ucd` (`cmake -DFORGE_FETCH_UCD=ON` downloads them); exits 77 (skipped) without them |
| `xos_text_<target>` | compiles the C++ sources for FreeBSD, macOS, MinGW and aarch64 musl with zig |

`test_text` and `test_locale` pass in CI on Linux, macOS (which also compiles `locale_apple.mm`), Windows (MSVC and MinGW) and FreeBSD. `test_text_conformance` is skipped in CI because the data is not fetched there; it passed 100% locally on Linux. The Android CI job runs `test_text` and `test_locale` on an x86_64 API 34 emulator (time zones from the tzdata bundle); the iOS job compiles `forge_text` (including `locale_apple.mm`) but runs nothing.

## Limits
- Not ICU: no full CLDR (only the 52 locales above, and not every field), no IDNA/UTS 46, no spoof checking, no spellout, no Chinese or Indian lunisolar calendars.
- No dictionary-based line or word breaking for Thai, Lao, Khmer or Burmese; they break only where UAX 14 allows (spaces).
- No ja/zh/ko collation reordering; tailorings are a simple rule list.
- Non-Gregorian month names are English transliterations ("Ramadan", "Nisan").
- Regex possessive quantifiers are treated as greedy.
- Which `ar` regions use Arabic-Indic digits is hard-coded (EG, SD, SY, IQ, JO, KW).
- `next_grapheme` segments from `pos` to the end of the string, so it is O(n) per call.
