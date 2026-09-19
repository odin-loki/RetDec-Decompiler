# URLs below are required artifact locations for CMake dependency downloads (not documentation).

set(CAPSTONE_URL
    "https://github.com/capstone-engine/capstone/archive/refs/tags/6.0.0-Alpha10.zip"
    CACHE STRING "URL of Capstone archive to use."
)
set(CAPSTONE_ARCHIVE_SHA256
    "b42356d981d4c9e791c917b682689e8baeaa0e0db5385ded17254b1214828ee4"
    CACHE STRING ""
)

set(GOOGLETEST_URL
    "https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip"
    CACHE STRING "URL of Googletest archive to use."
)
set(GOOGLETEST_ARCHIVE_SHA256
    "63b9c77751a5b8f492486005f67533fdc58682b67476fcb91650be5958d5195a"
    CACHE STRING ""
)

set(KEYSTONE_URL
    "https://github.com/keystone-engine/keystone/archive/refs/tags/0.9.2.zip"
    CACHE STRING "URL of Keystone archive to use."
)
set(KEYSTONE_ARCHIVE_SHA256
    "9cebf492f64b8632d0a0678ca334f266e78978d6873dacfb795c8753d8afb12c"
    CACHE STRING ""
)

# Existing CMake build directories cache these values. After a pin bump,
# reconfigure with -DLLVM_URL=... -DLLVM_ARCHIVE_SHA256=... or drop those
# cache entries. Otherwise ExternalProject keeps the previous archive.
set(LLVM_URL
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.0/llvm-project-23.1.0.src.tar.xz"
    CACHE STRING "URL of LLVM archive to use."
)
set(LLVM_ARCHIVE_SHA256
    "ab1f0e3ec52448c33e8782eaf0422504b87c7b016b22514653ee0d8fcee479ff"
    CACHE STRING ""
)

set(YARA_URL
    "https://github.com/VirusTotal/yara/archive/v4.5.8.zip"
    CACHE STRING "URL of Yara archive to use."
)
set(YARA_ARCHIVE_SHA256
    "e623b16e4b0b07bb4ea614c1ea03c8a9b7d90b457e5f0bd65cdac7bca2f0c290"
    CACHE STRING ""
)

set(YARAMOD_URL
    "https://github.com/avast/yaramod/archive/v4.8.1.zip"
    CACHE STRING "URL of YaraMod archive to use."
)
set(YARAMOD_ARCHIVE_SHA256
	"c21ac8fa012d683f295affc0609eca6eb0741f5a76fcea3e2d5a517a52342d62"
	CACHE STRING ""
)

# zlib (bundled for Linux/Unix -> Windows MinGW cross so LLVM can use LLVM_ENABLE_ZLIB=ON).
#
# A LIST, tried in order. One URL is one outage away from a red build: a
# ctest-windows run failed with
#
#     SHA256 hash of .../zlib-1.3.1.tar.gz does not match expected value
#       expected: '9a93b2b7...'
#         actual: 'e21df9a9...'
#
# three times over, from zlib.net. The hash check did its job -- it refused
# whatever that was -- and then the build had nowhere else to go. madler's
# GitHub release asset is byte-identical to zlib.net's tarball (same SHA256,
# 1,512,791 bytes, verified) and is served by a CDN, so it goes first.
# The bare https://www.zlib.net/zlib-1.3.2.tar.gz is NOT a fallback: it is a
# 355-byte error page.
set(ZLIB_URL
	"https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz;https://zlib.net/fossils/zlib-1.3.2.tar.gz"
	CACHE STRING "URLs of the zlib tarball, tried in order (GitHub release asset, then zlib.net fossils)."
)
set(ZLIB_ARCHIVE_SHA256
	"bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
	CACHE STRING ""
)

set(SUPPORT_PKG_URL
    "https://github.com/avast/retdec-support/releases/download/2019-03-08/retdec-support_2019-03-08.tar.xz"
    CACHE STRING "URL of RetDec support package to use."
)
set(SUPPORT_PKG_SHA256
    "629351609bca0f4b8edbd4e53789192305256aeb908e953f5546e121a911d54e"
    CACHE STRING ""
)
set(SUPPORT_PKG_VERSION
    "2019-03-08"
    CACHE STRING ""
)

# llama.cpp — enable with -DRETDEC_ENABLE_LLAMACPP=ON; bump via scripts/upgrade-dep.sh LLAMACPP
# v0.4.1 is the latest stable semver (nightly line is bNNNN; do not pin nightlies).
set(LLAMACPP_URL
    "https://github.com/ggml-org/llama.cpp/archive/refs/tags/v0.4.1.zip"
    CACHE STRING "URL of llama.cpp archive to use."
)
set(LLAMACPP_ARCHIVE_SHA256
    "cc4ddb85b68fc4a03f606d7911528796815966273c3a61dfba6fc69932b15cfa"
    CACHE STRING ""
)

# xsimd — header-only; enable with -DRETDEC_ENABLE_XSIMD=ON. Do not vendor under deps/xsimd/.
set(XSIMD_URL
    "https://github.com/xtensor-stack/xsimd/archive/refs/tags/14.3.0.zip"
    CACHE STRING "URL of xsimd archive to use."
)
set(XSIMD_ARCHIVE_SHA256
    "b768fe65493c6f3849dcbdd34987382f31e4b561eb15b663a51af5f105fc4aca"
    CACHE STRING ""
)

# tree-sitter-c — C grammar for N10. URL/SHA only this commit (Wave 5 3b).
# Complete source is the release tarball, not the GitHub tag zip.
set(TREE_SITTER_C_URL
    "https://github.com/tree-sitter/tree-sitter-c/releases/download/v0.24.2/tree-sitter-c.tar.gz"
    CACHE STRING "URL of tree-sitter-c archive to use."
)
set(TREE_SITTER_C_ARCHIVE_SHA256
    "f3a2cdfbca39c79f60baf2ef62b42084c609782c76485de457ba36ff65d51baf"
    CACHE STRING ""
)

# tree-sitter runtime — ABI 15, compatible with tree-sitter-c 0.24.2 (LANGUAGE_VERSION 15).
set(TREE_SITTER_URL
    "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.0.zip"
    CACHE STRING "URL of tree-sitter archive to use."
)
set(TREE_SITTER_ARCHIVE_SHA256
    "71e0997e8ada866c673b63836f8aa2ea8035d251d1ec62fc33e1e191e0916e95"
    CACHE STRING ""
)
option(RETDEC_SUPPORT_PKG_VERIFY_SHA256
	"Verify SHA-256 of downloaded retdec-support archive (disable for custom mirrors/tarballs)"
	ON)
