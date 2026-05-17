#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEBUG_BUILD="${DEBUG_BUILD:-$REPO_ROOT/build/linux}"
COV_BUILD="${COV_BUILD:-$HOME/retdec-build/core-coverage}"
SRC="${DEBUG_BUILD}/external/src"
DST="${COV_BUILD}/external/src"

for proj in llvm-project capstone-project keystone-project yaramod-project googletest; do
    stamp_dir="${proj}-stamp"
    echo "Copying stamps for $proj..."
    for f in download configure patch update build install done; do
        src_file="$SRC/$stamp_dir/$proj-$f"
        dst_file="$DST/$stamp_dir/$proj-$f"
        if [ -f "$src_file" ] && [ ! -f "$dst_file" ]; then
            touch "$dst_file"
            echo "  created $proj-$f"
        fi
    done
done

# Also symlink the build outputs from core-debug so LLVM doesn't need to rebuild
for proj in llvm-project capstone-project keystone-project yaramod-project googletest; do
    src_build="$SRC/${proj}-build"
    dst_build="$DST/${proj}-build"
    if [ -d "$src_build" ] && [ ! -d "$dst_build" ]; then
        echo "Linking ${proj}-build..."
        ln -s "$src_build" "$dst_build"
    fi
    # Also copy the zip file if present
    zip_name="${proj%-project}.zip"
    if [ -f "$SRC/$zip_name" ] && [ ! -f "$DST/$zip_name" ]; then
        ln -sf "$SRC/$zip_name" "$DST/$zip_name"
    fi
done
echo 'Done'
