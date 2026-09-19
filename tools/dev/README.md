# Optional maintainer scripts

These files are **not** part of the supported build, CI, or v2.0.22 installer
workflow. They were moved out of the repository root to reduce clutter. Use
them only if you understand what each script does.

RetDec Imortek is a specification-extraction decompiler. Normal development
uses **CMake presets** (`full-linux-debug` / `full-linux-release` on
Linux/WSL/macOS → `build/linux`; `full-windows-debug` /
`full-windows-release` → `build/windows`) and scripts under `scripts/`.
See [docs/BUILD_REFERENCE.md](../../docs/BUILD_REFERENCE.md).

**Paths:** Most shell scripts set `REPO_ROOT` from their location (`tools/dev`
→ repo root) and default `BUILD` to `$REPO_ROOT/build/linux`. Override with
environment variables (`BUILD`, `CONFIG`, `RETDEC_BUILD` for Python helpers)
if your binary directory differs. `fix_coverage_stamps.sh` uses `DEBUG_BUILD`
(default `build/linux`) and `COV_BUILD` (default
`$HOME/retdec-build/core-coverage` for a second tree).

| Script | Rough purpose |
|--------|----------------|
| `inspect_yaramod_archives.sh` | Inspect MinGW `.a` archives for yaramod/re2 (`YARAMOD`, `RE2_EXT` env overrides) |
| `run_llvmir2hll_corpus_coverage.sh` | Corpus compile + decompile + llvmir2hll coverage report (heavy; env overrides for paths) |
| `build_coverage.sh`, `collect_coverage.sh`, `run_cov_analysis.sh`, `src_coverage_detail.sh` | Historical coverage helpers |
| `check_optimizer_cov.sh`, `fix_coverage_stamps.sh`, `test_gcda_update.sh` | Coverage stamp / gcov debugging |
| `analyze_coverage.py`, `analyze_cov.py`, `check_stamps.py`, `check_stamps2.py` | Coverage analysis |
| `compile_corpus.sh`, `check_recompile.sh` | Local corpus compile / recompile probes |
| `debug_segfault.sh`, `debug_crashes.sh`, `crash_one.sh`, `find_ll_bug.sh` | Crash debugging |
| `generate_report.sh`, `gen_report.sh`, `gen_report2.sh` | Ad-hoc reports |
| `test_suite.sh`, `test_full.sh`, `run_final_test.sh`, `verify_fixes.sh` | Historical test wrappers (prefer `ctest --preset …`) |
| `kill_coverage.sh`, `debug_coverage.sh` | Coverage process helpers |
| `get_compile_cmd.py` | Compile-command lookup |
| `fix_serial.pl` | Serial-number / stamp fix helper |

For format and push gates, use `scripts/check_format.sh` and
`scripts/check_push_gates.sh`, not the scripts in this folder.
