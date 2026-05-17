#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
SRCDIR="${SRCDIR:-$REPO_ROOT}"
OBJ=$BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir

echo "=== Spot check key files ==="
declare -A files=(
    ["llvmir2hll.cpp"]="llvmir2hll.cpp"
    ["optimizer/optimizer_manager.cpp"]="optimizer/optimizer_manager.cpp"
    ["hll/hll_writers/c_hll_writer.cpp"]="hll/hll_writers/c_hll_writer.cpp"
    ["graphs/cfg/cfg.cpp"]="graphs/cfg/cfg.cpp"
    ["optimizer/optimizers/while_true_to_for_loop_optimizer.cpp"]="optimizer/optimizers/while_true_to_for_loop_optimizer.cpp"
    ["analysis/used_vars_visitor.cpp"]="analysis/used_vars_visitor.cpp"
    ["obtainer/call_info_obtainers/optim_call_info_obtainer.cpp"]="obtainer/call_info_obtainers/optim_call_info_obtainer.cpp"
    ["hll/bir_writer.cpp"]="hll/bir_writer.cpp"
)

cd "$OBJ"
tot_lines=0; exec_lines=0
for rel in "${!files[@]}"; do
    gcda="${rel%.cpp}.cpp.gcda"
    if [ -f "$gcda" ]; then
        result=$(gcov -b "$gcda" 2>&1 | grep "Lines executed" | head -1)
        pct=$(echo "$result" | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1)
        exec=$(echo "$result" | grep -oP 'of \K[0-9]+' | head -1)
        printf "  %-60s %s%% (%s lines)\n" "$rel" "${pct:-0}" "${exec:-?}"
    else
        echo "  $rel: NO GCDA"
    fi
done

# Overall stats via simple gcov pass
echo ""
echo "=== Computing overall llvmir2hll stats ==="
total=0; executed=0; br_total=0; br_exec=0
for gcda in $(find $OBJ -name '*.gcda'); do
    while IFS= read -r line; do
        if [[ "$line" =~ ^Lines\ executed:([0-9.]+)%\ of\ ([0-9]+) ]]; then
            pct="${BASH_REMATCH[1]}"
            n="${BASH_REMATCH[2]}"
            ex=$(echo "$pct * $n / 100" | bc)
            total=$((total + n))
            executed=$((executed + ex))
        fi
        if [[ "$line" =~ ^Branches\ executed:([0-9.]+)%\ of\ ([0-9]+) ]]; then
            pct="${BASH_REMATCH[1]}"
            n="${BASH_REMATCH[2]}"
            ex=$(echo "$pct * $n / 100" | bc)
            br_total=$((br_total + n))
            br_exec=$((br_exec + ex))
        fi
    done < <(gcov -b "$gcda" 2>&1 | head -10)
done

if [ $total -gt 0 ]; then
    echo "  Lines: $executed/$total ($(( executed * 100 / total ))%)"
fi
if [ $br_total -gt 0 ]; then
    echo "  Branches: $br_exec/$br_total ($(( br_exec * 100 / br_total ))%)"
fi
