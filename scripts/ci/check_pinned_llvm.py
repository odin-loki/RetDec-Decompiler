#!/usr/bin/env python3
"""PIN-01 -- compile the tree against the LLVM this project PINS.

Why this exists
---------------
cmake/deps.cmake pins LLVM 23.1.0 and ctest-linux builds it from source. Every
other check in this directory takes whatever llvm-config the machine happens to
have: 18 or 20 on a developer box, 20 in standalone-check. So the toolchain the
project is actually built with was compiled against by exactly one workflow,
the slowest one, and nothing cheap could see a break in it.

One did break. `top = irb.CreateUnaryIntrinsic(...)` where `top` is the
CallInst* that loadX87DataReg returns compiles on LLVM 20, whose
CreateUnaryIntrinsic returns CallInst*, and does not on 23, whose returns
Value*. ctest-linux said so for a day and a half while every other check stayed
green.

B2L-01's header already held a symptom of the same gap, read at the time as a
local quirk: it lists src/debugformat/dwarf.cpp among 23 translation units that
"do not compile against the distribution LLVM 20" because it uses a post-20
DataExtractor constructor and header layout. It compiles against 23. The file
was not wrong; the compiler was.

What this measures, and what it does not
----------------------------------------
It fetches the pinned archive -- the URL and SHA256 read out of
cmake/deps.cmake, so the two cannot drift -- and builds it as far as the
tablegen-generated headers. That is intrinsics_gen plus
llvm/Analysis/TargetLibraryInfo.inc: a couple of minutes, no LLVM libraries,
about 3 GiB. Then every entry of the project's own compile_commands.json is
re-run as -fsyntax-only against those headers.

Driving it from the compile database rather than from a hand-written flag list
is the point. A first attempt with guessed flags reported fifty-odd failures,
nearly all of them src/cli_parser needing -std=c++20 and nothing to do with
LLVM. The database says what CMake gives each translation unit.

-fsyntax-only is a real limit and is stated rather than glossed: it sees
declarations and types, not link-time symbols and not template instantiations
that only a full compile forces. ctest-linux remains the thing that builds and
links. This is the cheap half that can run before a push.

Prerequisites
-------------
A configured build directory, for its compile_commands.json. Configuring does
not build the dependencies -- the paths it writes for LLVM and Capstone need
not exist yet, and this substitutes its own for them. Capstone's headers do
have to exist somewhere; --capstone-prefix points at them and defaults to the
one check_capstone2llvmir_tests.sh builds.

Usage:
  python3 scripts/ci/check_pinned_llvm.py [--workdir DIR] [--compile-db PATH]
                                          [--capstone-prefix DIR] [--jobs N]
  python3 scripts/ci/check_pinned_llvm.py --self-test
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import textwrap
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# A floor, not an equality. Adding sources must not fail this; losing most of
# them to a broken substitution must.
MIN_CHECKED = 900

# Dependency headers that the build's own download step writes and this check
# does not run. A translation unit that stops at one of these is skipped and
# named; anything else that fails to compile is a failure.
FETCHED_HEADERS = ("tree_sitter/",)

# Space the LLVM source tree and its tablegen build need.
MIN_FREE_GIB = 4


def die(msg):
    print(f"PIN-01: FAIL {msg}", file=sys.stderr)
    sys.exit(1)


def read_pin():
    """The archive URL and SHA256, out of cmake/deps.cmake."""
    text = (ROOT / "cmake" / "deps.cmake").read_text(encoding="utf-8")
    url = re.search(
        r"set\(LLVM_URL\s*\n\s*\"([^\"]+)\"", text)
    sha = re.search(
        r"set\(LLVM_ARCHIVE_SHA256\s*\n\s*\"([0-9a-f]{64})\"", text)
    if not url or not sha:
        die("cannot read LLVM_URL / LLVM_ARCHIVE_SHA256 out of cmake/deps.cmake")
    return url.group(1), sha.group(1)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_and_extract(work, url, sha):
    """The pinned archive, verified, with the four directories LLVM's CMake needs."""
    src = work / "llvm-src"
    if (src / "llvm" / "CMakeLists.txt").is_file() and (src / "libc").is_dir():
        print(f"PIN-01: source tree already in {src}")
        return src

    free_gib = shutil.disk_usage(work).free / (1 << 30)
    if free_gib < MIN_FREE_GIB:
        die(f"{free_gib:.1f} GiB free on the filesystem holding {work}; "
            f"the LLVM source and its tablegen build need about {MIN_FREE_GIB} GiB")

    tarball = work / Path(url).name
    if not tarball.is_file() or sha256_of(tarball) != sha:
        print(f"PIN-01: fetching {url}")
        rc = subprocess.run(
            ["curl", "-sSL", "--retry", "3", "-o", str(tarball), url]).returncode
        if rc != 0:
            die(f"could not download {url}")
    got = sha256_of(tarball)
    if got != sha:
        die(f"{tarball.name} hashes {got}, cmake/deps.cmake says {sha}")
    print(f"PIN-01: {tarball.name} matches the pinned SHA256")

    # The release tarball's top directory is its own name without .tar.xz.
    top = tarball.name[:-len(".tar.xz")] if tarball.name.endswith(".tar.xz") \
        else tarball.name.split(".tar")[0]
    src.mkdir(parents=True, exist_ok=True)
    members = [f"{top}/{d}" for d in ("llvm", "cmake", "libc", "third-party")]
    rc = subprocess.run(
        ["tar", "-xJf", str(tarball), "--strip-components=1", *members],
        cwd=src).returncode
    if rc != 0:
        die(f"could not extract {members} from {tarball.name}")
    return src


