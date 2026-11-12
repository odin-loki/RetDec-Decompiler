// Lets clang++ syntax-check the generated kernels.metal on hosts without the Metal toolchain (ctest
// gpu_compile_metal_syntax). Address-space keywords vanish, Metal attributes are ignored.
#pragma once
#include <cmath>
#include <cstring>
#define FORGE_MSL_HOST_SHIM
#define device
#define constant
#define thread
#define kernel
using uint = unsigned;
template<class T, class F> T as_type(F f) { T t; std::memcpy(&t, &f, sizeof t); return t; }
using std::exp2; using std::log2; using std::sqrt; using std::fma; using std::sin; using std::cos;
