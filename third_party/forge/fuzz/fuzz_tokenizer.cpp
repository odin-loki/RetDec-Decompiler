#include "fuzz.hpp"
#include <forge/modelfile/tokenizer.hpp>
FORGE_FUZZ({
    std::string_view s{reinterpret_cast<const char*>(data), size};
    auto cut = s.find('\x01');  // input = tokenizer.json [\x01 text-to-encode]
    forge::modelfile::bpe_tokenizer t(s.substr(0, cut));
    auto txt = cut == std::string_view::npos ? std::string_view("the theory of things") : s.substr(cut + 1);
    auto ids = t.encode(txt);
    (void)t.decode(ids);
})
