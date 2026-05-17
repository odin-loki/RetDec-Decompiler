#!/usr/bin/env bash
F=/tmp/benchmark_out/benchmark_cpp_O1.c

echo "=== demangled names containing key patterns ==="
grep "// Demangled:" "$F" | grep -iE 'quicksort|heap|insert|is_even|is_odd|ackermann|accumulate|transform|for_each|find|partition|fibonacci' | head -20

echo ""
echo "=== vtable struct info ==="
grep -A 2 "struct vtable" "$F" | head -20

echo ""
echo "=== class method names ==="
grep "// Demangled:" "$F" | grep -E 'Circle|Rectangle|Triangle|Shape|Animal|Dog|Cat|Bird|Vec2|Buffer' | head -15

echo ""
echo "=== STL algorithm wrappers ==="
grep "// Demangled:" "$F" | grep -E 'stl_|accumulate|transform|for_each|find|partition' | head -15

echo ""
echo "=== all Demangled lines count ==="
grep -c "// Demangled:" "$F" || echo 0

echo ""
echo "=== exception/RTTI refs ==="
grep -cE "__cxa_throw|__cxa_begin_catch|__dynamic_cast|type_info" "$F" || echo 0
