#!/usr/bin/env python3
"""Generate os/src/text/cldr_data.inc from cldr-json 46 (a subset of fields for the bundled locales).

Usage: gen_cldr.py <cldr_dir> <out.inc>
cldr_dir holds <locale>/{numbers,currencies,ca-gregorian,dateFields,listPatterns,units,languages}.json and
supp/{plurals,ordinals,weekData,measurementData,currencyData,likelySubtags}.json (see tools/cldr/fetch.sh).

Layout: one deduplicated string pool; a sorted global key list; per locale a sorted array of (key, value) index
pairs. Regional locales (en-AU) only store values that differ from their language (en); lookup walks the chain.
"""
import json, os, sys

LOCALES = "en en-GB en-AU en-IN en-CA de de-CH fr fr-CA es es-MX it pt pt-PT nl sv da nb fi pl cs ru uk tr el he ar fa hi bn th vi id ja ko zh zh-Hant ro hu sk bg hr ga cy ms sw et lt lv sl sr ca".split()
CURRENCIES = "USD EUR GBP JPY CNY AUD CAD CHF HKD NZD SEK NOK DKK PLN CZK HUF RON BGN HRK RUB UAH TRY ILS AED SAR EGP INR PKR BDT IDR MYR SGD THB VND PHP KRW TWD BRL MXN ARS CLP COP PEN ZAR NGN KES IRR ISK BTC".split()
UNITS = set("""acceleration-g-force angle-degree angle-radian area-square-kilometer area-hectare area-square-meter area-square-centimeter area-square-mile area-acre area-square-foot area-square-inch
concentr-percent concentr-permille consumption-liter-per-100-kilometer consumption-mile-per-gallon digital-petabyte digital-terabyte digital-gigabyte digital-megabyte digital-kilobyte digital-byte digital-bit
digital-gigabit digital-megabit digital-kilobit duration-century duration-decade duration-year duration-quarter duration-month duration-week duration-day duration-hour duration-minute duration-second
duration-millisecond duration-microsecond duration-nanosecond electric-ampere electric-volt electric-ohm energy-kilocalorie energy-calorie energy-kilojoule energy-joule energy-kilowatt-hour
frequency-gigahertz frequency-megahertz frequency-kilohertz frequency-hertz graphics-pixel graphics-megapixel length-kilometer length-meter length-decimeter length-centimeter length-millimeter
length-micrometer length-nanometer length-mile length-yard length-foot length-inch length-nautical-mile length-light-year mass-tonne mass-kilogram mass-gram mass-milligram mass-ton mass-stone
mass-pound mass-ounce power-kilowatt power-watt power-horsepower pressure-hectopascal pressure-bar pressure-millibar pressure-pound-force-per-square-inch pressure-atmosphere pressure-pascal
speed-kilometer-per-hour speed-meter-per-second speed-mile-per-hour speed-knot temperature-celsius temperature-fahrenheit temperature-kelvin temperature-generic volume-liter volume-milliliter
volume-cubic-meter volume-cubic-centimeter volume-gallon volume-gallon-imperial volume-quart volume-pint volume-cup volume-fluid-ounce volume-tablespoon volume-teaspoon""".split())
FIELDS = ["year", "quarter", "month", "week", "day", "hour", "minute", "second"]

def flat(x, p, out):
    if isinstance(x, dict):
        for k, v in x.items(): flat(v, p + "/" + k if p else k, out)
    elif isinstance(x, str): out[p] = x

