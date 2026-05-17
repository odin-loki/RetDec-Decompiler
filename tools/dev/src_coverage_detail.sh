#!/usr/bin/env bash
# Per-source-file coverage using gcov's own source file matching
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
OBJ=$BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir
SRCDIR="${SRCDIR:-$REPO_ROOT/src/llvmir2hll}"

cd "$OBJ"

total_lines=0; exec_lines=0
total_br=0; exec_br=0

declare -A file_data

# Find all gcda files and run gcov, extract our source file stats
while IFS= read -r gcda; do
    relpath="${gcda#$OBJ/}"
    relpath="${relpath%.gcda}"
    src="$SRCDIR/${relpath}"
    [ -f "$src" ] || continue
    
    # Run gcov and look for our source file's stats
    gcov_out=$(gcov -b "$gcda" 2>&1)
    
    # Look for the line starting with "File '/mnt/c/.../src/llvmir2hll/..."
    while IFS= read -r line; do
        if [[ "$line" == *"$src"* ]]; then
            # Next line has "Lines executed:XX.YY% of ZZ"
            IFS= read -r lline
            lpct=$(echo "$lline" | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1)
            lcount=$(echo "$lline" | grep -oP 'of \K[0-9]+' | head -1)
            # Next line has "Branches executed:..."
            IFS= read -r bline_exec
            IFS= read -r bline_taken
            bpct=$(echo "$bline_exec" | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1)
            bcount=$(echo "$bline_exec" | grep -oP 'of \K[0-9]+' | head -1)
            
            if [ -n "$lcount" ] && [ -n "$lpct" ]; then
                lexec=$(echo "$lpct * $lcount / 100" | bc 2>/dev/null || echo 0)
                total_lines=$((total_lines + lcount))
                exec_lines=$((exec_lines + lexec))
            fi
            if [ -n "$bcount" ] && [ -n "$bpct" ]; then
                bexec=$(echo "$bpct * $bcount / 100" | bc 2>/dev/null || echo 0)
                total_br=$((total_br + bcount))
                exec_br=$((exec_br + bexec))
                
                # Store for per-file output
                name="${relpath}"
                file_data["$name"]="lines=${lexec:-0}/${lcount:-0} branches=${bexec:-0}/${bcount:-0} lp=${lpct:-0} bp=${bpct:-0}"
            fi
            break
        fi
    done < <(echo "$gcov_out")
done < <(find "$OBJ" -name '*.gcda' | sort)

# Show bottom 20 by branch coverage
echo "Files with lowest source-line branch coverage:"
echo "----------------------------------------------------------------------"
for key in "${!file_data[@]}"; do
    echo "$key ${file_data[$key]}"
done | sort -t'=' -k6 -n | head -30 | while read line; do
    name=$(echo "$line" | awk '{print $1}')
    lines=$(echo "$line" | grep -oP 'lines=\K[^\ ]+')
    br=$(echo "$line" | grep -oP 'branches=\K[^\ ]+')
    lp=$(echo "$line" | grep -oP 'lp=\K[0-9.]+')
    bp=$(echo "$line" | grep -oP 'bp=\K[0-9.]+')
    printf "  %-60s L:%s B:%s (%.1f%% br)\n" "${name:0:60}" "$lines" "$br" "${bp:-0}"
done

echo ""
echo "=== OVERALL src/llvmir2hll source coverage ==="
if [ $total_lines -gt 0 ]; then
    echo "  Lines:    $exec_lines / $total_lines ($(( exec_lines * 100 / total_lines ))%)"
fi
if [ $total_br -gt 0 ]; then
    echo "  Branches: $exec_br / $total_br ($(( exec_br * 100 / total_br ))%)"
fi
