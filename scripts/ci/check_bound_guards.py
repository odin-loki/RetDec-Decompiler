#!/usr/bin/env python3
"""BOUND-01 -- every int64 bound that gets +/- 1 must be guarded against the
extreme that overflows.

Why this exists
---------------
`if_to_switch_optimizer.cpp` reconstructs a switch from a nest of compares. To
do that it turns `v > L` into "the first case is L + 1" and `v >= F` into "the
last case below is F - 1". Both are signed arithmetic on a value that came out
of the decompiled program, so both need a guard: `L + 1` is undefined at
INT64_MAX and `F - 1` is undefined at INT64_MIN.

The file has forty of these functions in four mirror families, roughly six
thousand lines of near-identical compare-tree reconstruction. They were written
by copying, and a copy takes the guard with it -- including when the mirror
needs the OTHER extreme. `tryConvertGeWithSixLevelNestedSplitInThen` guards all
six of its bounds against INT64_MAX and then subtracts one from every one of
them; its own three-level sibling guards INT64_MIN, which is the extreme that
matters for a subtraction.

Reading forty near-identical functions is exactly the task a reader does badly
and a differential does well, so this is a differential.

What counts as guarded
----------------------
Either an explicit limit test (`x == numeric_limits<int64_t>::min()` before
`x - 1`), or an ordering constraint that bounds the value away from the extreme:
`if (x <= y) return false;` makes x strictly greater than some int64, so
`x - 1` cannot overflow. Both are accepted, because both are sound.

Usage: python3 scripts/ci/check_bound_guards.py [--self-test]
"""

import os
import re
import sys

ROOTS = [
    'src/llvmir2hll/optimizer/optimizers',
    'src/llvmir2hll/llvm',
]

FUNC = re.compile(r'^[A-Za-z_].*::(\w+)\s*\(')
INT64 = re.compile(r'\bint64_t\s+(\w+)')
ADD = re.compile(r'\b(\w+)\s*\+\s*1\b')
SUB = re.compile(r'\b(\w+)\s*-\s*1\b')
GMAX = re.compile(r'\b(\w+)\s*==\s*std::numeric_limits<int64_t>::max')
GMIN = re.compile(r'\b(\w+)\s*==\s*std::numeric_limits<int64_t>::min')
# `x <= y` or `x < y` in a bail-out makes x strictly greater than an int64.
LT_REL = re.compile(r'\b(\w+)\s*<=?\s*\w+')
GT_REL = re.compile(r'\b(\w+)\s*>=?\s*\w+')


def functions(path):
    """Yield (name, first_line, body_lines) for each top-level definition."""
    lines = open(path, encoding='utf-8').read().split('\n')
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m:
            start, depth, seen, j = i, 0, False, i
            while j < len(lines):
                depth += lines[j].count('{') - lines[j].count('}')
                if '{' in lines[j]:
                    seen = True
                if seen and depth == 0:
                    break
                j += 1
            yield m.group(1), start + 1, lines[start:j + 1]
            i = j
        i += 1


def unguarded(name, line, body):
    """Return the bound arithmetic in this function that nothing bounds."""
    text = '\n'.join(l.split('//')[0] for l in body)
    i64 = set(INT64.findall(text))
    gmax, gmin = set(GMAX.findall(text)), set(GMIN.findall(text))
    strictly_gt = set(LT_REL.findall(text))
    strictly_lt = set(GT_REL.findall(text))
    out = []
    for v in sorted(set(ADD.findall(text))):
        if v in i64 and v not in gmax and v not in strictly_lt:
            out.append((name, line, v, '+ 1', 'max'))
    for v in sorted(set(SUB.findall(text))):
        if v in i64 and v not in gmin and v not in strictly_gt:
            out.append((name, line, v, '- 1', 'min'))
    return out


def scan(roots):
    findings, nfuncs, nfiles = [], 0, 0
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _, names in os.walk(root):
            for n in sorted(names):
                if not n.endswith('.cpp'):
                    continue
                path = os.path.join(dirpath, n)
                nfiles += 1
                for fname, line, body in functions(path):
                    nfuncs += 1
                    for f in unguarded(fname, line, body):
                        findings.append((path,) + f)
    return findings, nfuncs, nfiles


SELF_TEST_SRC = '''
bool C::guarded(ShPtr<IfStmt> s) {
\tint64_t a = 0;
\tif (a == std::numeric_limits<int64_t>::min()) { return false; }
\tif (x != a - 1) { return false; }
\treturn true;
}
bool C::orderedIsAlsoGuarded(ShPtr<IfStmt> s) {
\tint64_t b = 0;
\tif (b <= other) { return false; }
\tif (x != b - 1) { return false; }
\treturn true;
}
bool C::wrongExtreme(ShPtr<IfStmt> s) {
\tint64_t c = 0;
\tif (c == std::numeric_limits<int64_t>::max()) { return false; }
\tif (x != c - 1) { return false; }
\treturn true;
}
'''


def self_test():
    """The checker has to accept both forms of guard and still catch the
    wrong extreme. A differential that answers "all clear" for everything is
    the failure mode to rule out."""
    lines = SELF_TEST_SRC.split('\n')
    found = []
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m:
            start, depth, seen, j = i, 0, False, i
            while j < len(lines):
                depth += lines[j].count('{') - lines[j].count('}')
                if '{' in lines[j]:
                    seen = True
                if seen and depth == 0:
                    break
                j += 1
            found.extend(unguarded(m.group(1), start + 1, lines[start:j + 1]))
            i = j
        i += 1
    names = {f[0] for f in found}
    if names != {'wrongExtreme'}:
        print('BOUND-01: self-test FAILED -- flagged %s, expected exactly '
              '{wrongExtreme}' % (sorted(names) or '{}'), file=sys.stderr)
        return False
    print('BOUND-01: self-test ok (an explicit guard and an ordering guard '
          'both accepted; the wrong extreme caught)')
    return True


def main():
    if '--self-test' in sys.argv and not self_test():
        return 2
    findings, nfuncs, nfiles = scan(ROOTS)
    for path, name, line, var, op, lim in findings:
        print('BOUND-01: %s:%d %s: `%s %s` and nothing bounds %s away from '
              'INT64_%s' % (path, line, name, var, op, var, lim.upper()),
              file=sys.stderr)
    if findings:
        print('BOUND-01: FAIL %d unguarded bound arithmetic site(s)'
              % len(findings), file=sys.stderr)
        return 1
    print('BOUND-01: OK %d function(s) in %d file(s); every int64 bound that '
          'is incremented or decremented is bounded away from the extreme '
          'that would overflow' % (nfuncs, nfiles))
    return 0


if __name__ == '__main__':
    sys.exit(main())
