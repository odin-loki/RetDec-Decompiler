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
#   bash scripts/check_format.sh --all        # every tracked source, whole file
#   bash scripts/check_format.sh --self-test  # does the check still catch things
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

FAILED=0
CHECKED=0

report() {
  local f="$1"; shift
  echo "check_format: needs reformat: $f" >&2
  diff -u "$f" <(clang-format "$@" "$f") >&2 || true
  echo "::error file=$f::needs clang-format"
  FAILED=1
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
if [[ "${1:-}" == "--all" ]]; then
  while IFS= read -r f; do
    [[ -z "$f" || ! -f "$f" ]] && continue
    is_source "$f" || continue
    check_whole "$f"
  done < <(git ls-files include/ src/ tests/)
  echo "check_format: checked ${CHECKED} file(s), whole"
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

BASE="$(base_ref || true)"
# On the default branch the merge base is HEAD itself, which would diff a
# commit against itself and check nothing at all.
if [[ -n "${BASE}" ]] && [[ "$(git rev-parse "${BASE}")" == "$(git rev-parse HEAD)" ]]; then
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
  diff -q "$path" <(clang-format "${RANGES[@]}" "$path") &>/dev/null \
    || report "$path" "${RANGES[@]}"
done < <(git diff --name-status "${BASE}" HEAD -- include/ src/ tests/)

echo "check_format: checked ${CHECKED} file(s) against ${BASE}"

if [[ $FAILED -ne 0 ]]; then
  echo "check_format: reformat the changed lines -- clang-format -i --lines=A:B <file>," \
       "or clang-format -i <file> for a file this change adds" >&2
  exit 1
fi

echo "check_format: OK"
