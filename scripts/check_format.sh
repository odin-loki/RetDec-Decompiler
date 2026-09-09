#!/usr/bin/env bash
# Dry-run clang-format check on C/C++ under include/, src/, tests/.
#
# The tree is not clang-format-clean and cannot be made so: 59 of the 60 files
# under src/llvmir2hll and 53 of 60 under src/bin2llvmir are Avast's, written
# in a brace style .clang-format does not describe, and reformatting vendored
# code destroys the ability to diff it against upstream. So this checks the
# LINES a change touches, not the files it touches -- which is what "so new
# work stays clean" meant. A file the change adds is checked whole, since
# every line of it is new work.
#
# Usage:
#   bash scripts/check_format.sh              # changes since the merge base
#   bash scripts/check_format.sh --base REF   # ...since REF instead
#   bash scripts/check_format.sh --fix        # reformat them instead of reporting
#                                            (several passes; see the loop below)
#   bash scripts/check_format.sh --all        # every tracked source, whole file
#   bash scripts/check_format.sh --self-test  # does the check still catch things
#
# --base exists because "what CI will say" depends on what CI diffs against. On
# a push that is the previously pushed commit, which locally is @{upstream};
# the default here is the merge base with the default branch, which is what a
# pull request diffs and is a different, larger question. Asking the first one
# before pushing is what scripts/check_push_gates.sh does.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v clang-format &>/dev/null; then
  echo "check_format: clang-format not found in PATH" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Self-test: build a throwaway repository whose HEAD~ is misformatted and whose
