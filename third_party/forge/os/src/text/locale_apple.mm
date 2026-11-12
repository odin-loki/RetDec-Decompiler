// forge::os::locale: Apple hooks (CoreFoundation) for the system locale, preferred languages and time zone.
#include "locale_internal.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <cstring>

namespace forge::os::locale::detail {
namespace {
std::string to_std(CFStringRef s) {
    if (!s) return {};
    if (const char* p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return p;
    CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string r(std::size_t(len), '\0');
    if (!CFStringGetCString(s, r.data(), len, kCFStringEncodingUTF8)) return {};
    r.resize(std::strlen(r.c_str())); return r;
}
}  // namespace
std::string apple_system_locale() {
    CFLocaleRef l = CFLocaleCopyCurrent(); if (!l) return {};
    std::string r = to_std(CFLocaleGetIdentifier(l));   // "en_AU", "zh-Hans_CN", "en_AU@calendar=japanese"
    CFRelease(l); return r;
}
// Language & Region customisations: first weekday, the "j" (preferred hour) skeleton resolved for the current locale
// (it honours the 24-Hour Time switch), and the number separators.
// Only values the user changed are reported: each is compared with a plain CFLocale of the same identifier.
void apple_user_overrides(std::string& fw, std::string& time_format, std::string& decimal, std::string& group) {
    CFLocaleRef l = CFLocaleCopyCurrent(); if (!l) return;
    CFLocaleRef d = CFLocaleCreate(kCFAllocatorDefault, CFLocaleGetIdentifier(l));
    if (!d) { CFRelease(l); return; }
    if (CFCalendarRef cal = CFCalendarCopyCurrent()) {
        static const char* wd[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};   // CFCalendar: 1 = Sunday
        CFIndex f = CFCalendarGetFirstWeekday(cal), f0 = f;
        if (CFCalendarRef dc = CFCalendarCreateWithIdentifier(kCFAllocatorDefault, CFCalendarGetIdentifier(cal))) { CFCalendarSetLocale(dc, d); f0 = CFCalendarGetFirstWeekday(dc); CFRelease(dc); }
        if (f != f0 && f >= 1 && f <= 7) fw = wd[f - 1];
        CFRelease(cal);
    }
    auto hour = [](CFLocaleRef x) { std::string r; if (CFStringRef p = CFDateFormatterCreateDateFormatFromTemplate(kCFAllocatorDefault, CFSTR("j"), 0, x)) { r = to_std(p); CFRelease(p); } return r; };
    if (auto h = hour(l); h != hour(d)) time_format = h;
    auto val = [](CFLocaleRef x, CFStringRef k) { return to_std(static_cast<CFStringRef>(CFLocaleGetValue(x, k))); };
    if (auto v = val(l, kCFLocaleDecimalSeparator); v != val(d, kCFLocaleDecimalSeparator)) decimal = v;
    if (auto v = val(l, kCFLocaleGroupingSeparator); v != val(d, kCFLocaleGroupingSeparator)) group = v;
    CFRelease(d); CFRelease(l);
}
std::vector<std::string> apple_preferred_languages() {
    std::vector<std::string> r;
    CFArrayRef a = CFLocaleCopyPreferredLanguages(); if (!a) return r;
    for (CFIndex i = 0, n = CFArrayGetCount(a); i < n; ++i) r.push_back(to_std((CFStringRef)CFArrayGetValueAtIndex(a, i)));
    CFRelease(a); return r;
}
std::string apple_local_zone() {
    CFTimeZoneRef z = CFTimeZoneCopySystem(); if (!z) return {};
    std::string r = to_std(CFTimeZoneGetName(z));
    CFRelease(z); return r;
}
}  // namespace forge::os::locale::detail
