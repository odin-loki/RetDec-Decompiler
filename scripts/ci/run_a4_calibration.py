#!/usr/bin/env python3
"""A4: empirical precision of reported confidences. Does not change detectors."""
from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load_dets(cfg_path: Path) -> list[tuple[str, float]]:
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    out = []
    for fn in cfg.get("functions") or []:
        for det in fn.get("semanticDetections") or []:
            kind = det.get("kind") or ""
            label = det.get("label") or ""
            if not label:
                continue
            conf = float(det.get("confidence") or 0.0)
            out.append((f"{kind}:{label}", conf))
    return out


def main() -> int:
    work = ROOT / "build/linux/b8-loop-work"
    rows = []
    if work.is_dir():
        for cfg in work.glob("*/*.config.json"):
            for key, conf in load_dets(cfg):
                # Loop-negatives have empty ground truth: every detection is FP.
                rows.append({"source": "b8-loop-negative", "key": key, "confidence": conf, "correct": False})

    bins: dict[str, list[bool]] = defaultdict(list)
    by_key: dict[str, list[bool]] = defaultdict(list)
    for r in rows:
        b = min(int(r["confidence"] * 5), 4)
        label = f"{b * 0.2:.1f}-{b * 0.2 + 0.2:.1f}"
        bins[label].append(r["correct"])
        by_key[r["key"]].append(r["correct"])

    curve = []
    for label in sorted(bins):
        vals = bins[label]
        curve.append({
            "bin": label,
            "n": len(vals),
            "empirical_precision": (sum(vals) / len(vals)) if vals else 0.0,
        })
    per_key = []
    for key, vals in sorted(by_key.items()):
        per_key.append({
            "key": key,
            "n": len(vals),
            "empirical_precision": (sum(vals) / len(vals)) if vals else 0.0,
        })

    payload = {
        "fitted": False,
        "note": (
            "Loop-negative detections only. Every hit is a false positive, "
            "so empirical precision is 0 at the reported confidences. "
            "Detector constants were not changed."
        ),
        "n": len(rows),
        "reliability_bins": curve,
        "per_detection": per_key,
    }
    (ROOT / "results/a4-calibration.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )
    md = [
        "# A4 confidence calibration (observation)",
        "",
        "Detector constants were **not fitted**. A reported 0.90 is not 90%",
        "correct on this set.",
        "",
        f"- observations: {len(rows)} detections on the 100 loop-negatives",
        "- ground truth: empty (every detection is a false positive)",
        "",
        "## Reliability bins",
        "",
        "| Reported confidence | n | empirical precision |",
        "|--------------------|---|---------------------|",
    ]
    for row in curve:
        md.append(f"| {row['bin']} | {row['n']} | {row['empirical_precision']:.3f} |")
    md += [
        "",
        "## Per detection kind",
        "",
        "| Detection | n | empirical precision |",
        "|-----------|---|---------------------|",
    ]
    for row in per_key:
        md.append(f"| `{row['key']}` | {row['n']} | {row['empirical_precision']:.3f} |")
    md += [
        "",
        "Extract can still report B8 loop FP 0.000 while this table is",
        "non-empty: generic `std::` algorithms are skipped and container",
        "labels need confidence ≥ 0.8. Fitting would require changing",
        "detector constants and re-scoring the 216-binary table; that",
        "was not done.",
        "",
    ]
    (ROOT / "results/a4-calibration.md").write_text("\n".join(md), encoding="utf-8")
    print(f"A4_OK n={len(rows)} fitted=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
