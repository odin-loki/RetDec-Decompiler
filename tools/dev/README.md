# Optional maintainer scripts

These files are **not** part of the supported build or CI workflow. They were moved out of the repository root to reduce clutter. Use them only if you understand what each script does.

**Paths:** Most shell scripts set `REPO_ROOT` from their location (`tools/dev` → repo root) and default `BUILD` to `$REPO_ROOT/build/linux`. Override with environment variables (`BUILD`, `CONFIG`, `RETDEC_BUILD` for Python helpers) if your binary directory differs. `fix_coverage_stamps.sh` uses `DEBUG_BUILD` (default `build/linux`) and `COV_BUILD` (default `$HOME/retdec-build/core-coverage` for a second tree).

| Script | Rough purpose |
|--------|----------------|
| `inspect_yaramod_archives.sh` | Inspect MinGW `.a` archives for yaramod/re2 (`YARAMOD`, `RE2_EXT` env overrides) |
| `run_llvmir2hll_corpus_coverage.sh` | Corpus compile + decompile + llvmir2hll coverage report (heavy; env overrides for paths) |
| Others (`*.sh`, `*.py`, …) | Historical coverage, stamp checks, debugging helpers |

For normal development, use **CMake presets** and scripts under `scripts/`; see [docs/BUILD_REFERENCE.md](../../docs/BUILD_REFERENCE.md).
