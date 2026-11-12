#!/usr/bin/env bash
# install-licence-files.sh — Split monolithic LICENSE into condensed set, and
# collect the licence texts of the vendored third-party components into
# LICENSE-THIRD-PARTY (installed to share/retdec by the top-level CMakeLists).
#
# Usage:
#   bash scripts/install-licence-files.sh [REPO_ROOT]
#   bash scripts/install-licence-files.sh --third-party-only [REPO_ROOT]
#       only (re)generate LICENSE-THIRD-PARTY from third_party/
set -euo pipefail

THIRD_PARTY_ONLY=0
if [[ "${1:-}" == "--third-party-only" ]]; then
	THIRD_PARTY_ONLY=1
	shift
fi

REPO="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# ── LICENSE-THIRD-PARTY ──────────────────────────────────────────────────────
# retdec-gui is built from two first-party libraries vendored in third_party/
# (FuseUI and Forge: AGPL-3.0-or-later with a commercial option, same owner as
# RetDec) and the third-party code they carry. Their licence texts travel with
# every binary package.
write_third_party() {
	local out="${REPO}/LICENSE-THIRD-PARTY" tp="${REPO}/third_party"
	local stb="${tp}/fuseui/vendor/stb/stb_truetype.h"
	local f
	for f in "${tp}/fuseui/LICENSE.md" "${tp}/forge/NOTICE" \
		"${tp}/forge/third_party/monocypher/LICENCE.md" \
		"${tp}/fuseui/vendor/fonts/opensans/LICENSE.txt" \
		"${tp}/fuseui/vendor/lua/LICENSE.txt" "${stb}"; do
		[[ -f "${f}" ]] || { echo "Missing ${f}" >&2; exit 1; }
	done
	{
		cat <<'TXT'
Third-party licence texts for components vendored in third_party/
==================================================================

retdec-gui is built on FuseUI (third_party/fuseui) and Forge (third_party/forge).
Both are first-party: Copyright (c) 2026 Odin Loch (Imortek), the owner of
RetDec, dual-licensed AGPL-3.0-or-later or commercial, exactly like RetDec
itself (LICENSE, LICENSE-AGPL, LICENSE-COMMERCIAL). The AGPL-3.0 text is in
LICENSE-AGPL and is not repeated here.

The code and data below are third-party and keep their own licences.

  stb (stb_truetype, stb_image, stb_image_write, stb_image_resize2)
                         third_party/fuseui/vendor/stb      MIT or public domain (Unlicense)
  Open Sans font         third_party/fuseui/vendor/fonts    Apache-2.0
  Lua 5.2                third_party/fuseui/vendor/lua      MIT (vendored; not linked
                                                              into retdec-gui)
  Monocypher 4.0.2       third_party/forge/third_party      BSD-2-Clause or CC0-1.0
  Level Zero API header  third_party/forge/third_party      MIT (see the Forge NOTICE)

TXT
		echo "=================================================================="
		echo "Forge NOTICE (third_party/forge/NOTICE)"
		echo "=================================================================="
		cat "${tp}/forge/NOTICE"
		echo
		echo "=================================================================="
		echo "Monocypher (third_party/forge/third_party/monocypher/LICENCE.md)"
		echo "=================================================================="
		cat "${tp}/forge/third_party/monocypher/LICENCE.md"
		echo
		echo "=================================================================="
		echo "stb (licence block of third_party/fuseui/vendor/stb/stb_truetype.h)"
		echo "=================================================================="
		sed -n '/^This software is available under 2 licenses/,/^\*\//p' "${stb}" | sed '$d'
		echo
		echo "=================================================================="
		echo "Lua (third_party/fuseui/vendor/lua/LICENSE.txt)"
		echo "=================================================================="
		cat "${tp}/fuseui/vendor/lua/LICENSE.txt"
		echo
		echo "=================================================================="
		echo "Open Sans (third_party/fuseui/vendor/fonts/opensans/LICENSE.txt)"
		echo "=================================================================="
		cat "${tp}/fuseui/vendor/fonts/opensans/LICENSE.txt"
	} > "${out}"
	echo "Installed: LICENSE-THIRD-PARTY"
}

if [[ "${THIRD_PARTY_ONLY}" -eq 1 ]]; then
	write_third_party
	exit 0
fi
OLD="${REPO}/LICENSE"
BACKUP="${REPO}/LICENSE.full"

if [[ ! -f "${OLD}" ]]; then
	echo "Missing ${OLD}" >&2
	exit 1
fi

cp -f "${OLD}" "${BACKUP}"

# Strip UTF-8 BOM if present.
python3 - "${OLD}" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
raw = p.read_bytes()
if raw.startswith(b"\xef\xbb\xbf"):
    p.write_bytes(raw[3:])
PY

# AGPL verbatim: from first "GNU AFFERO" block through end of standard AGPL text.
python3 - "${OLD}" "${REPO}/LICENSE-AGPL" <<'PY'
import pathlib, sys, re
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
start = src.find("GNU AFFERO GENERAL PUBLIC LICENSE")
end = src.find("ADDITIONAL TERMS")
if start < 0 or end < 0:
    raise SystemExit("Could not locate AGPL boundaries in LICENSE")
agpl = src[start:end].rstrip() + "\n"
pathlib.Path(sys.argv[2]).write_text(agpl, encoding="utf-8", newline="\n")
PY

cat > "${REPO}/LICENSE" <<'EOF'
RetDec — Enhanced Retargetable Decompiler
Copyright (c) 2025-2026 Odin Loch, trading as Imortek.

