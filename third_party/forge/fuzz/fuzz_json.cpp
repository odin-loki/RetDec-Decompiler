#include "fuzz.hpp"
#include <forge/json/json.hpp>
FORGE_FUZZ({ auto v = forge::json::parse({reinterpret_cast<const char*>(data), size}); (void)forge::json::dump(v); })
