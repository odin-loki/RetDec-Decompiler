#!/usr/bin/env python3
"""TESTSEL-01 -- every test this tree registers with ctest is run by some workflow.

Why this exists
---------------
tests/ registers 46 tests with add_test(). Fifteen of them carry a LABELS
property; thirty-one carry none. Every ctest invocation in .github/workflows
filtered on a label:

    ctest --test-dir build/linux -L unit --output-on-failure
    ctest --test-dir build/linux --output-on-failure -L integration

`-L unit` selects six tests. `-L integration` selects six more. The other
thirty-four -- thirty gtest suites holding 2,158 test cases, plus a few
registered twice -- were compiled and linked by CI on every macOS run and then
never executed by anything. The Linux and Windows jobs do not even build them.

That is the same shape as the RETDEC_ENABLE_FILEINFO defect found the same
week: a selector narrower than the thing it is reported as covering. A green
"Unit tests" step that runs 6 of 46 registered tests is not a lie anybody told;
it is a lie the label filter tells on everyone's behalf, every run, for free.

What this checks
----------------
For each test registered with add_test(NAME ...), at least one of:

  * a `ctest` command line in some workflow selects it -- no -L/-R at all
    selects everything, `-L RE` selects it when one of its labels matches RE,
    -LE / -E can take it back out again;
  * a workflow step runs its executable directly by name.

Anything left over is named, with the file that registers it, and the check
fails.

What this does NOT check: that the workflow which selects a test also builds
it, or that the job it lives in is one that runs. Selection is what was broken
and selection is what this measures.

Usage: python3 scripts/ci/check_test_selection.py [--self-test]
"""

from __future__ import annotations

import os
import re
import sys
import shlex
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

ADD_TEST = re.compile(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_.+-]+)", re.IGNORECASE)
SET_PROPS = re.compile(
    r"set_tests_properties\s*\((.*?)\)", re.IGNORECASE | re.DOTALL)
LABELS = re.compile(r"\bLABELS\b\s+(\"[^\"]*\"|[A-Za-z0-9_;.+-]+)", re.IGNORECASE)


def registered_tests(tests_root: Path) -> dict[str, tuple[str, set[str]]]:
    """name -> (CMakeLists path, labels)."""
    found: dict[str, tuple[str, set[str]]] = {}
    labels_of: dict[str, set[str]] = {}
    for f in sorted(tests_root.rglob("CMakeLists.txt")):
        text = f.read_text(encoding="utf-8", errors="replace")
        rel = str(f.relative_to(tests_root.parent))
        for m in ADD_TEST.finditer(text):
            found.setdefault(m.group(1), (rel, set()))
        for m in SET_PROPS.finditer(text):
            body = m.group(1)
            lm = LABELS.search(body)
            if not lm:
                continue
            raw = lm.group(1).strip('"')
            labels = {p for p in re.split(r"[;\s]+", raw) if p}
            # Everything before the PROPERTIES keyword is the test-name list.
            head = re.split(r"\bPROPERTIES\b", body, maxsplit=1,
                            flags=re.IGNORECASE)[0]
            for name in head.split():
                labels_of.setdefault(name, set()).update(labels)
    return {n: (f, labels_of.get(n, set())) for n, (f, _) in found.items()}


class CtestInvocation:
    """The selection a single `ctest ...` command line performs."""

    def __init__(self, argv: list[str]) -> None:
        self.include_label: str | None = None
        self.exclude_label: str | None = None
        self.include_name: str | None = None
        self.exclude_name: str | None = None
        i = 0
        while i < len(argv):
            a = argv[i]
            nxt = argv[i + 1] if i + 1 < len(argv) else None
            if a == "-L" and nxt:
                self.include_label = nxt
                i += 2
                continue
            if a == "-LE" and nxt:
                self.exclude_label = nxt
                i += 2
                continue
            if a == "-R" and nxt:
                self.include_name = nxt
                i += 2
                continue
            if a == "-E" and nxt:
                self.exclude_name = nxt
                i += 2
                continue
            i += 1

    def selects(self, name: str, labels: set[str]) -> bool:
        def search(pattern: str, text: str) -> bool:
            try:
                return re.search(pattern, text) is not None
            except re.error:
                return False

        if self.include_label is not None:
            if not any(search(self.include_label, l) for l in labels):
                return False
        if self.exclude_label is not None:
            if any(search(self.exclude_label, l) for l in labels):
                return False
        if self.include_name is not None and not search(self.include_name, name):
            return False
        if self.exclude_name is not None and search(self.exclude_name, name):
            return False
        return True


def _argv_of(line: str) -> list[str]:
    """The ctest argument list on one line, tolerant of YAML and shell noise."""
    frag = line[line.index("ctest"):]
    # A YAML folded scalar or a shell continuation can leave a trailing
    # backslash; ${{ }} expressions are not shell words.
    frag = frag.rstrip("\\").replace("${{", " ").replace("}}", " ")
    try:
        return shlex.split(frag, posix=True)
    except ValueError:
        return frag.split()


