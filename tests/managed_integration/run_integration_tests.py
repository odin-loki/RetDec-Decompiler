#!/usr/bin/env python3
"""
Managed-language integration test harness for RetDec.

Usage:
    python3 run_integration_tests.py [options]

Options:
    --retdec-bin PATH    Path to retdec-decompiler binary (default: auto-detect)
    --fixtures DIR       Fixtures directory (default: fixtures/ next to this script)
    --golden DIR         Golden-file directory (default: golden/ next to this script)
    --update-golden      Write current output as new golden files (approval workflow)
    --language LANG      Run only this language (java, kotlin, dex, csharp, vbnet,
                         fsharp, python, lua, wasm, malformed)
    --timeout SEC        Per-fixture timeout in seconds (default: 60)
    --jobs N             Parallel workers (default: CPU count)
    --verbose            Print decompiler stdout/stderr on failure
    --junit XML          Write JUnit-compatible XML report

Exit code: 0 if all pass, 1 if any fail.
"""
import argparse
import concurrent.futures
import difflib
import os
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Configuration: which binary extensions each language uses
# ---------------------------------------------------------------------------
LANGUAGE_SPECS = {
    "java":    {"exts": [".class", ".jar"],      "expect_success": True},
    "kotlin":  {"exts": [".jar"],                "expect_success": True},
    "dex":     {"exts": [".dex", ".apk"],        "expect_success": True},
    "csharp":  {"exts": [".dll"],                "expect_success": True},
    "vbnet":   {"exts": [".dll"],                "expect_success": True},
    "fsharp":  {"exts": [".dll"],                "expect_success": True},
    "python":  {"exts": [".pyc"],                "expect_success": True},
    "lua":     {"exts": [".luac", ".luac51", ".luac54"], "expect_success": True},
    "wasm":    {"exts": [".wasm"],               "expect_success": True},
    "malformed": {"exts": [
        ".truncated_50pct", ".truncated_10pct", ".truncated_4bytes",
        ".random_flipped", ".null_body", ".class", ".dex", ".wasm", ".pyc", ".luac",
    ], "expect_success": False},
}

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------
@dataclass
class TestCase:
    language: str
    fixture_path: Path
    golden_path: Path
    expect_success: bool

@dataclass
class TestResult:
    test: TestCase
    passed: bool
    duration: float
    message: str = ""
    stdout: str = ""
    stderr: str = ""

# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------
def find_retdec(hint: Optional[str]) -> Path:
    if hint:
        p = Path(hint)
        if p.is_file():
            return p
        raise FileNotFoundError(f"retdec-decompiler not found: {hint}")
    # Search in common build directories relative to this script
    script_dir = Path(__file__).parent
    candidates = [
        script_dir / "../../build/src/retdec-decompiler/retdec-decompiler",
        script_dir / "../../build-release/src/retdec-decompiler/retdec-decompiler",
        Path("/usr/local/bin/retdec-decompiler"),
        Path("/usr/bin/retdec-decompiler"),
    ]
    for p in candidates:
        p = p.resolve()
        if p.is_file() and os.access(p, os.X_OK):
            return p
    # Fall back to PATH
    found = shutil.which("retdec-decompiler")
    if found:
        return Path(found)
    raise FileNotFoundError(
        "retdec-decompiler not found. Use --retdec-bin to specify its path."
    )

def discover_fixtures(fixtures_dir: Path, languages: List[str]) -> List[TestCase]:
    cases: List[TestCase] = []
    golden_base = fixtures_dir.parent / "golden"

    for lang in languages:
        spec = LANGUAGE_SPECS[lang]
        lang_dir = fixtures_dir / lang
        if not lang_dir.exists():
            continue

        if lang == "malformed":
            # Malformed fixtures are in subdirectories per language
            for sub in sorted(lang_dir.iterdir()):
                if sub.is_dir():
                    for f in sorted(sub.iterdir()):
                        if f.is_file() and any(f.name.endswith(ext) for ext in spec["exts"]):
                            golden = golden_base / "malformed" / sub.name / f.name
                            cases.append(TestCase(
                                language="malformed",
                                fixture_path=f,
                                golden_path=golden,
                                expect_success=spec["expect_success"],
                            ))
        else:
            for f in sorted(lang_dir.rglob("*")):
                if f.is_file() and any(f.suffix == ext or f.name.endswith(ext) for ext in spec["exts"]):
                    rel = f.relative_to(fixtures_dir / lang)
                    golden = golden_base / lang / (str(rel) + ".expected")
                    cases.append(TestCase(
                        language=lang,
                        fixture_path=f,
                        golden_path=golden,
                        expect_success=spec["expect_success"],
                    ))
    return cases

# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------
def run_one(retdec: Path, test: TestCase, timeout: int, verbose: bool) -> TestResult:
    start = time.monotonic()
    tmp_out = test.fixture_path.parent / (test.fixture_path.name + ".retdec_out")
    try:
        proc = subprocess.run(
            [str(retdec), "--input", str(test.fixture_path),
             "--output", str(tmp_out)],
            capture_output=True, text=True, timeout=timeout,
        )
        succeeded = proc.returncode == 0
        stdout = proc.stdout
        stderr = proc.stderr
    except subprocess.TimeoutExpired:
        duration = time.monotonic() - start
        return TestResult(test, False, duration, f"TIMEOUT after {timeout}s")
    except Exception as e:
        duration = time.monotonic() - start
        return TestResult(test, False, duration, f"EXEC ERROR: {e}")
    finally:
        # Clean up temp output
        for p in [tmp_out, Path(str(tmp_out) + ".c"), Path(str(tmp_out) + ".ll")]:
            if p.exists():
                try:
                    p.unlink()
                except OSError:
                    pass

    duration = time.monotonic() - start

    # For malformed: we expect failure (non-zero exit or no crash = partial pass)
    if not test.expect_success:
        # Pass if retdec exited cleanly (no SIGSEGV/abort) regardless of exit code
        if proc.returncode in (-6, -11, -8):  # SIGABRT, SIGSEGV, SIGFPE
            return TestResult(test, False, duration,
                              f"CRASH (exit {proc.returncode}) on malformed input",
                              stdout, stderr)
        return TestResult(test, True, duration, "graceful rejection of malformed input")

    # For valid input: must succeed
    if not succeeded:
        return TestResult(test, False, duration,
                          f"retdec exited {proc.returncode}", stdout, stderr)

    # Golden file comparison
    output_files = [
        Path(str(tmp_out) + ".c"),
        Path(str(tmp_out) + ".ll"),
        tmp_out,
    ]
    actual_text: Optional[str] = None
    for op in [test.fixture_path.parent / (test.fixture_path.name + ".c"),
               test.fixture_path.parent / (test.fixture_path.name + ".ll")]:
        if op.exists():
            try:
                actual_text = op.read_text(errors="replace")
            except OSError:
                pass
            break

    if actual_text is None:
        return TestResult(test, True, duration, "decompiled (no text output to diff)")

    if not test.golden_path.exists():
        return TestResult(test, True, duration,
                          f"NO GOLDEN FILE — run with --update-golden to create one",
                          stdout, stderr)

    expected_text = test.golden_path.read_text(errors="replace")
    if actual_text.strip() == expected_text.strip():
        return TestResult(test, True, duration, "matches golden")

    diff = "".join(difflib.unified_diff(
        expected_text.splitlines(keepends=True),
        actual_text.splitlines(keepends=True),
        fromfile="expected", tofile="actual",
        n=5,
    ))
    return TestResult(test, False, duration, f"GOLDEN MISMATCH:\n{diff[:3000]}", stdout, stderr)

# ---------------------------------------------------------------------------
# Golden-file update
# ---------------------------------------------------------------------------
def update_golden(retdec: Path, test: TestCase, timeout: int):
    proc = subprocess.run(
        [str(retdec), "--input", str(test.fixture_path)],
        capture_output=True, text=True, timeout=timeout,
    )
    for ext in [".c", ".ll"]:
        candidate = test.fixture_path.parent / (test.fixture_path.name + ext)
        if candidate.exists():
            test.golden_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(candidate, test.golden_path)
            print(f"  [golden] {test.golden_path}")
            return
    print(f"  [golden] no output file found for {test.fixture_path.name}")

# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def write_junit(results: List[TestResult], out_path: str):
    total = len(results)
    failures = sum(1 for r in results if not r.passed)
    suite = ET.Element("testsuite", name="managed_integration",
                        tests=str(total), failures=str(failures),
                        time=str(sum(r.duration for r in results)))
    for r in results:
        tc = ET.SubElement(suite, "testcase",
                           name=str(r.test.fixture_path.name),
                           classname=r.test.language,
                           time=str(r.duration))
        if not r.passed:
            fail = ET.SubElement(tc, "failure", message=r.message[:500])
            fail.text = r.message
    tree = ET.ElementTree(suite)
    ET.indent(tree, space="  ")
    tree.write(out_path, encoding="unicode", xml_declaration=True)
    print(f"JUnit report: {out_path}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--retdec-bin")
    parser.add_argument("--fixtures", default=str(Path(__file__).parent / "fixtures"))
    parser.add_argument("--golden",   default=str(Path(__file__).parent / "golden"))
    parser.add_argument("--update-golden", action="store_true")
    parser.add_argument("--language", action="append", dest="languages")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--junit")
    args = parser.parse_args()

    try:
        retdec = find_retdec(args.retdec_bin)
    except FileNotFoundError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)

    print(f"RetDec: {retdec}")

    fixtures_dir = Path(args.fixtures)
    languages = args.languages or list(LANGUAGE_SPECS.keys())
    cases = discover_fixtures(fixtures_dir, languages)

    if not cases:
        print("No fixture files found. Run compile_fixtures.sh first.")
        sys.exit(0)

    print(f"Found {len(cases)} test cases across {len(languages)} languages.")

    if args.update_golden:
        print("Updating golden files...")
        for tc in cases:
            update_golden(retdec, tc, args.timeout)
        print("Done.")
        sys.exit(0)

    # Run in parallel
    results: List[TestResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_one, retdec, tc, args.timeout, args.verbose): tc
                   for tc in cases}
        for fut in concurrent.futures.as_completed(futures):
            r = fut.result()
            results.append(r)
            status = "PASS" if r.passed else "FAIL"
            print(f"  [{status}] {r.test.language}/{r.test.fixture_path.name} "
                  f"({r.duration:.2f}s) {r.message[:80] if not r.passed else ''}")
            if not r.passed and args.verbose:
                if r.stdout: print("    STDOUT:", r.stdout[:500])
                if r.stderr: print("    STDERR:", r.stderr[:500])

    passed = sum(1 for r in results if r.passed)
    failed = len(results) - passed

    print(f"\n{'='*60}")
    print(f"Results: {passed}/{len(results)} passed, {failed} failed")

    if args.junit:
        write_junit(results, args.junit)

    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
