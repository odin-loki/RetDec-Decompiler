#include "fuzz.hpp"
#include <forge/inference/grammar.hpp>
// input = GBNF source; then drive the pushdown automaton with a pseudo-random byte walk that
// prefers bytes the grammar accepts (bounded so recursion / stack growth stays finite).
FORGE_FUZZ({
    std::string_view src{reinterpret_cast<const char*>(data), size};
    forge::inference::grammar g(src);
    auto st = g.start();
    std::uint32_t h = 2166136261u; for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619u;
    for (int step = 0; step < 48 && !st.empty(); ++step) {
        std::vector<unsigned char> ok;
        for (int c = 1; c < 256; ++c) if (!g.advance(st, (unsigned char)c).empty()) ok.push_back((unsigned char)c);
        (void)forge::inference::grammar::can_end(st);
        if (ok.empty()) break;
        h = h * 1103515245u + 12345u;
        st = g.advance(st, ok[(h >> 8) % ok.size()]);
        if (st.size() > 4096) break;
    }
})
