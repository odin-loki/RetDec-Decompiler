#pragma once
// Portable OS entropy: getentropy needs Android API 28 / macOS 10.12; arc4random_buf is everywhere on BSD/Apple/Bionic.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <forge/os/os.hpp>

namespace forge::modelfile {
inline int os_entropy(void* p, std::size_t n) { return os::entropy(p, n) ? 0 : -1; }
}  // namespace forge::modelfile