def load(cdir, loc):
    out = {}
    def j(name): return json.load(open(os.path.join(cdir, loc, name + ".json"), encoding="utf-8"))["main"][loc]
    n = j("numbers")["numbers"]; ns = n["defaultNumberingSystem"]
    out["num/system"] = ns; out["num/native"] = n.get("otherNumberingSystems", {}).get("native", ns)
    out["num/mingroup"] = n.get("minimumGroupingDigits", "1")
    for sysname, prefix in ((ns, ""), ("latn", "latn/")):
        if ns == "latn" and prefix: continue
        for k in ("decimal", "group", "percentSign", "plusSign", "minusSign", "exponential", "perMille", "infinity", "nan", "approximatelySign"):
            v = n.get("symbols-numberSystem-" + sysname, {}).get(k)
            if v is not None: out[prefix + "sym/" + k] = v
    d = n.get("decimalFormats-numberSystem-" + ns, n.get("decimalFormats-numberSystem-latn"))
    out["fmt/decimal"] = d["standard"]
    for width in ("short", "long"):
        for k, v in d[width]["decimalFormat"].items(): out["fmt/compact-%s/%s" % (width, k)] = v
    out["fmt/percent"] = n.get("percentFormats-numberSystem-" + ns, n["percentFormats-numberSystem-latn"])["standard"]
    out["fmt/scientific"] = n.get("scientificFormats-numberSystem-" + ns, n["scientificFormats-numberSystem-latn"])["standard"]
    c = n.get("currencyFormats-numberSystem-" + ns, n["currencyFormats-numberSystem-latn"])
    out["fmt/currency"] = c["standard"]; out["fmt/accounting"] = c["accounting"]
    if "standard-alphaNextToNumber" in c: out["fmt/currency-alpha"] = c["standard-alphaNextToNumber"]
    for k, v in c.items():
        if k.startswith("unitPattern-count-"): out["fmt/currency-unit/" + k[18:]] = v
    for k, v in c.get("short", {}).get("standard", {}).items():
        if "alt" not in k: out["fmt/currency-compact/" + k] = v
    out["fmt/currency-space"] = c.get("currencySpacing", {}).get("beforeCurrency", {}).get("insertBetween", "\u00a0")
    for k, v in n.get("miscPatterns-numberSystem-" + ns, n.get("miscPatterns-numberSystem-latn", {})).items(): out["fmt/misc/" + k] = v
    cur = j("currencies")["numbers"]["currencies"]
    for code in CURRENCIES:
        for k, v in cur.get(code, {}).items():
            if k in ("symbol", "symbol-alt-narrow", "displayName") or k.startswith("displayName-count-"): out["cur/%s/%s" % (code, k)] = v
    g = j("ca-gregorian")["dates"]["calendars"]["gregorian"]
    for sect in ("months", "days", "quarters"):
        for ctx, widths in g[sect].items():
            for w, vals in widths.items():
                for k, v in vals.items(): out["cal/%s/%s/%s/%s" % (sect, ctx, w, k)] = v
    for w, vals in g["dayPeriods"]["format"].items():
        for k in ("am", "pm", "midnight", "noon", "morning1", "afternoon1", "evening1", "night1"):
            if k in vals: out["cal/dayPeriods/%s/%s" % (w, k)] = vals[k]
    for w, vals in g["eras"].items():
        for k, v in vals.items():
            if "alt" not in k: out["cal/eras/%s/%s" % (w, k)] = v
    for k, v in g["dateFormats"].items():
        if "alt" not in k: out["cal/date/" + k] = v
    for k, v in g["timeFormats"].items():
        if "alt" not in k: out["cal/time/" + k] = v
    dtf = g["dateTimeFormats"]
    for k in ("full", "long", "medium", "short"):
        v = dtf[k]
        out["cal/datetime/" + k] = v if isinstance(v, str) else v.get("standard", next(iter(v.values())))
    for k, v in dtf.get("availableFormats", {}).items():
        if "alt" not in k and "count" not in k: out["cal/skeleton/" + k] = v
    fields = j("dateFields")["dates"]["fields"]
    for f in FIELDS:
        for suf in ("", "-short", "-narrow"):
            e = fields.get(f + suf)
            if not e: continue
            flat_e = {}; flat(e, "", flat_e)
            for k, v in flat_e.items():
                k = k.replace("relativeTime-type-", "").replace("relativeTimePattern-count-", "").replace("relative-type-", "rel/")
                out["rel/%s%s/%s" % (f, suf, k)] = v
    for k, v in j("listPatterns")["listPatterns"].items():
        for part, pat in v.items(): out["list/%s/%s" % (k.replace("listPattern-type-", ""), part)] = pat
    u = j("units")["units"]
    for w in ("long", "short", "narrow"):
        for name, e in u.get(w, {}).items():
            if name == "per": out["unit/%s/per" % w] = e["compoundUnitPattern"]; continue
            if name not in UNITS: continue
            for k, v in e.items():
                if k in ("displayName", "perUnitPattern") or (k.startswith("unitPattern-count-") and "case" not in k and "gender" not in k):
                    out["unit/%s/%s/%s" % (w, name.split("-", 1)[1], k.replace("unitPattern-count-", ""))] = v
    langs = j("languages")["localeDisplayNames"]["languages"]
    for code in sorted({l.split("-")[0] for l in LOCALES} | {"zh-Hant", "en-GB", "en-US", "pt-PT", "es-MX", "fr-CA", "de-CH"}):
        if code in langs: out["lang/" + code] = langs[code]
    return out

def supplemental(cdir):
    out = {}
    def j(name): return json.load(open(os.path.join(cdir, "supp", name + ".json"), encoding="utf-8"))["supplemental"]
    for kind, name in (("cardinal", "plurals"), ("ordinal", "ordinals")):
        for lang, rules in j(name)["plurals-type-" + kind].items():
            for k, v in rules.items():
                r = v.split("@")[0].strip()
                if r or k.endswith("other"): out["plural/%s/%s/%s" % (kind, lang, k.replace("pluralRule-count-", ""))] = r
    w = j("weekData")["weekData"]
    for k in ("firstDay", "minDays", "weekendStart", "weekendEnd"):
        for reg, v in w[k].items():
            if "alt" not in reg: out["week/%s/%s" % (k, reg)] = str(v)
    for reg, v in j("measurementData")["measurementData"]["measurementSystem"].items(): out["measure/" + reg] = v
    for reg, v in j("measurementData")["measurementData"].get("paperSize", {}).items(): out["paper/" + reg] = v
    cd = j("currencyData")["currencyData"]
    for code, f in cd["fractions"].items(): out["curdigits/" + code] = f.get("_digits", "2") + "," + f.get("_rounding", "0") + "," + f.get("_cashDigits", f.get("_digits", "2")) + "," + f.get("_cashRounding", f.get("_rounding", "0"))
    for reg, lst in cd["region"].items():
        for item in lst:
            for code, info in item.items():
                if "_to" not in info and info.get("_tender", "true") != "false": out["curregion/" + reg] = code
    for k, v in j("likelySubtags")["likelySubtags"].items():
        parts = k.split("-")
        if len(parts) == 1 or (parts[0] == "und" and len(parts) == 2) or k in ("zh-Hant", "zh-TW", "zh-HK", "sr-Latn", "pt-PT"):
            out["likely/" + k] = v
    return out

