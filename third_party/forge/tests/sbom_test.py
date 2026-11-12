"""SBOM check: SPDX 2.3 structure, every vendored file attributed, deterministic output."""
import json, subprocess, sys
root, py = sys.argv[1], sys.executable
run = lambda: subprocess.run([py, root + "/tools/sbom.py", "--root", root], check=True, capture_output=True, text=True).stdout
a, b = run(), run()
assert a == b, "SBOM not deterministic"
d = json.loads(a)
assert d["spdxVersion"] == "SPDX-2.3" and d["dataLicense"] == "CC0-1.0"
names = {p["name"]: p for p in d["packages"]}
assert {"forge", "monocypher"} <= set(names), names.keys()
ids = {p["SPDXID"] for p in d["packages"]} | {f["SPDXID"] for f in d["files"]} | {"SPDXRef-DOCUMENT"}
assert all(r["spdxElementId"] in ids and r["relatedSpdxElement"] in ids for r in d["relationships"])
mono = [f for f in d["files"] if "third_party/monocypher/" in f["fileName"]]
assert mono and all(f["licenseConcluded"] == "BSD-2-Clause OR CC0-1.0" for f in mono)
assert any(f["fileName"] == "./FORGE_PLAN.md" for f in d["files"])
print(f"sbom ok: {len(d['files'])} files, {len(d['packages'])} packages")
