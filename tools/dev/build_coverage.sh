#!/usr/bin/env bash
# Build only the llvmir2hll library with coverage instrumentation,
# linking all other dependencies from the existing core-debug build.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRCDIR="${SRCDIR:-$REPO_ROOT}"
DEBUG_BUILD="${DEBUG_BUILD:-$REPO_ROOT/build/linux}"
COV_BUILD="${COV_BUILD:-$HOME/retdec-build/core-coverage}"
GCOV_FLAGS="--coverage -O0 -fno-inline -g"

echo "=== Step 1: Patch cmake to add coverage flags only to llvmir2hll and bin2llvmir ==="

# We use the existing core-debug cmake setup but override flags for specific modules.
# Strategy: copy the compile_commands.json, recompile only llvmir2hll objects with --coverage,
# re-archive the library, then link a new decompiler binary.

COV_OBJ_DIR="$COV_BUILD/cov_objs"
mkdir -p "$COV_OBJ_DIR"

echo "=== Step 2: Recompile llvmir2hll sources with --coverage ==="

LLVMIR2HLL_SRC="$SRCDIR/src/llvmir2hll"
LLVMIR2HLL_OBJ_DIR="$DEBUG_BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir"

# Get the include paths and defines from the existing build
INCLUDES=$(cat "$DEBUG_BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir/flags.make" 2>/dev/null || echo "")
CXX_FLAGS=$(grep 'CXX_FLAGS' "$DEBUG_BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir/flags.make" 2>/dev/null | sed 's/CXX_FLAGS = //')
INCLUDES_FILE="$DEBUG_BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir/includes_CXX.rsp"

mkdir -p "$COV_OBJ_DIR/llvmir2hll"

total=0
for src in $(find "$LLVMIR2HLL_SRC" -name '*.cpp'); do
    rel="${src#$LLVMIR2HLL_SRC/}"
    obj="$COV_OBJ_DIR/llvmir2hll/${rel%.cpp}.o"
    obj_dir=$(dirname "$obj")
    mkdir -p "$obj_dir"
    
    # Compile with coverage flags using include paths from build system
    if g++ -std=c++17 $GCOV_FLAGS \
        -I"$SRCDIR/include" \
        -I"$SRCDIR/src" \
        -I"$DEBUG_BUILD/include" \
        @"$INCLUDES_FILE" \
        -DRETDEC_COMMIT_HASH=\"coverage\" \
        -DRETDEC_BUILD_OS=\"linux\" \
        -fPIC \
        -c "$src" -o "$obj" 2>/dev/null; then
        total=$((total+1))
    else
        echo "  WARNING: failed to compile $rel, using debug version"
        # Fall back to original object file
        orig_obj="$LLVMIR2HLL_OBJ_DIR/${rel%.cpp}.cpp.o"
        if [ -f "$orig_obj" ]; then
            cp "$orig_obj" "$obj"
        fi
    fi
done
echo "  Compiled $total llvmir2hll objects with coverage"

echo "=== Step 3: Create coverage-instrumented llvmir2hll library ==="
COV_LIB="$COV_OBJ_DIR/libretdec-llvmir2hll-cov.a"
all_objs=$(find "$COV_OBJ_DIR/llvmir2hll" -name '*.o' | tr '\n' ' ')
ar rcs "$COV_LIB" $all_objs
echo "  Created $COV_LIB"

echo "=== Done: Library ready for linking ==="
echo "Run the coverage test script to collect data"