# HEAD adds one well-formatted line to it, and one that adds a bad line.
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--self-test" ]]; then
  # The cases below deliberately run a check that fails; set -e would take the
  # first one as the end of the self-test.
  set +e
  T="$(mktemp -d)"
  trap 'rm -rf "${T}"' EXIT
  cp "${ROOT}/.clang-format" "${T}/.clang-format"
  cp "${ROOT}/scripts/check_format.sh" "${T}/check_format.sh" 2>/dev/null || true
  mkdir -p "${T}/src" "${T}/scripts"
  cp "${BASH_SOURCE[0]}" "${T}/scripts/check_format.sh"
  cd "${T}"
  git init -q .
  git config user.email t@t; git config user.name t
  # A file in the historical, unformatted style.
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n' > src/legacy.cpp
  git add -A; git commit -qm base

  run_here() { ( cd "${T}" && bash scripts/check_format.sh "$@" >"${T}/out" 2>&1 ); }
  fails=0
  expect() {
    local want="$1" desc="$2"; shift 2
    "$@"; local got=$?
    if [[ "${got}" -ne "${want}" ]]; then
      echo "self-test: ${desc}: expected exit ${want}, got ${got}" >&2
      cat "${T}/out" >&2
      fails=$(( fails + 1 ))
    fi
  }

  # 1. No change at all: the misformatted legacy file must not fail the check.
  expect 0 "an untouched misformatted file is not reported" run_here

  # 2. A well-formatted line added to the legacy file passes.
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n\nint added()\n{\n\treturn 2;\n}\n' > src/legacy.cpp
  git add -A; git commit -qm "well formatted addition"
  expect 0 "a well-formatted line added to a legacy file passes" run_here
  git reset -q --hard HEAD~1

  # 3. A misformatted line added to the legacy file fails.
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n\nint  added( )   {return   2;}\n' > src/legacy.cpp
  git add -A; git commit -qm "badly formatted addition"
  expect 1 "a misformatted line added to a legacy file is reported" run_here
  git reset -q --hard HEAD~1

  # 4. A new file is checked whole, so a misformatted one fails even if the
  #    change only adds a line or two of it.
  printf 'int  brandnew( )   {return   3;}\n' > src/brandnew.cpp
  git add -A; git commit -qm "new file, badly formatted"
  expect 1 "a misformatted new file is reported" run_here
  git reset -q --hard HEAD~1

  # 5. A well-formatted new file passes.
  printf 'int brandnew()\n{\n\treturn 3;\n}\n' > src/brandnew.cpp
  git add -A; git commit -qm "new file, well formatted"
  expect 0 "a well-formatted new file passes" run_here
  git reset -q --hard HEAD~1

  # 6. --base picks the commit to diff against.  A misformatted line committed
  #    two commits ago is not this push's business and must not be reported
  #    against the last one -- which is the whole reason CI on a push diffs
  #    what it pushed rather than the whole branch.
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n\nint  older( )   {return 4;}\n' > src/legacy.cpp
  git add -A; git commit -qm "misformatted, one push ago"
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n\nint  older( )   {return 4;}\n\nint newer()\n{\n\treturn 5;\n}\n' > src/legacy.cpp
  git add -A; git commit -qm "well formatted, this push"
  expect 0 "--base HEAD~1 ignores a misformatted line from an earlier commit" \
    run_here --base HEAD~1
  expect 1 "--base HEAD~2 reports it" run_here --base HEAD~2
  expect 2 "--base rejects something that is not a commit" \
    run_here --base no/such/ref

  # 6b. A vendored file is never formatted whole, however many passes it
  #     takes: reformatting it destroys the ability to diff it against
  #     upstream, which is the whole reason the check is line-scoped.
  printf '/**\n * @copyright (c) 2017 Avast Software, licensed under the MIT license\n */\nint legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n' > src/vendored.cpp
  git add -A; git commit -qm "vendored base"
  printf '/**\n * @copyright (c) 2017 Avast Software, licensed under the MIT license\n */\nint legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n\nint  tangled( )   {int x=1;\n  if(x){return   x;}\n     return 0;}\n' > src/vendored.cpp
  git add -A; git commit -qm "a vendored file with a tangled addition"
  run_here --base HEAD~1 --fix-whole
  if ! grep -q '    if (a) { return 1; }' src/vendored.cpp; then
    echo "self-test: the whole-file pass reformatted a vendored file" >&2
    cat src/vendored.cpp >&2
    fails=$(( fails + 1 ))
  fi
  git checkout -q -- . ; git reset -q --hard HEAD~2

  # 6c. ...and a file this fork owns is, on that pass.
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n' > src/ours.cpp
  git add -A; git commit -qm "our base"
  printf 'int legacy(int a) {\n    if (a) { return 1; }\n    return 0;\n}\n\nint  tangled( )   {int x=1;\n  if(x){return   x;}\n     return 0;}\n' > src/ours.cpp
  git add -A; git commit -qm "our file with a tangled addition"
  run_here --base HEAD~1 --fix-whole
  if grep -q '    if (a) { return 1; }' src/ours.cpp; then
    echo "self-test: the whole-file pass left a fork-owned file line-scoped" >&2
    cat src/ours.cpp >&2
    fails=$(( fails + 1 ))
  fi
  git checkout -q -- . ; git reset -q --hard HEAD~2

  # 7. --fix reformats what the check reports, and the check then passes.
  expect 0 "--fix reformats the reported lines" run_here --base HEAD~2 --fix
  expect 0 "the check passes after --fix" run_here --base HEAD~2
  if ! grep -q 'int older()' src/legacy.cpp; then
    echo "self-test: --fix did not reformat the misformatted line" >&2
    cat src/legacy.cpp >&2
    fails=$(( fails + 1 ))
  fi
  # ...and only those lines: the legacy body above it is untouched.
  if ! grep -q '    if (a) { return 1; }' src/legacy.cpp; then
    echo "self-test: --fix reformatted lines the change did not touch" >&2
    cat src/legacy.cpp >&2
    fails=$(( fails + 1 ))
  fi
  git checkout -q -- src/legacy.cpp
  git reset -q --hard HEAD~2

  # 8. An uncommitted change is what someone running this before a commit
  #    means by "check my work", and it used to be invisible: the file list
  #    came from BASE..HEAD, so a file dirty in the working tree was never
  #    named, and the check quietly examined the previous push's range instead.
  printf 'int pending()\n{\n\treturn 6;\n}\n' > src/pending.cpp
  git add -A; git commit -qm "clean base for the uncommitted case"
  printf 'int pending()\n{\n\treturn 6;\n}\n\nint  uncommitted( )   {return 7;}\n' > src/pending.cpp
  expect 1 "a misformatted uncommitted change is reported" run_here --base HEAD
  git checkout -q -- .
  git reset -q --hard HEAD~1

  cd "${ROOT}"
  if [[ "${fails}" -ne 0 ]]; then
    echo "check_format self-test: ${fails} case(s) failed" >&2
    exit 1
  fi
  echo "check_format self-test OK"
  exit 0
