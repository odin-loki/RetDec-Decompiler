# URLs below are required artifact locations for CMake dependency downloads (not documentation).

set(CAPSTONE_URL
    "https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.9.zip"
    CACHE STRING "URL of Capstone archive to use."
)
set(CAPSTONE_ARCHIVE_SHA256
    "0a651143e88a9c244a05dba149ee34e0379bebffa392eb4ccc285fc360442c4d"
    CACHE STRING ""
)

set(GOOGLETEST_URL
    "https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip"
    CACHE STRING "URL of Googletest archive to use."
)
set(GOOGLETEST_ARCHIVE_SHA256
    "f179ec217f9b3b3f3c6e8b02d3e7eda997b49e4ce26d6b235c9053bec9c0bf9f"
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

set(LLVM_URL
    "https://github.com/avast/llvm/archive/a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1.zip"
    CACHE STRING "URL of LLVM archive to use."
)
set(LLVM_ARCHIVE_SHA256
    "b5879b30768135e5fce84ccd8be356d2c55c940ab32ceb22d278b228e88c4c60"
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
set(ZLIB_URL
	"https://zlib.net/fossils/zlib-1.3.1.tar.gz"
	CACHE STRING "URL of zlib tarball for bundled cross builds (fossils mirror; root zlib.net path may 404)."
)
set(ZLIB_ARCHIVE_SHA256
	"9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
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
# b10451 is current as of 2026-08-16 and includes qwen35 / MTP (b9180+).
set(LLAMACPP_URL
    "https://github.com/ggml-org/llama.cpp/archive/refs/tags/b10451.zip"
    CACHE STRING "URL of llama.cpp archive to use."
)
set(LLAMACPP_ARCHIVE_SHA256
    "b04aeb511cc05451a410437eacd5a2d64a3130c27f10a54a23ad948369816cad"
    CACHE STRING ""
)

# xsimd — header-only; enable with -DRETDEC_ENABLE_XSIMD=ON. Do not vendor under deps/xsimd/.
set(XSIMD_URL
    "https://github.com/xtensor-stack/xsimd/archive/refs/tags/13.2.0.zip"
    CACHE STRING "URL of xsimd archive to use."
)
set(XSIMD_ARCHIVE_SHA256
    "3ff360dc82109b11b35389a5dfed8ac15155f356f39840dff2be2e230b935b8c"
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

# tree-sitter runtime — ABI 15, compatible with tree-sitter-c 0.24.2 (ABI 14).
set(TREE_SITTER_URL
    "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.26.12.zip"
    CACHE STRING "URL of tree-sitter archive to use."
)
set(TREE_SITTER_ARCHIVE_SHA256
    "cbafe90818093cd5f2b2f56ff8c10504bab9f26840a80fabd11985e2445f91ee"
    CACHE STRING ""
)
option(RETDEC_SUPPORT_PKG_VERIFY_SHA256
	"Verify SHA-256 of downloaded retdec-support archive (disable for custom mirrors/tarballs)"
	ON)
