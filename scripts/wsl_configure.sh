#!/usr/bin/env bash
# Configure RetDec in WSL (optional sudo apt for common deps), same tree as wsl_configure_nosudo.sh.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

log() { echo -e "\n\033[1;34m[$(date +%H:%M:%S)] $*\033[0m"; }

log "Checking build dependencies (sudo may prompt)"
PKGS=""
for pkg in libxkbcommon-dev ninja-build; do
	dpkg -l "$pkg" &>/dev/null || PKGS="$PKGS $pkg"
done
if [[ -n "$PKGS" ]]; then
	log "Installing:$PKGS"
	sudo apt-get update -qq
	sudo apt-get install -y $PKGS
fi

exec "${SCRIPT_DIR}/wsl_configure_nosudo.sh"
