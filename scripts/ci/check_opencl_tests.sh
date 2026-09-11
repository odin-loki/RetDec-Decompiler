#!/usr/bin/env bash
# OCL-01 — build src/opencl, compile its kernels, and run tests/opencl.
#
# Why this exists
# ---------------
# src/opencl is 4,547 lines with a complete CMakeLists.txt that no
# add_subdirectory names, and tests/opencl has eight suites with the same. No
# build of this repository compiled either, and the audit entry that recorded
# that also explained why it was left alone: src/opencl/CMakeLists.txt opens
# with find_package(OpenCL REQUIRED), so wiring it in unconditionally breaks
# configure everywhere without an SDK.
#
# That reason was about CMake, and the sources need no CMake to be checked.
# They include <CL/cl.h>, the standard library and their own headers -- no
# LLVM, nothing vendored. Compiling them took one command, and the first one
# found a file that has never compiled at all:
#
#   ocl_type_inferencer.cpp:297: no match for 'operator|=' (operand types are
#   'std::vector<bool>::reference' and 'bool')
#
# What this checks, and why each part
# -----------------------------------
#  1. Every src/opencl and tests/opencl translation unit compiles.
#  2. Every kernel builds. There are TWO copies of each: kernels/*.cl, and a
#     C++ raw-string literal in src/opencl/*_sources.cpp. The runtime compiles
#     the embedded copy, so checking only the .cl files would check the copy
#     that never runs. Three of five were broken in each -- `schar` is not an
#     OpenCL C type, `&(uint)expr` is not an lvalue, and atomic_inc's cast made
#     it ambiguous.
#  3. The suites run and at least MIN_TESTS of them pass.
#
# Without an ICD the tests still run: every GPU case calls GTEST_SKIP. That is
# 24 of the 96 silently not running, so when an ICD is present this requires
# zero skips. POCL is enough and is what CI installs.
#
# A kernel that fails to build is the reason this cannot rely on the suite
# alone: OCLContext falls back to the CPU path when a build fails, so the GPU
# tests pass green whether the kernel compiled or not. They did exactly that.
#
# Usage: bash scripts/ci/check_opencl_tests.sh [--jobs N] [--keep]
#                                              [--min-tests N] [--self-test]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JOBS="$(nproc 2>/dev/null || echo 4)"
KEEP=0
MIN_TESTS=90
SELF_TEST=0
WORKDIR="${OCL_WORKDIR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--jobs)      JOBS="$2"; shift 2 ;;
		--keep)      KEEP=1; shift ;;
		--min-tests) MIN_TESTS="$2"; shift 2 ;;
		--self-test) SELF_TEST=1; shift ;;
		*) echo "OCL-01: unknown option: $1" >&2; exit 2 ;;
	esac
done

die() { echo "OCL-01: FAIL $*" >&2; exit 1; }

command -v "${CXX:-g++}" >/dev/null || die "no C++ compiler"
printf '#include <CL/cl.h>\nint main(void){return 0;}\n' > /tmp/ocl-hdr-probe.c
"${CC:-gcc}" -fsyntax-only /tmp/ocl-hdr-probe.c 2>/dev/null \
	|| die "<CL/cl.h> is missing -- install opencl-headers"
rm -f /tmp/ocl-hdr-probe.c

if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d -t ocl-check-XXXXXX)"
	[ "${KEEP}" = 1 ] || trap 'rm -rf "${WORKDIR}"' EXIT
else
	mkdir -p "${WORKDIR}"
fi

cd "${ROOT}"

# ── 1. Compile every translation unit ────────────────────────────────────────
srcs=()
while IFS= read -r f; do srcs+=("$f"); done < <(find src/opencl tests/opencl -name '*.cpp' | sort)
[ "${#srcs[@]}" -ge 15 ] || die "expected at least 15 sources, found ${#srcs[@]}"

echo "OCL-01: compiling ${#srcs[@]} translation units with ${JOBS} job(s)"
: > "${WORKDIR}/compile.err"
printf '%s\n' "${srcs[@]}" | xargs -P "${JOBS}" -I{} \
	sh -c '"${CXX:-g++}" -std=c++23 -fsyntax-only -Iinclude -Isrc/opencl "$1" \
		2>>'"${WORKDIR}"'/compile.err || echo "FAILED $1" >>'"${WORKDIR}"'/compile.err' _ {}
if grep -q '^FAILED ' "${WORKDIR}/compile.err"; then
	echo "OCL-01: FAIL these do not compile:" >&2
	grep '^FAILED ' "${WORKDIR}/compile.err" >&2
	grep 'error:' "${WORKDIR}/compile.err" | head -20 >&2
	exit 1
fi

# ── 2. Build every kernel, in both copies ────────────────────────────────────
# The embedded copies are what the runtime compiles; the .cl files are what a
# reader sees. Both have to build or one of the two is a lie.
mkdir -p "${WORKDIR}/embedded"
python3 - "${WORKDIR}/embedded" <<'PY'
import re, sys
from pathlib import Path
out = Path(sys.argv[1])
MAP = {"parallelDisasmClSource": "parallel_disasm.cl",
       "typePropagationClSource": "type_propagation.cl",
       "steensgaardClSource": "steensgaard.cl",
       "semanticHashClSource": "semantic_hash.cl",
       "egraphSimplifyClSource": "egraph_simplify.cl"}
text = "".join((Path("src/opencl") / f).read_text(encoding="utf-8")
               for f in ["parallel_disasm_sources.cpp", "additional_kernel_sources.cpp"])
missing = []
for fn, cl in MAP.items():
    m = re.search(re.escape(fn) + r'\(\)\s*\{.*?return R"([A-Za-z_]*)\(', text, re.S)
    if not m:
        missing.append(fn); continue
    d = m.group(1); start = m.end()
    end = text.index(f'){d}";', start)
    (out / cl).write_text(text[start:end], encoding="utf-8")
