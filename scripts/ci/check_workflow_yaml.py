#!/usr/bin/env python3
"""WF-01 -- every GitHub Actions workflow in this repository has to parse.

Why this exists
---------------
A workflow file that GitHub cannot parse does not fail the run: there is no
run.  `workflow_dispatch` comes back with

    failed to parse workflow: (Line: 285, Col: 11):
    'if-no-files-found' is already defined

and every scheduled and push-triggered run of that file silently stops
happening until somebody notices.  Nothing in this repository looked at these
files, so a duplicate key reached the branch and was found only by trying to
dispatch the run it had broken.

It reached the branch *through* a YAML check, which is the part worth
recording: `yaml.safe_load()` accepts duplicate keys and keeps the last one,
so the file loaded cleanly here and was rejected there.  A checker that is
more permissive than the thing it stands in for is not a checker.  This one
rejects duplicates explicitly, at every level of the document.

What it checks
--------------
1. The file parses as YAML at all.
2. No mapping anywhere in it defines the same key twice.
3. The three keys GitHub requires of a workflow are present: a name for the
   file's own sake, `on`, and at least one job.
4. Every job has `runs-on` and at least one step, and every step has exactly
   one of `uses` or `run`.

Usage:
  python3 scripts/ci/check_workflow_yaml.py [--self-test]
"""

import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github/workflows"


class DuplicateKeyError(yaml.YAMLError):
    pass


class StrictLoader(yaml.SafeLoader):
    """SafeLoader that refuses a mapping with a repeated key."""


def _no_duplicate_keys(loader, node, deep=False):
    seen = {}
    for key_node, _ in node.value:
        key = loader.construct_object(key_node, deep=deep)
        try:
            hashable = key in seen
        except TypeError:  # unhashable key: YAML allows it, GitHub does not
            raise DuplicateKeyError(
                f"line {key_node.start_mark.line + 1}: key {key!r} is not a scalar")
        if hashable:
            raise DuplicateKeyError(
                f"line {key_node.start_mark.line + 1}: '{key}' is already defined "
                f"(first at line {seen[key]})")
        seen[key] = key_node.start_mark.line + 1
    return yaml.SafeLoader.construct_mapping(loader, node, deep)


StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _no_duplicate_keys)


def check_text(text: str, label: str) -> list[str]:
    """Problems with one workflow document, as a list of messages."""
    try:
        doc = yaml.load(text, StrictLoader)
    except yaml.YAMLError as e:
        return [f"{label}: {e}"]

    if not isinstance(doc, dict):
        return [f"{label}: the document is not a mapping"]

    problems = []
    # PyYAML resolves the bare word `on` to the boolean True, which is the
    # YAML 1.1 rule GitHub does not follow. Accept either spelling.
    if "on" not in doc and True not in doc:
        problems.append(f"{label}: no 'on:' trigger")
    if "name" not in doc:
        problems.append(f"{label}: no 'name:'")

    jobs = doc.get("jobs")
    if not isinstance(jobs, dict) or not jobs:
        problems.append(f"{label}: no jobs")
        return problems

    for job_name, job in jobs.items():
        if not isinstance(job, dict):
            problems.append(f"{label}: job '{job_name}' is not a mapping")
            continue
        if "uses" in job:
            continue  # a reusable-workflow call has no runs-on or steps
        if "runs-on" not in job:
            problems.append(f"{label}: job '{job_name}' has no 'runs-on'")
        steps = job.get("steps")
        if not isinstance(steps, list) or not steps:
            problems.append(f"{label}: job '{job_name}' has no steps")
            continue
        for i, step in enumerate(steps):
            if not isinstance(step, dict):
                problems.append(f"{label}: job '{job_name}' step {i} is not a mapping")
                continue
            has = ("uses" in step) + ("run" in step)
            if has != 1:
                what = "both 'uses' and 'run'" if has == 2 else "neither 'uses' nor 'run'"
                named = step.get("name", f"#{i}")
                problems.append(
                    f"{label}: job '{job_name}' step '{named}' has {what}")
    return problems


def self_test() -> int:
    fails = 0

    def case(desc, text, want_problem):
        nonlocal fails
        got = check_text(text, "t.yml")
        if want_problem and not got:
            print(f"self-test: {desc}: expected a problem, got none", file=sys.stderr)
            fails += 1
        elif not want_problem and got:
            print(f"self-test: {desc}: expected no problem, got {got}", file=sys.stderr)
            fails += 1
        elif want_problem and want_problem is not True:
            if not any(want_problem in g for g in got):
                print(f"self-test: {desc}: expected {want_problem!r} in {got}",
                      file=sys.stderr)
                fails += 1

    good = """
name: t
on: [push]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: go
        run: echo hi
"""
    case("a well-formed workflow", good, False)

    # The exact shape that reached the branch: a repeated key inside a
    # step's `with:`. safe_load takes the last one and says nothing.
    dup = good.replace("        run: echo hi", """        run: echo hi
      - uses: actions/upload-artifact@v4
        with:
          name: a
          if-no-files-found: ignore
          if-no-files-found: warn""")
    case("a duplicate key inside with:", dup, "'if-no-files-found' is already defined")
    assert yaml.safe_load(dup), "safe_load is supposed to accept this"

    case("a duplicate job name",
         good + "\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - run: true\n",
         "'build' is already defined")
    case("a duplicate top-level key", good + "\nname: again\n",
         "'name' is already defined")
    case("malformed YAML", "name: t\n  bad indent: [\n", True)
    case("no jobs", "name: t\non: [push]\n", "no jobs")
    case("a job with no runs-on",
         "name: t\non: [push]\njobs:\n  b:\n    steps:\n      - run: true\n",
         "no 'runs-on'")
    case("a job with no steps",
         "name: t\non: [push]\njobs:\n  b:\n    runs-on: ubuntu-latest\n",
         "no steps")
    case("a step with neither uses nor run",
         "name: t\non: [push]\njobs:\n  b:\n    runs-on: ubuntu-latest\n"
         "    steps:\n      - name: nothing\n",
         "neither 'uses' nor 'run'")
    case("a step with both",
         "name: t\non: [push]\njobs:\n  b:\n    runs-on: ubuntu-latest\n"
         "    steps:\n      - uses: a/b@v1\n        run: true\n",
         "both 'uses' and 'run'")
    case("a reusable-workflow call needs no runs-on",
         "name: t\non: [push]\njobs:\n  b:\n    uses: ./.github/workflows/x.yml\n",
         False)

    if fails:
        print(f"check_workflow_yaml self-test: {fails} case(s) failed", file=sys.stderr)
        return 1
    print("check_workflow_yaml self-test OK")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()

    files = sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))
    if not files:
        print(f"WF-01: FAIL no workflows under {WORKFLOW_DIR.relative_to(ROOT)}",
              file=sys.stderr)
        return 1

    problems = []
    for f in files:
        problems += check_text(f.read_text(errors="ignore"),
                               str(f.relative_to(ROOT)))

    if problems:
        print("WF-01: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    print(f"WF-01: OK {len(files)} workflow(s) parse, no duplicate keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
