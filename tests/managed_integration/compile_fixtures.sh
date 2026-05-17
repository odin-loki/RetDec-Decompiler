#!/usr/bin/env bash
# compile_fixtures.sh — Compile all managed-language fixture source files
# into binary form (*.class, *.jar, *.dex, *.dll, *.pyc, *.luac, *.wasm).
#
# Run from the repo root in WSL after installing compilers:
#   bash tests/managed_integration/compile_fixtures.sh
#
# Prerequisites (install with scripts/install_wsl_compilers.sh):
#   javac, kotlinc, d8 (Android SDK), dotnet, python3.8/3.11/3.12,
#   luac5.1, luac5.4, wat2wasm (wabt)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURES="$SCRIPT_DIR/fixtures"
ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"
D8="$ANDROID_HOME/build-tools/34.0.0/d8"

log()  { echo "[compile] $*"; }
warn() { echo "[warn]    $*" >&2; }
need() { command -v "$1" &>/dev/null || { warn "Missing: $1 (skipping $2)"; return 1; }; }

# ---------------------------------------------------------------------------
# 1. Java (.class and .jar)
# ---------------------------------------------------------------------------
if need javac "Java"; then
    log "Compiling Java..."
    cd "$FIXTURES/java"
    for src in Hello.java Loops.java Exceptions.java Generics.java Lambdas.java; do
        javac -source 11 -target 11 "$src"
        log "  $src → ${src%.java}.class"
    done
    jar cfe Hello.jar Hello Hello.class
    log "  → Hello.jar"
    cd "$SCRIPT_DIR"
fi

# ---------------------------------------------------------------------------
# 2. Kotlin (.class / .jar)
# ---------------------------------------------------------------------------
if need kotlinc "Kotlin"; then
    log "Compiling Kotlin..."
    cd "$FIXTURES/kotlin"
    # Hello and DataClass (no coroutines dependency)
    kotlinc Hello.kt DataClass.kt -include-runtime -d kotlin_fixtures.jar 2>/dev/null
    log "  Hello.kt DataClass.kt → kotlin_fixtures.jar"
    # Coroutines (needs -cp kotlinx-coroutines; skip gracefully if not present)
    if kotlinc -version 2>&1 | grep -q "2\." ; then
        kotlinc Coroutines.kt -include-runtime -d coroutines.jar \
            -cp "$(find ~/.gradle ~/.m2 /usr/share -name 'kotlinx-coroutines-core*.jar' 2>/dev/null | head -1)" \
            2>/dev/null || warn "Coroutines.kt skipped (no kotlinx-coroutines in classpath)"
    fi
    cd "$SCRIPT_DIR"
fi

# ---------------------------------------------------------------------------
# 3. Android DEX (via d8)
# ---------------------------------------------------------------------------
if [ -x "$D8" ]; then
    log "Compiling DEX..."
    cd "$FIXTURES"
    mkdir -p dex
    # Use already-compiled Java .class files
    "$D8" java/Hello.class --output dex/
    mv dex/classes.dex dex/Hello.dex 2>/dev/null || true
    log "  Hello.class → dex/Hello.dex"
    cd "$SCRIPT_DIR"
else
    warn "d8 not found at $D8 (DEX fixtures skipped)"
fi

# ---------------------------------------------------------------------------
# 4. .NET — C#, VB.NET, F#
# ---------------------------------------------------------------------------
if need dotnet ".NET"; then
    log "Compiling C#..."
    cd "$FIXTURES/csharp"
    dotnet build csharp_fixtures.csproj -c Release -o ./bin/ 2>/dev/null
    log "  → bin/csharp_fixtures.dll"
    cd "$SCRIPT_DIR"

    log "Compiling VB.NET..."
    cd "$FIXTURES/vbnet"
    dotnet build vbnet_fixtures.vbproj -c Release -o ./bin/ 2>/dev/null
    log "  → bin/vbnet_fixtures.dll"
    cd "$SCRIPT_DIR"

    log "Compiling F#..."
    cd "$FIXTURES/fsharp"
    dotnet build fsharp_fixtures.fsproj -c Release -o ./bin/ 2>/dev/null
    log "  → bin/fsharp_fixtures.dll"
    cd "$SCRIPT_DIR"
fi

# ---------------------------------------------------------------------------
# 5. Python .pyc (multiple versions)
# ---------------------------------------------------------------------------
log "Compiling Python .pyc files..."
for py_src in hello classes comprehensions generators closures; do
    for pyver in python3.8 python3.11 python3.12; do
        if command -v "$pyver" &>/dev/null; then
            "$pyver" -m py_compile "$FIXTURES/python/$py_src.py"
            log "  $py_src.py → __pycache__/$py_src.cpython-*.pyc ($pyver)"
        else
            warn "$pyver not found"
        fi
    done
done

# ---------------------------------------------------------------------------
# 6. Lua bytecode (5.1 and 5.4)
# ---------------------------------------------------------------------------
log "Compiling Lua bytecode..."
for lua_src in hello tables closures coroutines; do
    for luabin in luac5.1 luac5.4; do
        if command -v "$luabin" &>/dev/null; then
            ver="${luabin#luac}"
            out="$FIXTURES/lua/$lua_src.luac$ver"
            "$luabin" -o "$out" "$FIXTURES/lua/$lua_src.lua"
            log "  $lua_src.lua → $lua_src.luac$ver ($luabin)"
        else
            warn "$luabin not found"
        fi
    done
done

# ---------------------------------------------------------------------------
# 7. WebAssembly (.wasm from .wat)
# ---------------------------------------------------------------------------
if need wat2wasm "WASM"; then
    log "Compiling WebAssembly..."
    cd "$FIXTURES/wasm"
    for wat in add.wat memory.wat table.wat; do
        wat2wasm "$wat" -o "${wat%.wat}.wasm"
        log "  $wat → ${wat%.wat}.wasm"
    done
    # exceptions.wat needs --enable-exceptions
    wat2wasm exceptions.wat --enable-exceptions -o exceptions.wasm 2>/dev/null || \
        warn "exceptions.wat skipped (old wabt without exceptions proposal)"
    cd "$SCRIPT_DIR"
fi

# ---------------------------------------------------------------------------
# 8. Generate malformed variants (requires compiled binaries above)
# ---------------------------------------------------------------------------
log "Generating malformed fixtures..."
python3 "$FIXTURES/malformed/generate_malformed.py"

log "Done! All fixtures compiled."
