#!/usr/bin/env bash
# Inspect MinGW static archives for yaramod/re2 (optional maintainer helper).
# Override paths if your tree differs:
#   YARAMOD=/path/to/yaramod/lib RE2_EXT=/path/to/libre2.a bash tools/dev/inspect_yaramod_archives.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
YARAMOD="${YARAMOD:-$REPO_ROOT/build/windows/deps/install/yaramod/lib}"
RE2_EXT="${RE2_EXT:-$REPO_ROOT/build/windows/external/src/yaramod-project-build/deps/pog/deps/re2/re2/src/re2-dep-build/libre2.a}"

echo "=== libyaramod.a ==="
FIRST=$(x86_64-w64-mingw32-ar t "$YARAMOD/libyaramod.a" | head -1)
echo "First object: $FIRST"
mkdir -p /tmp/checkf1
cd /tmp/checkf1
x86_64-w64-mingw32-ar x "$YARAMOD/libyaramod.a" "$FIRST"
file "$FIRST"
cd /

echo "=== libpog_re2.a (installed) ==="
FIRST=$(x86_64-w64-mingw32-ar t "$YARAMOD/libpog_re2.a" | head -1)
echo "First object: $FIRST"
mkdir -p /tmp/checkf2
cd /tmp/checkf2
x86_64-w64-mingw32-ar x "$YARAMOD/libpog_re2.a" "$FIRST"
file "$FIRST"

echo "=== libre2.a (external build) ==="
FIRST=$(x86_64-w64-mingw32-ar t "$RE2_EXT" | head -1)
echo "First object: $FIRST"
mkdir -p /tmp/checkf3
cd /tmp/checkf3
x86_64-w64-mingw32-ar x "$RE2_EXT" "$FIRST"
file "$FIRST"

rm -rf /tmp/checkf{1,2,3}