def build_headers(work, src, jobs):
    """Configure LLVM and build only its tablegen-generated headers."""
    build = work / "llvm-build"
    generated = [
        build / "include" / "llvm" / "IR" / "IntrinsicEnums.inc",
        build / "include" / "llvm" / "Analysis" / "TargetLibraryInfo.inc",
        build / "include" / "llvm" / "Config" / "llvm-config.h",
    ]
    if all(p.is_file() for p in generated):
        print(f"PIN-01: generated headers already in {build}")
        return build

    if not (build / "build.ninja").is_file():
        args = [
            "cmake", "-S", str(src / "llvm"), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_STANDARD=17",
            "-DLLVM_TARGETS_TO_BUILD=X86",
            "-DLLVM_ENABLE_RTTI=ON", "-DLLVM_ENABLE_EH=ON",
            "-DLLVM_ENABLE_ASSERTIONS=YES", "-DLLVM_ENABLE_WARNINGS=NO",
            "-DLLVM_ENABLE_Z3_SOLVER=OFF", "-DLLVM_ENABLE_LIBXML2=OFF",
            "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_ENABLE_BINDINGS=OFF",
            "-DLLVM_ENABLE_FFI=OFF",
            "-DLLVM_INCLUDE_RUNTIMES=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF",
            "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF",
            "-DLLVM_INCLUDE_DOCS=OFF",
            "-DLLVM_BUILD_TOOLS=OFF", "-DLLVM_BUILD_RUNTIMES=OFF",
            "-DLLVM_BUILD_EXAMPLES=OFF", "-DLLVM_BUILD_TESTS=OFF",
            "-DLLVM_BUILD_BENCHMARKS=OFF", "-DLLVM_BUILD_DOCS=OFF",
            "-DLLVM_ENABLE_ZLIB=ON", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        ]
        log = work / "llvm-configure.log"
        with open(log, "w") as fh:
            rc = subprocess.run(args, stdout=fh, stderr=subprocess.STDOUT).returncode
        if rc != 0:
            sys.stderr.write((work / "llvm-configure.log").read_text()[-3000:])
            die(f"configuring the pinned LLVM failed; see {log}")

    print(f"PIN-01: building the tablegen-generated headers with {jobs} job(s)")
    log = work / "llvm-headers.log"
    with open(log, "w") as fh:
        rc = subprocess.run(
            ["ninja", "-j", str(jobs), "intrinsics_gen",
             "include/llvm/Analysis/TargetLibraryInfo.inc"],
            cwd=build, stdout=fh, stderr=subprocess.STDOUT).returncode
    if rc != 0:
        sys.stderr.write(log.read_text()[-3000:])
        die(f"could not build the pinned LLVM's generated headers; see {log}")
    missing = [str(p) for p in generated if not p.is_file()]
    if missing:
        die(f"the header build reported success and did not produce {missing[0]}")
    return build


