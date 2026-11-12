// forge::os::text: terminal display width (East Asian Width, emoji presentation, combining marks).
#include "internal.hpp"

namespace forge::os::text {
using namespace detail;
int width(cp_t c, width_options o) {
    if (c == 0) return 0;
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return -1;
    auto& p = prop(c);
    if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0xFEFF) return 0;
    if (p.gc == int(category::Mn) || p.gc == int(category::Me) || p.gc == int(category::Cf)) return c == 0x00AD ? 1 : 0;
    if (c >= 0x1160 && c <= 0x11FF) return 0;   // Hangul medial vowels / final consonants combine
    if (p.flags & F_DefIgn) return 0;
    if (p.eaw == EA_W || p.eaw == EA_F) return 2;
    if (p.eaw == EA_A) return o.ambiguous_wide ? 2 : 1;
    return 1;
}
int display_width(std::string_view s, width_options o) {
    int w = 0;
    for (auto g : graphemes(s)) {
        std::size_t i = 0; cp_t first = decode_utf8(g, i);
        int gw = width(first, o); if (gw < 0) gw = 0;
        bool emoji_seq = false;
        while (i < g.size()) {
            cp_t c = decode_utf8(g, i);
            if (c == 0xFE0F && is_emoji(first)) emoji_seq = true;               // VS16: emoji presentation
            else if (c == 0x200D || (c >= 0x1F3FB && c <= 0x1F3FF)) emoji_seq |= is_extended_pictographic(first);
            else if (prop(first).gcb == GB_RI && prop(c).gcb == GB_RI) emoji_seq = true;
            else if (c == 0xFE0E) gw = 1;                                       // VS15: text presentation
            else if (prop(c).gc == int(category::Mc)) gw += 0;                  // spacing marks stay in the cluster
        }
        if (emoji_seq) gw = 2;
        w += gw;
    }
    return w;
}
}  // namespace forge::os::text
