#!/usr/bin/env bash
echo "=== MANAGED ==="
echo -n "Java:    "; javac --version 2>/dev/null || echo MISSING
source ~/.sdkman/bin/sdkman-init.sh 2>/dev/null; echo -n "Kotlin:  "; kotlinc -version 2>/dev/null | head -1 || echo MISSING
echo -n "dotnet:  "; dotnet --version 2>/dev/null || echo MISSING
echo -n "d8(DEX): "; ~/android-sdk/build-tools/34.0.0/d8 --version 2>/dev/null | head -1 || echo MISSING
echo -n "Python3: "; python3 --version 2>/dev/null || echo MISSING
echo -n "Lua:     "; luac -v 2>/dev/null | head -1 || echo MISSING
echo -n "wat2wasm:"; wat2wasm --version 2>/dev/null || echo MISSING
echo ""
echo "=== NATIVE ==="
echo -n "gcc:     "; gcc --version 2>/dev/null | head -1 || echo MISSING
echo -n "clang:   "; clang --version 2>/dev/null | head -1 || echo MISSING
echo -n "rustc:   "; rustc --version 2>/dev/null || echo MISSING
echo -n "go:      "; go version 2>/dev/null || echo MISSING
echo -n "nasm:    "; nasm --version 2>/dev/null | head -1 || echo MISSING