def substitutions(build_dir, llvm_src, llvm_build, capstone_prefix, capstone_arch):
    """Where the configured tree says its dependencies are -> where they are here."""
    b = str(build_dir)
    subs = {
        f"{b}/external/src/llvm-project/llvm/include": str(llvm_src / "llvm" / "include"),
        f"{b}/deps/install/llvm/include": str(llvm_build / "include"),
        f"{b}/deps/install/capstone/include": str(Path(capstone_prefix) / "include"),
    }
    if capstone_arch:
        subs[f"{b}/external/src/capstone-project/arch"] = str(capstone_arch)
    return subs


def configured_source_root(db_path):
    """The checkout the database was configured for, per its CMakeCache.txt.

    check_push_gates.sh runs the gates against a pristine `git worktree` of
    HEAD, which has no configured build directory of its own, so the database
    it can reach belongs to a DIFFERENT checkout. Using it as-is would compile
    that other checkout's sources and report on the wrong tree -- a harness
    measuring something other than what it says it measures, which is the
    failure mode this whole audit keeps finding. Rebase it instead.
    """
    cache = Path(db_path).parent / "CMakeCache.txt"
    if not cache.is_file():
        return None
    m = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$",
                  cache.read_text(encoding="utf-8", errors="replace"), re.M)
    return m.group(1).strip() if m else None


def load_jobs(db_path, subs, rebase=None):
    db = json.loads(Path(db_path).read_text(encoding="utf-8"))
    jobs, skipped, moved, cxx = [], [], 0, 0
    for entry in db:
        if not entry["file"].endswith((".cpp", ".cc", ".cxx")):
            continue                              # C sources of vendored deps
        # Counted before anything can drop this entry. Counting it after the
        # rebase check put the dropped files in `moved` and not in `cxx`, and
        # the accounting below then reported four more entries accounted for
        # than the database held. The invariant caught it, which is what it is
        # there for.
        cxx += 1
        cmd = entry.get("command")
        if cmd is None:
            cmd = " ".join(shlex.quote(a) for a in entry.get("arguments", []))
        if rebase:
            src_root, here = rebase
            cmd = cmd.replace(src_root, here)
            entry = dict(entry,
                         file=entry["file"].replace(src_root, here),
                         directory=entry.get("directory", here).replace(src_root, here))
            if not os.path.isfile(entry["file"]):
                moved += 1
                continue
        for old, new in subs.items():
            cmd = cmd.replace(old, new)
        missing = [p for p in re.findall(r"-(?:isystem|I)\s*(\S+)", cmd)
                   if not os.path.isdir(p)]
        if missing:
            skipped.append((entry["file"], missing[0]))
            continue
        argv, i = [], 0
        parts = shlex.split(cmd)
        while i < len(parts):
            if parts[i] == "-o":
                i += 2
                continue
            if parts[i] == "-c":
                i += 1
                continue
            argv.append(parts[i])
            i += 1
        argv.insert(1, "-fsyntax-only")
        # A braced initialiser that narrows is a warning to gcc and clang and a
        # hard error to MSVC ("error C2398: ... requires a narrowing
        # conversion"). One in src/capstone2llvmir/x86/x86_sse.cpp cost a
        # 35-minute Windows round. Promoting it here turns that into seconds.
        argv.insert(2, "-Werror=narrowing")
        # Every path in the command is absolute and the -o is gone, so the
        # working directory only has to exist. A rebased database names one
        # inside a build tree this checkout does not have.
        cwd = entry.get("directory", str(ROOT))
        if not os.path.isdir(cwd):
            cwd = str(ROOT)
        jobs.append((entry["file"], argv, cwd, None))
    return jobs, skipped, moved, cxx


