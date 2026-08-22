#!/usr/bin/env python3
"""Generate loop-containing negatives that are not target algorithm-recovery labels.

Avoids the assigned idiom signatures: digit '0'..'9' (atoi), null-scan
without mul (strlen), recursive self-call (dfs), 0x7f/0x80/shr-7 (varint).
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "algorithm_recovery" / "sources" / "negative_loops"

SPECS: list[tuple[str, str]] = []


def add(stem: str, src: str) -> None:
    SPECS.append((stem, src.strip() + "\n"))


for i in range(10):
    n = 8 + i
    add(
        f"fir_tap_{i:02d}",
        f"""
int main(void) {{
    int x[{n}];
    int c[{n}];
    int acc = 0;
    for (int k = 0; k < {n}; ++k) {{
        x[k] = k * 3 + {i};
        c[k] = 1 + (k & 3);
    }}
    for (int k = 0; k < {n}; ++k) acc += x[k] * c[k];
    return acc & 255;
}}
""",
    )
    add(
        f"hist16_{i:02d}",
        f"""
int main(void) {{
    int h[16] = {{0}};
    int v = {20 + i};
    for (int k = 0; k < 32; ++k) {{
        int bin = (v + k) & 15;
        h[bin] += 1;
        v = v * 17 + 1;
    }}
    return h[{i % 16}] & 255;
}}
""",
    )
    add(
        f"bresenham_{i:02d}",
        f"""
int main(void) {{
    int x0 = 0, y0 = 0, x1 = {6 + i}, y1 = {3 + (i % 4)};
    int dx = x1 - x0, dy = y1 - y0, err = dx - dy, plot = 0;
    int x = x0, y = y0;
    for (int s = 0; s < {20 + i}; ++s) {{
        plot += x + y;
        int e2 = err * 2;
        if (e2 > -dy) {{ err -= dy; x += 1; }}
        if (e2 < dx) {{ err += dx; y += 1; }}
        if (x >= x1 && y >= y1) break;
    }}
    return plot & 255;
}}
""",
    )
    add(
        f"box_blur_{i:02d}",
        f"""
int main(void) {{
    int a[12];
    for (int k = 0; k < 12; ++k) a[k] = k + {i};
    int s = 0;
    for (int k = 1; k < 11; ++k) s += (a[k - 1] + a[k] + a[k + 1]) / 3;
    return s & 255;
}}
""",
    )
    add(
        f"http_verb_{i:02d}",
        f"""
int main(void) {{
    const char* p = "{'GET' if i % 2 == 0 else 'PUT'} /x";
    int st = 0;
    for (; *p; ++p) {{
        char ch = *p;
        if (st == 0 && ch == 'G') st = 1;
        else if (st == 1 && ch == 'E') st = 2;
        else if (st == 2 && ch == 'T') st = 3;
        else if (st == 0 && ch == 'P') st = 4;
        else if (ch == ' ') break;
    }}
    return st + {i};
}}
""",
    )
    add(
        f"utf8_scan_{i:02d}",
        f"""
int main(void) {{
    const unsigned char s[] = {{0xC3, 0xA9, 0x61, 0x00}};
    int n = 0;
    for (int k = 0; s[k]; ++k) {{
        unsigned char b = s[k];
        if ((b & 0x80u) == 0) n += 1;
        else if ((b & 0xE0u) == 0xC0u) n += 2;
        else n += 3;
    }}
    return (n + {i}) & 255;
}}
""",
    )
    add(
        f"sliding_max_{i:02d}",
        f"""
int main(void) {{
    int a[10];
    for (int k = 0; k < 10; ++k) a[k] = (k * 9 + {i}) & 31;
    int m = a[0];
    for (int k = 1; k < 10; ++k) if (a[k] > m) m = a[k];
    return m;
}}
""",
    )
    add(
        f"transpose4_{i:02d}",
        f"""
int main(void) {{
    int m[4][4];
    int t[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = r * 4 + c + {i};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[c][r] = m[r][c];
    return t[1][2] & 255;
}}
""",
    )
    add(
        f"sat_add_{i:02d}",
        f"""
int main(void) {{
    int acc = 0;
    for (int k = 0; k < {12 + i}; ++k) {{
        acc += k * 3;
        if (acc > 200) acc = 200;
    }}
    return acc;
}}
""",
    )
    add(
        f"dot3_{i:02d}",
        f"""
int main(void) {{
    int a[3] = {{1, 2, {3 + i}}};
    int b[3] = {{{i}, 4, 5}};
    int d = 0;
    for (int k = 0; k < 3; ++k) d += a[k] * b[k];
    return d & 255;
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
    print(f"Wrote {len(SPECS)} loop-negative sources under {OUT}")
    if len(SPECS) < 80:
        raise SystemExit(f"need 80+ loop negatives, got {len(SPECS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
