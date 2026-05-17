#!/usr/bin/env bash
F=/tmp/benchmark_out/benchmark_cpp_O1.c
echo "=== quicksort body ==="
sed -n '/recovered range: quicksort /,/^}/p' "$F" | head -12
echo "=== heapify body ==="
sed -n '/recovered range: heapify /,/^}/p' "$F" | head -12
echo "=== is_even / is_odd ==="
sed -n '/recovered range: is_even /,/^}/p' "$F" | head -10
sed -n '/recovered range: is_odd /,/^}/p' "$F" | head -10
echo "=== exception patterns ==="
grep -E '__cxa_throw|__cxa_begin_catch|__gxx_persona' "$F" | head -5
echo "=== dynamic_cast ==="
grep -E 'dynamic_cast|__dynamic_cast' "$F" | head -5
echo "=== atomic compare_exchange ==="
grep -E 'compare_exchange|fetch_add' "$F" | head -5
echo "=== vtable pointer stores ==="
grep -E '_ZTV|vtable' "$F" | head -5
echo "=== ackermann ==="
sed -n '/recovered range: ackermann /,/^}/p' "$F" | head -12
