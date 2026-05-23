# Formal Verification Bridge (Frama-C)

One-page export path from RetDec decompiled C to [Frama-C](https://www.frama-c.com/) for deductive verification and value analysis. **Documentation only** — no Frama-C integration ships in-tree.

---

## Goal

Analysts who need **partial correctness evidence** on security-critical functions can round-trip RetDec output into Frama-C ACSL-annotated C, run `-val` (value analysis) or `-wp` (weakest-precondition), and treat results as **supplementary** — not a proof of full binary fidelity.

---

## Export path

```
  binary.exe
      │
      ▼
  retdec-decompiler -o module.c --select-functions verify_me,fnc2  binary.exe
      │
      ▼
  module.c  +  module.c.config.json
      │
      ▼  (manual or scripted)
  frama-c-prepare.sh   ← strip RetDec meta comments, fix types, add ACSL stubs
      │
      ▼
  module_frama.c
      │
      ▼
  frama-c -val -main verify_me module_frama.c
  frama-c -wp  -proven-annotations module_frama.c
```

### Step 1 — Scoped decompilation

Decompile only functions under review to reduce noise:

```bash
retdec-decompiler -o out.c --select-functions parse_header,check_bounds input.exe
```

Prefer **quality** profile / default backend (no `--backend-no-opts`) for clearer control flow.

### Step 2 — Hygiene script (stub)

Create a team-local script that:

1. Removes RetDec meta comments (`// ----- Meta -----`, address comments).
2. Replaces unsupported types (`int64_t` width mismatches, vararg thunks) with Frama-C-friendly typedefs.
3. Inserts `/*@ requires ... ensures ... @*/` stubs for entry functions (Frama-C requires user annotations for `-wp`).
4. Marks unknown callees with `//@ assigns \nothing;` or wraps them in `extern` stubs.

Example skeleton (`scripts/frama_c_prepare_stub.sh` — not shipped):

```bash
#!/usr/bin/env bash
set -euo pipefail
in="${1:?usage: frama_c_prepare_stub.sh in.c out.c}"
out="${2:?}"
grep -v '^// -----' "$in" | grep -v '^// Function:' > "$out"
echo "/* ACSL: add contracts before verification */" >> "$out"
```

### Step 3 — Frama-C invocation

```bash
# Value analysis — find definite runtime errors (UB, out-of-bounds when annotations exist)
frama-c -val -main check_bounds module_frama.c

# WP — prove annotated properties (requires ACSL on check_bounds)
frama-c -wp -rte -main check_bounds module_frama.c
```

---

## Limitations (explicit)

| Topic | RetDec → Frama-C gap |
|-------|----------------------|
| Memory model | Decompiled pointers may not match original provenance; `-val` may report false alarms |
| Loops | Structuring may emit `while(1)` with breaks; loop invariants must be supplied manually |
| Inline asm | Lost or modeled as opaque calls |
| Floating point | x87/SSE lowering may not match IEEE assumptions Frama-C expects |
| Whole program | Only exported/selected functions; no automatic linking against real CRT |

**Invariant:** Treat Frama-C results as analysis on the **decompiled model**, not on the original binary. Cross-check critical claims against disassembly or symbolic execution.

---

## Future integration hooks

- **`scripts/retdec_export_frama.py`** — parse `.config.json` function list, emit Frama-C driver Makefile.
- **GUI** — Analysis → Export for Verification… (copy bundle + README).
- **ACSL templates** — per-ABI calling convention stubs in `support/formal/`.

---

## References

- [pipeline_stage_map.md](pipeline_stage_map.md) — Stage 28 output validation
- [PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md) — diagnostic / approximation invariants
- Frama-C user manual: https://frama-c.com/html/documentation.html
