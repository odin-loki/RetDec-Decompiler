#!/usr/bin/env bash
F=/tmp/benchmark_out/benchmark_cpp_O1.c
echo "=== is_even/is_odd occurrences ==="
grep -n 'is_even\|is_odd' "$F" | head -10
echo ""
echo "=== ackermann occurrences ==="
grep -n 'ackermann' "$F" | head -10