def cstr(s):
    o = []
    for b in s.encode("utf-8"):
        if b in (0x22, 0x5C): o.append("\\" + chr(b))
        elif 0x20 <= b < 0x7F and b != 0x3F: o.append(chr(b))
        else: o.append("\\%03o" % b)
    return '"' + "".join(o) + '"'

def main():
    cdir, outp = sys.argv[1], sys.argv[2]
    data = {loc: load(cdir, loc) for loc in LOCALES}
    for loc in LOCALES:
        if "-" in loc:
            parent = data.get(loc.split("-")[0], {})
            data[loc] = {k: v for k, v in data[loc].items() if parent.get(k) != v}
    supp = supplemental(cdir)
    keys = sorted({k for d in data.values() for k in d} | set(supp))
    kid = {k: i for i, k in enumerate(keys)}
    # Strings live in chunks below 64 KiB (MSVC caps a string literal at 65535 bytes); offset = chunk << 16 | pos.
    chunks = [bytearray()]; offs = {}
    def intern(s):
        if s not in offs:
            b = s.encode("utf-8")
            if len(chunks[-1]) + len(b) + 1 > 60000: chunks.append(bytearray())
            offs[s] = (len(chunks) - 1) << 16 | len(chunks[-1]); chunks[-1].extend(b); chunks[-1].append(0)
        return offs[s]
    for k in keys: intern(k)
    with open(outp, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("// Generated by tools/cldr/gen_cldr.py from CLDR 46 (Unicode License v3). Do not edit.\n")
        f.write("namespace forge::os::locale::cldr {\n")
        f.write("inline constexpr const char cldr_version[] = \"46\";\n")
        entries = {}
        for loc in ["root"] + LOCALES:
            d = supp if loc == "root" else data[loc]
            entries[loc] = sorted((kid[k], intern(v)) for k, v in d.items())
        f.write("inline constexpr unsigned key_count = %d;\n" % len(keys))
        f.write("inline constexpr unsigned key_offsets[] = {\n")
        for i in range(0, len(keys), 16): f.write("    " + ",".join(str(offs[k]) for k in keys[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write("struct entry { unsigned short key; unsigned value; };\n")
        for loc, es in entries.items():
            name = "loc_" + loc.replace("-", "_")
            f.write("inline constexpr entry %s[] = {\n" % name)
            for i in range(0, len(es), 12): f.write("    " + ",".join("{%d,%d}" % e for e in es[i:i + 12]) + ",\n")
            f.write("};\n")
        f.write("struct locale_table { const char* tag; const entry* entries; unsigned count; };\n")
        f.write("inline constexpr locale_table locales[] = {\n")
        for loc in ["root"] + LOCALES:
            f.write('    {"%s", loc_%s, %d},\n' % (loc, loc.replace("-", "_"), len(entries[loc])))
        f.write("};\n")
        for ci, chunk in enumerate(chunks):
            f.write("inline constexpr char pool%d[] =\n" % ci)
            i = 0
            while i < len(chunk):
                e = min(i + 3000, len(chunk))
                while e < len(chunk) and 0x80 <= chunk[e] < 0xC0: e -= 1   # never split a UTF-8 sequence
                part = bytes(chunk[i:e]); i = e
                # Raw UTF-8 keeps the file small; only NUL, quotes, backslash, '?' (trigraphs) and controls are escaped.
                o = bytearray()
                for b in part:
                    if b in (0x22, 0x5C): o += b"\\" + bytes([b])
                    elif b == 0x3F or b < 0x20 or b == 0x7F: o += b"\\%03o" % b
                    else: o.append(b)
                f.write('    "' + o.decode("utf-8", "surrogateescape") + '"\n')
            f.write(";\n")
        f.write("inline constexpr const char* pools[] = {%s};\n" % ", ".join("pool%d" % i for i in range(len(chunks))))
        f.write("inline const char* str(unsigned off) { return pools[off >> 16] + (off & 0xFFFF); }\n")
        f.write("}  // namespace forge::os::locale::cldr\n")
    print("keys", len(keys), "pool", sum(map(len, chunks)), "chunks", len(chunks), "entries", sum(len(e) for e in entries.values()))

main()