def workflow_selection(workflows: Path):
    """(ctest invocations, executables run directly by name) across workflows."""
    invocations: list[tuple[str, CtestInvocation]] = []
    direct: set[str] = set()
    for f in sorted(list(workflows.glob("*.yml")) + list(workflows.glob("*.yaml"))):
        text = f.read_text(encoding="utf-8", errors="replace")
        for raw in text.splitlines():
            line = raw.strip()
            if line.startswith("#"):
                continue
            # `ctest` as a command, not as part of a cache key or a job name:
            # the token has to END there. `key: ctest-linux` reads as a ctest
            # with no filter otherwise, which selects everything and makes this
            # check pass for a reason that has nothing to do with the tests.
            if re.search(r"(?:^|[\s`'\"(&|])ctest(?=\s)", line):
                argv = _argv_of(line)
                # And a command has flags. `- name: ORPH-01 suites ctest never
                # runs` is a step title, not a run of the whole suite, and
                # counting it as one is exactly the failure mode this check
                # exists to catch -- in the checker instead of the workflow.
                if len(argv) >= 2 and any(a.startswith("-") for a in argv[1:]):
                    invocations.append((f.name, CtestInvocation(argv)))
            # A step that runs a test binary by path, e.g.
            #   run: ./build/linux/tests/gui/retdec-gui-tests
            #   $exe = Resolve-Path ".\build\windows\tests\gui\...exe"
            for m in re.finditer(r"[\\/]tests[\\/][A-Za-z0-9_]+[\\/]([A-Za-z0-9_.+-]+)",
                                 line):
                direct.add(m.group(1).removesuffix(".exe"))
    return invocations, direct


def audit(tests_root: Path, workflows: Path):
    tests = registered_tests(tests_root)
    invocations, direct = workflow_selection(workflows)
    uncovered: list[tuple[str, str]] = []
    for name, (where, labels) in sorted(tests.items()):
        if name in direct:
            continue
        if any(inv.selects(name, labels) for _, inv in invocations):
            continue
        uncovered.append((name, where))
    return tests, invocations, direct, uncovered


def self_test() -> None:
    """A filter that skips a registered test has to be reported as one."""
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        tests = root / "tests" / "widget"
        tests.mkdir(parents=True)
        wf = root / "workflows"
        wf.mkdir()
        (tests / "CMakeLists.txt").write_text(
            'add_test(NAME widget-tests COMMAND widget-tests)\n'
            'add_test(NAME widget_unit COMMAND widget_unit)\n'
            'set_tests_properties(widget_unit PROPERTIES\n'
            '    TIMEOUT 60 LABELS "widget;unit")\n')

        # broke: the label filter every workflow used here
        (wf / "a.yml").write_text(
            "jobs:\n  x:\n    steps:\n"
            "      - run: ctest --test-dir build/linux -L unit --output-on-failure\n")
        _, _, _, uncovered = audit(root / "tests", wf)
        assert [n for n, _ in uncovered] == ["widget-tests"], uncovered

        # fixed: no filter at all selects the whole registered set
        (wf / "a.yml").write_text(
            "jobs:\n  x:\n    steps:\n"
            "      - run: ctest --test-dir build/linux --output-on-failure --timeout 600\n")
        _, _, _, uncovered = audit(root / "tests", wf)
        assert uncovered == [], uncovered

        # also fixed: a step that runs the binary directly counts
        (wf / "a.yml").write_text(
            "jobs:\n  x:\n    steps:\n"
            "      - run: ctest --test-dir build/linux -L unit --output-on-failure\n"
            "      - run: ./build/linux/tests/widget/widget-tests\n")
        _, _, _, uncovered = audit(root / "tests", wf)
        assert uncovered == [], uncovered

        # -LE takes a test back out: a run that excludes every label a test
        # has does not cover it.
        (wf / "a.yml").write_text(
            "jobs:\n  x:\n    steps:\n"
            "      - run: ctest --test-dir build/linux -LE 'unit|widget'\n")
        _, _, _, uncovered = audit(root / "tests", wf)
        assert [n for n, _ in uncovered] == ["widget_unit"], uncovered
    print("TESTSEL-01: self-test OK")


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        self_test()

    tests, invocations, direct, uncovered = audit(
        ROOT / "tests", ROOT / ".github" / "workflows")

    if not tests:
        print("TESTSEL-01: FAIL no add_test() found under tests/ -- "
              "the parser or the tree moved", file=sys.stderr)
        return 1
    if not invocations:
        print("TESTSEL-01: FAIL no ctest command line found in .github/workflows",
              file=sys.stderr)
        return 1

    if uncovered:
        print(f"TESTSEL-01: FAIL {len(uncovered)} of {len(tests)} registered "
              f"test(s) are selected by no workflow", file=sys.stderr)
        for name, where in uncovered:
            labels = tests[name][1]
            shown = ";".join(sorted(labels)) if labels else "(no labels)"
            print(f"  {name}  [{shown}]  <- {where}", file=sys.stderr)
        print("", file=sys.stderr)
        print("  Either give the test a label some workflow selects, or run a "
              "ctest with no -L so the whole registered set goes.", file=sys.stderr)
        return 1

    print(f"TESTSEL-01: OK {len(tests)} registered test(s), "
          f"{len(invocations)} ctest invocation(s), "
          f"{len(direct)} run directly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
