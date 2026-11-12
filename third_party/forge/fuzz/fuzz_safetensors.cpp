#include "fuzz.hpp"
#include <forge/import/safetensors.hpp>
FORGE_FUZZ({
    forge::import::safetensors s(forge::fuzz::as_file(data, size, ".safetensors"));
    for (auto& [n, e] : s.entries()) (void)s.load(n);
})
