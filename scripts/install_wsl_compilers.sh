#!/usr/bin/env bash
# install_wsl_compilers.sh — Install all compilers needed to build
# RetDec managed-language integration test fixtures in WSL (Ubuntu 22.04+).
#
# Does NOT run apt update or apt upgrade.
# Each step is skipped if the tool is already installed.
#
# Usage:
#   bash scripts/install_wsl_compilers.sh
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -eo pipefail   # -e: exit on error  (no -u: avoids issues with snap/sdkman env)

log()  { echo ""; echo ">>> $*"; }
skip() { echo "    [skip] $*"; }
ok()   { echo "    [ok]   $*"; }

# ---------------------------------------------------------------------------
# 1. Java
# ---------------------------------------------------------------------------
if command -v javac &>/dev/null; then
    skip "javac already installed: $(javac --version 2>&1)"
else
    log "Installing Java (default-jdk)..."
    sudo apt install -y default-jdk
    ok "javac: $(javac --version 2>&1)"
fi

# ---------------------------------------------------------------------------
# 2. Kotlin  (via snap — no SDKMAN needed)
# ---------------------------------------------------------------------------
if command -v kotlinc &>/dev/null; then
    skip "kotlinc already installed: $(kotlinc -version 2>&1 | head -1)"
else
    log "Installing Kotlin via snap..."
    sudo snap install --classic kotlin
    ok "kotlinc: $(kotlinc -version 2>&1 | head -1)"
fi

# ---------------------------------------------------------------------------
# 3. .NET 8 SDK
# ---------------------------------------------------------------------------
if command -v dotnet &>/dev/null; then
    skip "dotnet already installed: $(dotnet --version)"
else
    log "Installing .NET 8 SDK..."
    OS_VER="$(lsb_release -rs 2>/dev/null || echo 22.04)"
    wget -q "https://packages.microsoft.com/config/ubuntu/${OS_VER}/packages-microsoft-prod.deb" \
        -O /tmp/msprod.deb
    sudo dpkg -i /tmp/msprod.deb
    # Refresh only the Microsoft feed, not the whole system
    sudo apt update -o Dir::Etc::sourcelist="sources.list.d/microsoft-prod.list" \
                    -o Dir::Etc::sourceparts="-" \
                    -o APT::Get::List-Cleanup="0" -qq 2>/dev/null || sudo apt update -qq
    sudo apt install -y dotnet-sdk-8.0
    ok "dotnet: $(dotnet --version)"
fi

# ---------------------------------------------------------------------------
# 4. Python 3.8 / 3.11 / 3.12
# ---------------------------------------------------------------------------
log "Checking Python versions..."
MISSING_PY=()
for ver in 3.8 3.11 3.12; do
    command -v "python${ver}" &>/dev/null || MISSING_PY+=("python${ver}")
done