fi

cd "$ROOT"

if ! clang-format --dump-config >/dev/null; then
  echo "check_format: .clang-format is invalid for $(clang-format --version)" >&2
  exit 1
fi

is_source() { [[ "$1" =~ \.(cpp|h|hpp|cc|c|cu)$ ]]; }

MODE=changed
FIX=0
WHOLE_FILE_FIX=0
FORCE_WHOLE_FILE_FIX=0
BASE_OVERRIDE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --all) MODE=all; shift ;;
    --fix) FIX=1; shift ;;
    # The last --fix pass, on its own. Only the self-test uses it: the
    # condition it fires under -- a file the line scoping cannot settle -- is
    # a property of real files that is not worth faking, and the guard it
    # carries, that a vendored file is never reformatted whole, is worth
    # pinning on its own.
    --fix-whole) FIX=1; FORCE_WHOLE_FILE_FIX=1; shift ;;
    --base) BASE_OVERRIDE="${2:-}"; shift 2 ;;
    *) echo "check_format: unknown argument: $1" >&2; exit 2 ;;
  esac
done

TMP_FORMATTED="$(mktemp)"
trap 'rm -f "${TMP_FORMATTED}"' EXIT

FAILED=0
CHECKED=0
FIXED=0

# Is this file vendored from upstream Avast, rather than this fork's own?
#
# It decides what --fix may do to a file the line scoping cannot settle.
# Reformatting vendored code destroys the ability to diff it against upstream,
# which is the whole reason the check is line-scoped; a file this fork wrote
# has no upstream to lose.
is_vendored() {
  head -n 8 "$1" 2>/dev/null | grep -qi 'Avast Software'
}

