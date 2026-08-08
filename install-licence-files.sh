#!/usr/bin/env bash
# install-licence-files.sh — Split monolithic LICENSE into condensed set.
# Usage: bash install-licence-files.sh [REPO_ROOT]
set -euo pipefail

REPO="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
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

Third-party components (see cmake/deps.cmake for pinned versions):

  LLVM (University of Illinois/NCSA) — via avast/llvm fork
  Capstone (BSD-3-Clause)
  YARA (BSD-3-Clause)
  yaramod (MIT)
  Keystone (GPL-2.0) — tool-only; excluded from commercial packages
  OpenSSL (Apache-2.0)
  zlib (Zlib)
  googletest (BSD-3-Clause)
  Qt6 (LGPL/GPL/commercial — runtime dependency when GUI enabled)
  Eigen (MPL-2.0)
  rapidjson (MIT)
  tinyxml2 (Zlib)
  stb (MIT/public domain)
  tlsh (Apache-2.0 / BSD)
  elfio (MIT)
  whereami (MIT/WTFPL)

Keystone (GPL-2.0) is used only in capstone2llvmirtool and related tests.
It must not be included in commercial distribution packages.

llama.cpp (MIT) — when RETDEC_ENABLE_NEURAL is enabled.
EOF

echo "Installed: LICENSE LICENSE-AGPL LICENSE-COMMERCIAL NOTICE"
echo "Backup of previous monolithic file: ${BACKUP}"