if [[ ${#MISSING_PY[@]} -eq 0 ]]; then
    skip "python3.8, python3.11, python3.12 all installed"
else
    echo "    Missing: ${MISSING_PY[*]}"
    log "Adding deadsnakes PPA (provides older Python versions)..."
    sudo add-apt-repository -y ppa:deadsnakes/ppa
    # Refresh package lists — required after adding a new repo (no upgrade)
    echo "    Refreshing package lists (no upgrade)..."
    sudo apt-get update -qq
    log "Installing missing Python versions: ${MISSING_PY[*]}..."
    sudo apt-get install -y python3 "${MISSING_PY[@]}"
    for ver in 3.8 3.11 3.12; do
        command -v "python${ver}" &>/dev/null && ok "python${ver}: $(python${ver} --version)"
    done
fi

# ---------------------------------------------------------------------------
# 5. Lua 5.1 and 5.4
# ---------------------------------------------------------------------------
log "Checking Lua..."
MISSING_LUA=()
command -v luac5.1 &>/dev/null || MISSING_LUA+=(lua5.1)
command -v luac5.4 &>/dev/null || MISSING_LUA+=(lua5.4)
if [[ ${#MISSING_LUA[@]} -eq 0 ]]; then
    skip "luac5.1 and luac5.4 already installed"
else
    log "Installing: ${MISSING_LUA[*]}..."
    sudo apt install -y "${MISSING_LUA[@]}"
    ok "luac5.1: $(luac5.1 -v 2>&1 | head -1)"
    ok "luac5.4: $(luac5.4 -v 2>&1 | head -1)"
fi

# ---------------------------------------------------------------------------
# 6. WABT (wat2wasm)
# ---------------------------------------------------------------------------
if command -v wat2wasm &>/dev/null; then
    skip "wat2wasm already installed: $(wat2wasm --version 2>&1 | head -1)"
else
    log "Installing WABT (wat2wasm)..."
    sudo apt install -y wabt
    ok "wat2wasm: $(wat2wasm --version 2>&1 | head -1)"
fi

# ---------------------------------------------------------------------------
# 7. Emscripten
# ---------------------------------------------------------------------------
if command -v emcc &>/dev/null; then
    skip "emcc already installed: $(emcc --version 2>&1 | head -1)"
else
    log "Installing Emscripten..."
    sudo apt install -y emscripten
    ok "emcc: $(emcc --version 2>&1 | head -1)"
fi

# ---------------------------------------------------------------------------
# 8. Android SDK command-line tools (for d8 / DEX compilation)
# ---------------------------------------------------------------------------
ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"
D8="$ANDROID_HOME/build-tools/34.0.0/d8"

if [[ -x "$D8" ]]; then
    skip "Android d8 already at $D8"
else
    log "Installing Android command-line tools..."
    command -v unzip &>/dev/null || sudo apt install -y unzip

    ANDROID_ZIP="/tmp/android-cmdline-tools.zip"
    if [[ ! -f "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ]]; then
        log "  Downloading Android cmdline-tools (~130 MB)..."
        wget --progress=bar:force \
            "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" \
            -O "$ANDROID_ZIP"
        mkdir -p "$ANDROID_HOME/cmdline-tools"
        unzip -q "$ANDROID_ZIP" -d "$ANDROID_HOME/cmdline-tools/"
        mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" \
           "$ANDROID_HOME/cmdline-tools/latest" 2>/dev/null || true
    fi

    export ANDROID_HOME
    export PATH="$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/build-tools/34.0.0"

    log "  Accepting Android SDK licences and installing build-tools 34.0.0..."
    yes | sdkmanager --licenses 2>&1 || true
    sdkmanager "build-tools;34.0.0" 2>&1 || {
        echo "    [warn] sdkmanager failed — Android build-tools may not be fully installed."
        echo "           You can retry manually: sdkmanager 'build-tools;34.0.0'"
    }
    if [[ -x "$D8" ]]; then
        ok "d8 installed at $D8"
    else
        echo "    [warn] d8 not found at $D8 — Android DEX tests will be skipped."
    fi
fi

# Persist ANDROID_HOME in ~/.bashrc
if ! grep -q 'ANDROID_HOME' ~/.bashrc 2>/dev/null; then
    {
        echo ""
        echo "# Android SDK (added by install_wsl_compilers.sh)"
        echo "export ANDROID_HOME=\"\$HOME/android-sdk\""
        echo "export PATH=\"\$PATH:\$ANDROID_HOME/cmdline-tools/latest/bin:\$ANDROID_HOME/build-tools/34.0.0\""
    } >> ~/.bashrc
    echo "    [ok]   ANDROID_HOME added to ~/.bashrc"
fi
export ANDROID_HOME
export PATH="$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/build-tools/34.0.0"

# ---------------------------------------------------------------------------
# 9. Clang / LLVM / AFL++ / lcov
# ---------------------------------------------------------------------------
log "Checking fuzzing and coverage tools..."
MISSING_TOOLS=()
command -v clang    &>/dev/null || MISSING_TOOLS+=(clang)
command -v llvm-ar  &>/dev/null || MISSING_TOOLS+=(llvm)
command -v afl-fuzz &>/dev/null || MISSING_TOOLS+=(afl++)
command -v lcov     &>/dev/null || MISSING_TOOLS+=(lcov)
command -v genhtml  &>/dev/null || MISSING_TOOLS+=(lcov)  # genhtml ships with lcov

if [[ ${#MISSING_TOOLS[@]} -eq 0 ]]; then
    skip "clang, llvm, afl++, lcov already installed"
else
    # Deduplicate
    MISSING_TOOLS=($(printf '%s\n' "${MISSING_TOOLS[@]}" | sort -u))
    log "Installing: ${MISSING_TOOLS[*]}..."
    sudo apt install -y "${MISSING_TOOLS[@]}" || \
        echo "    [warn] Some tools could not be installed — check output above."
    command -v clang    &>/dev/null && ok "clang: $(clang --version 2>&1 | head -1)"
    command -v lcov     &>/dev/null && ok "lcov:  $(lcov --version 2>&1 | head -1)"
fi

# ---------------------------------------------------------------------------
# Final verification
# ---------------------------------------------------------------------------
echo ""
log "=== Installation complete — verification ==="
echo ""

pcheck() {
    local cmd="$1"; local label="${2:-$cmd}"
    if command -v "$cmd" &>/dev/null; then
        printf "  OK   %-24s %s\n" "$label" "$("$cmd" --version 2>&1 | head -1)"
    else
        printf "  MISS %-24s not found\n" "$label"
    fi
}

pcheck javac      "javac (Java)"
pcheck kotlinc    "kotlinc (Kotlin)"
pcheck dotnet     "dotnet (.NET)"
pcheck python3    "python3"
pcheck python3.8  "python3.8"
pcheck python3.11 "python3.11"
pcheck python3.12 "python3.12"
pcheck luac5.1    "luac5.1 (Lua)"
pcheck luac5.4    "luac5.4 (Lua)"
pcheck wat2wasm   "wat2wasm (WABT)"
pcheck emcc       "emcc (Emscripten)"
pcheck clang      "clang"
pcheck afl-fuzz   "afl-fuzz (AFL++)"
pcheck lcov       "lcov"
pcheck genhtml    "genhtml"

if [[ -x "$ANDROID_HOME/build-tools/34.0.0/d8" ]]; then
    printf "  OK   %-24s %s\n" "d8 (Android)" "$ANDROID_HOME/build-tools/34.0.0/d8"
else
    printf "  MISS %-24s ANDROID_HOME=%s\n" "d8 (Android)" "$ANDROID_HOME"
fi

echo ""
echo "Run 'source ~/.bashrc' to activate any new PATH entries in this shell."
