#!/usr/bin/env python3
"""Generate 200+ C sources that are not target algorithm-recovery labels (audit B8)."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "algorithm_recovery" / "sources" / "negative"

SPECS: list[tuple[str, str]] = []


def add(stem: str, src: str) -> None:
    SPECS.append((stem, src.strip() + "\n"))


for i in range(20):
    k = 10 + i
    add(
        f"unit_f2c_{i:02d}",
        f"""
int main(void) {{
    int c = {k};
    int f = (c * 9) / 5 + 32;
    return f & 255;
}}
""",
    )
    add(
        f"clamp_u8_{i:02d}",
        f"""
int main(void) {{
    int x = {k * 17};
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return x;
}}
""",
    )
    add(
        f"lerp_rgb_{i:02d}",
        f"""
int main(void) {{
    int a = {k};
    int b = {k + 40};
    int t = {i};
    return a + ((b - a) * t) / 20;
}}
""",
    )
    add(
        f"dow_{i:02d}",
        f"""
int main(void) {{
    int y = 2000 + {i};
    int m = 1 + ({i} % 12);
    int d = 1 + ({i} % 28);
    int t = y + y / 4 - y / 100 + y / 400 + (13 * m + 8) / 5 + d;
    return t % 7;
}}
""",
    )
    add(
        f"bmi_{i:02d}",
        f"""
int main(void) {{
    int kg = 50 + {i};
    int cm = 150 + {i};
    return (kg * 10000) / (cm * cm);
}}
""",
    )
    add(
        f"haversine_stub_{i:02d}",
        f"""
int main(void) {{
    int lat = {i} * 3;
    int lon = {i} * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}}
""",
    )
    add(
        f"pid_step_{i:02d}",
        f"""
int main(void) {{
    int err = {k} - {i};
    int integ = err + {i};
    int deriv = err - {i};
    return (3 * err + integ + deriv) & 255;
}}
""",
    )
    add(
        f"flag_pack_{i:02d}",
        f"""
int main(void) {{
    int a = {i} & 1;
    int b = ({i} >> 1) & 1;
    int c = ({i} >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}}
""",
    )
    add(
        f"ipv4_octets_{i:02d}",
        f"""
int main(void) {{
    unsigned v = 0x0A000000u + {i}u;
    return (int)((v >> 24) & 255u);
}}
""",
    )
    add(
        f"mortgage_stub_{i:02d}",
        f"""
int main(void) {{
    int principal = 100000 + {i} * 1000;
    int rate = 3 + ({i} % 5);
    int years = 15 + ({i} % 15);
    return (principal / 100) * rate / years;
}}
""",
    )
    add(
        f"log_level_{i:02d}",
        f"""
int main(void) {{
    int level = {i} % 5;
    if (level == 0) return 10;
    if (level == 1) return 20;
    if (level == 2) return 30;
    if (level == 3) return 40;
    return 50;
}}
""",
    )


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.c"):
        old.unlink()
    for old in OUT.glob("*.labels.json"):
        old.unlink()
    for stem, src in SPECS:
        (OUT / f"{stem}.c").write_text(src, encoding="utf-8")
        (OUT / f"{stem}.labels.json").write_text('{"labels":[]}\n', encoding="utf-8")
    print(f"Wrote {len(SPECS)} negative sources under {OUT}")
    if len(SPECS) < 200:
        raise SystemExit(f"need 200+ sources, got {len(SPECS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
