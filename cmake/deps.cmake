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
    "https://github.com/keystone-engine/keystone/archive/d7ba8e378e5284e6384fc9ecd660ed5f6532e922.zip"
    CACHE STRING "URL of Keystone archive to use."
)
set(KEYSTONE_ARCHIVE_SHA256
    "13bd00e062e9c778fe76aaab5c163348b3c9457c0e9b2a4c2fb3e2d8747694ca"
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
option(RETDEC_SUPPORT_PKG_VERIFY_SHA256
	"Verify SHA-256 of downloaded retdec-support archive (disable for custom mirrors/tarballs)"
	ON)