report() {
  local f="$1"; shift
  if [[ ${FIX} -eq 1 ]]; then
    if [[ ${WHOLE_FILE_FIX} -eq 1 ]] && [[ $# -gt 0 ]] && ! is_vendored "$f"; then
      clang-format -i "$f"
      echo "check_format: reformatted $f (whole file: the line scoping does not settle)"
    else
      clang-format -i "$@" "$f"
      echo "check_format: reformatted $f"
    fi
    FIXED=$((FIXED + 1))
    return
  fi
  echo "check_format: needs reformat: $f" >&2
  diff -u "$f" <(clang-format "$@" "$f") >&2 || true
  echo "::error file=$f::needs clang-format"
  FAILED=1
}

# Does clang-format want to change any of the lines this change touched?
#
# `clang-format --lines=A:B` reformats whole constructs, so it can rewrite
# lines outside A:B when one of them shares a statement with a line inside.
# Counting that as "the change is unformatted" is what makes a line-scoped
# check cascade: the rewritten neighbour is itself a change, so the next range
# is wider, and the range grows until it has eaten the file -- which for a file
# whose style predates .clang-format means demanding the whole-file reformat
# the line scoping exists to avoid. Measured on src/type_seed/itanium_seeder.cpp:
# 114 lines of churn after one pass, 261 after five, still growing.
#
# So the question asked is the narrower one that matters: of the lines this
# change touched, is any formatted differently from what clang-format would
# produce? Hunks lying entirely outside them are clang-format reaching past its
# range, and are not the author's to fix.
touched_lines_differ() {
  local orig="$1" formatted="$2"; shift 2
  python3 - "$orig" "$formatted" "$@" <<'PY'
import subprocess, sys
orig, formatted, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
ranges = []
for spec in specs:
    lo, hi = spec.split("=", 1)[1].split(":")
    ranges.append((int(lo), int(hi)))
out = subprocess.run(
    ["diff", "--unchanged-group-format=", "--old-group-format=%df,%dl\n",
     "--new-group-format=%dF,%dF\n", "--changed-group-format=%df,%dl\n",
     orig, formatted], capture_output=True, text=True).stdout
for line in out.splitlines():
    line = line.strip()
    if not line:
        continue
    lo, hi = (int(x) for x in line.split(","))
    if any(lo <= r_hi and r_lo <= hi for r_lo, r_hi in ranges):
        sys.exit(1)
sys.exit(0)
PY
}

check_whole() {
  local f="$1"
  CHECKED=$((CHECKED + 1))
  if ! clang-format "$f" >/dev/null; then
    echo "check_format: clang-format failed on $f (invalid style or parse error)" >&2
    echo "::error file=$f::clang-format failed"
    FAILED=1
    return
  fi
  diff -q "$f" <(clang-format "$f") &>/dev/null || report "$f"
}

# --all: the historical behaviour, for anyone who wants the whole picture.
if [[ "${MODE}" == "all" ]]; then
  while IFS= read -r f; do
    [[ -z "$f" || ! -f "$f" ]] && continue
    is_source "$f" || continue
    check_whole "$f"
  done < <(git ls-files include/ src/ tests/)
  echo "check_format: checked ${CHECKED} file(s), whole"
  if [[ ${FIX} -eq 1 ]]; then
    echo "check_format: reformatted ${FIXED} file(s); re-run without --fix to confirm"
    exit 0
  fi
  [[ $FAILED -eq 0 ]] || { echo "check_format: run clang-format -i on the files above" >&2; exit 1; }
  echo "check_format: OK"
  exit 0
fi

# The base to diff against.  In Actions that is what the push or PR built on;
# locally it is the merge base with the default branch, or the previous commit.
base_ref() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    if [[ -n "${GITHUB_BASE_REF:-}" ]]; then
      git fetch --depth=50 origin "${GITHUB_BASE_REF}" >/dev/null 2>&1 || true
      git merge-base "origin/${GITHUB_BASE_REF}" HEAD 2>/dev/null && return
    fi
    local before="${GITHUB_EVENT_BEFORE:-}"
    if [[ -n "${before}" && ! "${before}" =~ ^0+$ ]] \
        && git cat-file -e "${before}^{commit}" 2>/dev/null; then
      printf '%s\n' "${before}"
      return
    fi
    git rev-parse HEAD^ 2>/dev/null || true
    return
  fi
  for b in origin/main main origin/master master; do
    if git rev-parse --verify -q "$b" >/dev/null; then
      git merge-base "$b" HEAD 2>/dev/null && return
    fi
  done
  git rev-parse HEAD^ 2>/dev/null || true
}

if [[ -n "${BASE_OVERRIDE}" ]]; then
  if ! BASE="$(git rev-parse --verify -q "${BASE_OVERRIDE}^{commit}")"; then
    echo "check_format: --base ${BASE_OVERRIDE} is not a commit" >&2
    exit 2
  fi
else
  BASE="$(base_ref || true)"
fi
# On the default branch the merge base is HEAD itself, which would diff a
# commit against itself and check nothing at all -- unless the working tree is
# dirty, in which case the uncommitted work *is* the range, and is exactly what
# someone running this before a commit means by it.
if [[ -n "${BASE}" ]] && [[ "$(git rev-parse "${BASE}")" == "$(git rev-parse HEAD)" ]] \
   && git diff --quiet HEAD -- include/ src/ tests/; then
  BASE="$(git rev-parse HEAD^ 2>/dev/null || true)"
fi
if [[ -z "${BASE}" ]]; then
  # Nothing to diff against (a shallow clone with no parent, an initial
  # commit).  Checking the historic tree here would fail on vendored code, so
  # say what happened rather than passing quietly.
  echo "check_format: no base commit to diff against; nothing checked"
  echo "check_format: OK"
  exit 0
fi

# Line ranges of the added/changed lines, per file, from a zero-context diff.
changed_ranges() {
  git diff -U0 "${BASE}" -- "$1" | awk '
    /^@@/ {
      # @@ -a,b +c,d @@   or   @@ -a +c @@
      match($0, /\+[0-9]+(,[0-9]+)?/)
      spec = substr($0, RSTART + 1, RLENGTH - 1)
      split(spec, p, ",")
      start = p[1] + 0
      count = (p[2] == "" ? 1 : p[2] + 0)
      if (count > 0) printf "--lines=%d:%d\n", start, start + count - 1
    }'
}

scan_changed() {
  CHECKED=0
  FAILED=0
  FIXED=0
  while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  status="${line%%$'\t'*}"
  path="${line#*$'\t'}"
  # A rename reports "R100\told\tnew"; the new path is the last field.
  path="${path##*$'\t'}"
  [[ -f "$path" ]] || continue
  is_source "$path" || continue

  if [[ "${status:0:1}" == "A" ]]; then
    check_whole "$path"
    continue
  fi

  mapfile -t RANGES < <(changed_ranges "$path")
  [[ ${#RANGES[@]} -eq 0 ]] && continue
  CHECKED=$((CHECKED + 1))
  if ! clang-format "${RANGES[@]}" "$path" >/dev/null; then
    echo "check_format: clang-format failed on $path (invalid style or parse error)" >&2
    echo "::error file=$path::clang-format failed"
    FAILED=1
    continue
  fi
  clang-format "${RANGES[@]}" "$path" > "${TMP_FORMATTED}"
  touched_lines_differ "$path" "${TMP_FORMATTED}" "${RANGES[@]}" \
    || report "$path" "${RANGES[@]}"
  # BASE against the *working tree*, not against HEAD.
  #
  # changed_ranges() already diffs BASE against the working tree, so listing
  # only the files that changed between BASE and HEAD meant a file edited but
  # not yet committed was never named, and so never checked -- run before the
  # commit, the check silently examined the previous push's range instead. In
  # CI the working tree is the checkout of HEAD, so the two spellings agree
  # there; locally, before committing, only this one looks at the work in hand.
  done < <(git diff --name-status "${BASE}" -- include/ src/ tests/)
}

if [[ ${FIX} -eq 1 ]]; then
  # More than one pass, because the ranges move. `clang-format --lines=A:B`
  # reformats whole constructs, so fixing a line shifts its neighbours, which
  # widens the next diff, which widens the next range. On most files that
  # settles in two or three passes. On a file that mixes tabs and spaces line
  # by line it does not settle at all -- measured at 114, 261 then 672 lines on
  # one, still growing -- and the last pass formats those whole, which is where
  # that walk ends anyway. Only for a file this fork owns: reformatting
  # vendored code destroys the ability to diff it against upstream, which is
  # the whole reason the check is line-scoped.
  total=0
  for pass in 1 2 3 4 5; do
    WHOLE_FILE_FIX=${FORCE_WHOLE_FILE_FIX}
    [[ ${pass} -eq 5 ]] && WHOLE_FILE_FIX=1
    scan_changed
    total=$(( total + FIXED ))
    [[ ${FIXED} -eq 0 ]] && break
  done
  echo "check_format: reformatted ${total} file(s) in ${pass} pass(es); re-run without --fix to confirm"
  exit 0
fi

scan_changed
echo "check_format: checked ${CHECKED} file(s) against ${BASE}"

if [[ $FAILED -ne 0 ]]; then
  echo "check_format: reformat the changed lines -- bash scripts/check_format.sh --fix," \
       "or clang-format -i --lines=A:B <file> by hand" >&2
  exit 1
fi

echo "check_format: OK"