def cache_floor(llvm_build):
    """Nothing cached before this instant is trusted.

    A syntax check depends on every header its translation unit reaches, so a
    stamp keyed on the .cpp alone would go on saying "fine" after a header
    changed under it. The floor is the newest of: this script, the pinned
    LLVM's generated headers, and every header in the tree. Touch one header
    and the whole cache is cold, which is the safe direction.
    """
    newest = os.path.getmtime(__file__)
    gen = llvm_build / "include" / "llvm" / "IR" / "IntrinsicEnums.inc"
    if gen.is_file():
        newest = max(newest, os.path.getmtime(gen))
    for base in ("include", "src"):
        for root, _dirs, files in os.walk(ROOT / base):
            for name in files:
                if name.endswith((".h", ".hpp", ".inc", ".def")):
                    newest = max(newest, os.path.getmtime(os.path.join(root, name)))
    return newest


def run_one(job):
    path, argv, cwd, stamp = job
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    if p.returncode == 0 and stamp is not None:
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.touch()
    return path, p.returncode, p.stderr


def self_test(llvm_src, llvm_build):
    """The instrument has to fail on the defect it exists to catch."""
    probe = textwrap.dedent("""
        #include <llvm/IR/IRBuilder.h>
        #include <llvm/IR/Intrinsics.h>
        llvm::CallInst* f(llvm::IRBuilder<>& irb, llvm::Value* v)
        {
            llvm::CallInst* c = irb.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, v);
            return c;
        }
        """)
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "probe.cpp"
        src.write_text(probe)
        argv = ["g++", "-std=c++17", "-fsyntax-only", "-w",
                "-isystem", str(llvm_src / "llvm" / "include"),
                "-isystem", str(llvm_build / "include"), str(src)]
        p = subprocess.run(argv, capture_output=True, text=True)
        if p.returncode == 0:
            die("the self-test probe assigns CreateUnaryIntrinsic's result to a "
                "CallInst* and compiled anyway, so this check cannot see the "
                "defect that broke the build")
        if "invalid conversion" not in p.stderr:
            die("the self-test probe failed for a reason other than the "
                "conversion it pins:\n" + p.stderr[:800])
    print("PIN-01: self-test OK -- CallInst* from CreateUnaryIntrinsic is rejected")


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--workdir", default=os.environ.get(
        "PIN_WORKDIR", str(ROOT / "build" / "pin-cache")))
    ap.add_argument("--compile-db", default=os.environ.get(
        "PIN_COMPILE_DB", str(ROOT / "build" / "linux" / "compile_commands.json")))
    ap.add_argument("--capstone-prefix", default=os.environ.get(
        "C2L_DEPS_DIR", "/tmp/c2l-deps") + "/capstone-install")
    ap.add_argument("--capstone-arch", default="")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)

    url, sha = read_pin()
    print(f"PIN-01: cmake/deps.cmake pins {Path(url).name}")
    llvm_src = fetch_and_extract(work, url, sha)
    llvm_build = build_headers(work, llvm_src, args.jobs)

    if args.self_test:
        self_test(llvm_src, llvm_build)
        return 0

    db = Path(args.compile_db)
    if not db.is_file():
        die(f"{db} is not there. Configure the project first -- "
            f"cmake -S . -B build/linux -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -- "
            f"or pass --compile-db. Configuring does not build the dependencies.")

    cap = Path(args.capstone_prefix)
    if not (cap / "include" / "capstone" / "capstone.h").is_file():
        die(f"no Capstone headers under {cap}; run "
            f"scripts/ci/check_capstone2llvmir_tests.sh once, or pass "
            f"--capstone-prefix")
    arch = args.capstone_arch
    if not arch:
        guess = sorted((cap.parent).glob("capstone-*/arch"))
        arch = str(guess[0]) if guess else ""

    src_root = configured_source_root(db)
    rebase = None
    build_dir = db.parent
    if src_root and os.path.realpath(src_root) != os.path.realpath(ROOT):
        rebase = (src_root, str(ROOT))
        build_dir = Path(str(db.parent).replace(src_root, str(ROOT)))
        print(f"PIN-01: the database was configured for {src_root}; reading it "
              f"against {ROOT}")
    jobs, skipped, moved, cxx = load_jobs(db, subs=substitutions(
        build_dir, llvm_src, llvm_build, cap, arch), rebase=rebase)
    if moved:
        print(f"PIN-01: {moved} source file(s) in the database do not exist "
              f"here and were dropped")

    for path, missing in skipped:
        if "/llvm" in missing:
            die(f"{path} was skipped because {missing} does not exist; the "
                f"substitution for the pinned LLVM did not take")

    stamps = work / "stamps"
    floor = cache_floor(llvm_build)
    todo, cached = [], 0
    for path, argv, cwd, _ in jobs:
        stamp = stamps / (os.path.relpath(path, ROOT).replace("/", "_") + ".ok")
        if stamp.is_file() and stamp.stat().st_mtime > max(floor, os.path.getmtime(path)):
            cached += 1
            continue
        todo.append((path, argv, cwd, stamp))

    print(f"PIN-01: {len(jobs)} translation units against the pinned LLVM, "
          f"{cached} already checked since the last change, {len(todo)} to run "
          f"with {args.jobs} job(s)")
    failures, fetched = [], []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for path, rc, err in ex.map(run_one, todo):
            if rc == 0:
                continue
            m = re.search(r"fatal error: (\S+): No such file or directory", err)
            if m and any(m.group(1).startswith(p) for p in FETCHED_HEADERS):
                fetched.append((path, m.group(1)))
            else:
                failures.append((path, err))

    for path, header in sorted(fetched):
        print(f"  skipped {os.path.relpath(path, ROOT)}: needs {header}, which "
              f"the build's download step writes and this check does not run")
    by_dir = {}
    for path, missing in skipped:
        by_dir.setdefault(missing, []).append(path)
    for missing, paths in sorted(by_dir.items()):
        print(f"  skipped {len(paths)} translation unit(s): "
              f"{os.path.relpath(missing, ROOT)} does not exist, so this build "
              f"directory has not built that dependency")

    checked = len(jobs) - len(fetched)
    # Every C++ entry in the database is in exactly one of these four. A
    # harness that reports on part of what it was handed and does not say which
    # part is how an audit comes to believe it covered a tree it did not.
    accounted = checked + len(fetched) + len(skipped) + moved
    if accounted != cxx:
        die(f"{cxx} C++ entries in the database and {accounted} accounted for "
            f"({checked} checked, {len(fetched)} waiting on a dependency "
            f"header, {len(skipped)} waiting on a dependency include "
            f"directory, {moved} not in this checkout)")
    print(f"PIN-01: {cxx} C++ entries -- {checked} checked, {len(fetched)} "
          f"waiting on a dependency header, {len(skipped)} waiting on a "
          f"dependency include directory, {moved} not in this checkout")
    if failures:
        for path, err in sorted(failures)[:10]:
            print(f"--- {os.path.relpath(path, ROOT)}", file=sys.stderr)
            for line in err.splitlines():
                if "error:" in line:
                    print(f"    {line}", file=sys.stderr)
        die(f"{len(failures)} of {checked} translation units do not compile "
            f"against the LLVM cmake/deps.cmake pins")
    if checked < MIN_CHECKED:
        die(f"only {checked} translation units were checked; there were "
            f"{MIN_CHECKED}+ when this was written, so something is being "
            f"skipped that should not be")
    print(f"PIN-01: OK {checked} translation units compile against "
          f"{Path(url).name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
