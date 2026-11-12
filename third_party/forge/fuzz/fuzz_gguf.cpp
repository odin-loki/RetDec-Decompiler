#include "fuzz.hpp"
#include <forge/import/gguf.hpp>
FORGE_FUZZ({
    forge::import::gguf_file g(forge::fuzz::as_file(data, size, ".gguf"));
    (void)g.get("general.architecture");
    for (auto& t : g.tensors()) {
        try { (void)g.bytes(t); } catch (const forge::forge_exception&) {}
    }
})