This program is dual-licensed. You may use it under EITHER:

  (A) GNU Affero General Public License v3.0 or later (AGPL-3.0+),
      reproduced verbatim in LICENSE-AGPL, as modified by the
      commercial-tier terms in LICENSE-COMMERCIAL; OR

  (B) a separate commercial licence obtained from Odin Loch
      (odin.loch@outlook.com.au). See LICENSE-COMMERCIAL.

Free-tier eligibility (personal, charitable, educational, and entities
below 50,000 AUD annual income/revenue): AGPL-3.0+ at no cost, subject
to attribution requirements in LICENSE-COMMERCIAL.

Contributions are accepted under AGPL-3.0+ as extended by LICENSE-COMMERCIAL.
Submitting a pull request constitutes agreement with these terms.

This licence agreement is governed by Australian law.

Full texts:
  LICENSE-AGPL        — AGPL-3.0 verbatim
  LICENSE-COMMERCIAL  — Imortek attribution, tiers, commercial terms
  NOTICE              — third-party attributions
EOF

cat > "${REPO}/LICENSE-COMMERCIAL" <<'EOF'
Imortek Commercial Licence Terms (Section 7 summary)
Copyright (c) 2025-2026 Odin Loch, trading as Imortek.

These terms supplement LICENSE-AGPL under AGPL section 7.

§ 7.1 Free tier (no cost, AGPL-3.0+)
Personal, charitable, educational use, or entities with annual income or
revenue below 50,000 AUD (~35,000 USD). Above the threshold: obtain a
commercial licence or cease use.

§ 7.2 Attribution (free-tier users)
Prominent attribution in documentation, UI, marketing, integrations, and
publications:

  Powered by RetDec (Imortek edition), originally derived from the RetDec
  project. Copyright (c) 2025-2026 Odin Loch trading as Imortek.
  Licensed under AGPL-3.0+. See LICENSE for details.

§ 7.3 Modifications (AGPL path)
Modifications must be released under the same dual-licence framework unless
a commercial licence is obtained.

§ 7.4 Commercial licence
Required for commercial use above the free-tier threshold, proprietary
products, or when AGPL sharing obligations are not acceptable.

Fees (AUD, indicative — contact licensor for current schedule):
  - One-time setup fee tiered by annual income/budget.
  - Annual fee: 5% of revenue attributable to software incorporating this
    product (not total company revenue).
  - Volume discounts available for multiple licences.

Removed from prior published terms (deliberately):
  - Annual fee based on total company income at high brackets.
  - Mandatory open-sourcing of all research under commercial licence.
  - Quarterly R&D reporting under commercial licence.
  - 5% gross-profit royalty stacked on annual fee.
  - Bank payment details in public licence text.

§ 7.5 Commercial licence grants
Commercial use, waiver of AGPL modification-sharing (subject to product
attribution policy), support options, and deployment flexibility as agreed
in the signed commercial agreement.

§ 7.6 Contributing
Contributions are AGPL-3.0+ as extended by these terms.

§ 7.7 Governing law
Australia.

Enquiries: odin.loch@outlook.com.au
EOF

cat > "${REPO}/NOTICE" <<'EOF'
RetDec — Enhanced Retargetable Decompiler
Copyright (c) 2025-2026 Odin Loch, trading as Imortek.

This product includes software developed by Avast and contributors
(original RetDec, https://github.com/avast/retdec).

The MIT copyright and permission notice for upstream RetDec v5.0
(Copyright (c) 2017 Avast Software) are in LICENSE-MIT and must be
retained in all copies and substantial portions of the Software.

Third-party components (see cmake/deps.cmake for pinned versions):

  LLVM (Apache-2.0-with-LLVM-exceptions) — via avast/llvm fork (LLVM 8-era pin)
  Capstone (BSD-3-Clause)
  YARA (BSD-3-Clause)
  yaramod (MIT)
  Keystone (GPL-2.0) — tool-only; excluded from commercial packages
  OpenSSL (Apache-2.0)
  zlib (Zlib)
  googletest (BSD-3-Clause)
  Eigen (MPL-2.0)
  rapidjson (MIT)
  tinyxml2 (Zlib)
  stb (MIT/public domain)
  tlsh (Apache-2.0 / BSD)
  elfio (MIT)
  whereami (MIT/WTFPL)

Keystone (GPL-2.0) is used only in capstone2llvmirtool and related tests.
It must not be included in commercial distribution packages.

Vendored in third_party/ (see third_party/README.md; full licence texts in
LICENSE-THIRD-PARTY):

  FuseUI (third_party/fuseui) — the GUI toolkit retdec-gui is built on.
  Forge  (third_party/forge)  — OS layer, JSON, hashing, thread pool, and the
          inference stack behind neural refinement (RETDEC_ENABLE_NEURAL).
    Both first-party: Copyright (c) 2026 Odin Loch (Imortek), the owner of
    RetDec; AGPL-3.0-or-later with a commercial option, like RetDec itself.
    No other GUI toolkit is linked or shipped.
  stb (MIT or public domain) — third_party/fuseui/vendor/stb
  Open Sans font (Apache-2.0) — third_party/fuseui/vendor/fonts/opensans
  Lua 5.2 (MIT) — third_party/fuseui/vendor/lua (vendored; not linked
    into retdec-gui)
  Monocypher 4.0.2 (BSD-2-Clause or CC0-1.0) — third_party/forge/third_party
  Level Zero API header (MIT) — third_party/forge/third_party/level-zero
EOF

write_third_party

echo "Installed: LICENSE LICENSE-AGPL LICENSE-COMMERCIAL NOTICE"
echo "Backup of previous monolithic file: ${BACKUP}"