if missing:
    print("OCL-01: FAIL no embedded source found for: " + ", ".join(missing))
    sys.exit(1)
PY

cat > "${WORKDIR}/klint.c" <<'KLINT'
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
	cl_platform_id p; cl_device_id d; cl_int err;
	if (clGetPlatformIDs(1, &p, NULL) != CL_SUCCESS) return 2;
	if (clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 1, &d, NULL) != CL_SUCCESS) return 2;
	cl_context c = clCreateContext(NULL, 1, &d, NULL, NULL, &err);
	int bad = 0;
	for (int i = 1; i < argc; i++) {
		FILE* f = fopen(argv[i], "rb");
		if (!f) { printf("open failed: %s\n", argv[i]); bad++; continue; }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		char* src = malloc(n + 1); fread(src, 1, n, f); src[n] = 0; fclose(f);
		const char* s = src;
		cl_program prog = clCreateProgramWithSource(c, 1, &s, NULL, &err);
		if (clBuildProgram(prog, 1, &d, "", NULL, NULL) != CL_SUCCESS) {
			size_t ls = 0;
			clGetProgramBuildInfo(prog, d, CL_PROGRAM_BUILD_LOG, 0, NULL, &ls);
			char* log = malloc(ls + 1);
			clGetProgramBuildInfo(prog, d, CL_PROGRAM_BUILD_LOG, ls, log, NULL);
			log[ls] = 0;
			printf("=== %s ===\n%.1200s\n", argv[i], log);
			bad++;
		}
		free(src);
	}
	printf("kernels failing to build: %d\n", bad);
	return bad ? 1 : 0;
}
KLINT
"${CC:-gcc}" "${WORKDIR}/klint.c" -lOpenCL -o "${WORKDIR}/klint" \
	|| die "could not build the kernel compiler probe (is ocl-icd-opencl-dev installed?)"

# Called with no kernels, klint exits 0 when it found a device and 2 when it
# did not. That distinction matters: "no device" is a reduced run, "a kernel
# does not build" is a failure, and conflating them is how the GPU half went
# unmeasured in the first place.
set +e
"${WORKDIR}/klint" >/dev/null 2>&1
probe_status=$?
set -e
HAVE_DEVICE=1
if [ "${probe_status}" = 2 ]; then
	HAVE_DEVICE=0
fi

if [ "${HAVE_DEVICE}" = 1 ]; then
	kfail=0
	"${WORKDIR}/klint" src/opencl/kernels/*.cl || kfail=1
	"${WORKDIR}/klint" "${WORKDIR}"/embedded/*.cl || kfail=1
	[ "${kfail}" = 0 ] || die "a kernel does not build (see the log above)"
	echo "OCL-01: 5 kernels and 5 embedded copies build"
else
	echo "OCL-01: no OpenCL device -- kernels not compiled, GPU cases will skip" >&2
fi

# ── 3. Link and run the suites ───────────────────────────────────────────────
"${CXX:-g++}" -std=c++23 -Iinclude -Isrc/opencl \
	src/opencl/*.cpp tests/opencl/*.cpp \
	-lgtest -lgtest_main -lOpenCL -pthread \
	-o "${WORKDIR}/ocl_tests" 2>"${WORKDIR}/link.err" \
	|| { grep -E 'error|undefined' "${WORKDIR}/link.err" | head -20 >&2; die "link failed"; }

set +e
"${WORKDIR}/ocl_tests" --gtest_brief=1 > "${WORKDIR}/run.log" 2>&1
run_status=$?
set -e

ran="$(sed -n 's/.*\[==========\] \([0-9]*\) tests\? from .* ran.*/\1/p' "${WORKDIR}/run.log" | tail -1)"
passed="$(sed -n 's/.*\[  PASSED  \] \([0-9]*\) tests\?.*/\1/p' "${WORKDIR}/run.log" | tail -1)"
skipped="$(sed -n 's/.*\[  SKIPPED \] \([0-9]*\) tests\?.*/\1/p' "${WORKDIR}/run.log" | tail -1)"
: "${ran:=0}" "${passed:=0}" "${skipped:=0}"

if [ "${run_status}" != 0 ]; then
	grep -E '^\[  FAILED  \]' "${WORKDIR}/run.log" | head -20 >&2
	die "tests/opencl does not pass (${passed} passed, ${skipped} skipped)"
fi

[ "${passed}" -ge "${MIN_TESTS}" ] \
	|| die "only ${passed} test(s) passed, expected at least ${MIN_TESTS}"

# With a device present nothing may skip: a skip here means the GPU half went
# unmeasured, which is the state this check was written to end.
if [ "${HAVE_DEVICE}" = 1 ] && [ "${skipped}" != 0 ]; then
	die "${skipped} test(s) skipped although an OpenCL device is present"
fi

echo "OCL-01: OK ${passed} test(s) passed, ${skipped} skipped"

if [ "${SELF_TEST}" = 1 ]; then
	# A check that cannot fail is worth nothing. Break one thing and require
	# that this script notices.
	echo "OCL-01: self-test -- a kernel that does not build must fail the check"
	probe="${WORKDIR}/selftest.cl"
	printf '__kernel void k(__global int* o) { o[0] = not_a_function(); }\n' > "${probe}"
	if [ "${HAVE_DEVICE}" = 1 ]; then
		if "${WORKDIR}/klint" "${probe}" >/dev/null 2>&1; then
			die "self-test: the kernel probe accepted a kernel that cannot build"
		fi
		echo "OCL-01: self-test OK"
	else
		echo "OCL-01: self-test skipped -- no OpenCL device" >&2
	fi
fi
