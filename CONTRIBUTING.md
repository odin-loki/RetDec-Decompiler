# Contributing to RetDec

Thank you for your interest in contributing. **RetDec Imortek** is a
specification-extraction decompiler maintained by **Odin Loch, trading as
Imortek** (version **2.0.22**). Algorithm recovery, semantic export, and
optional offline neural refinement are the product; recovered C is a
supporting artefact.

## Where to start

- **[docs/developer_guide.md](docs/developer_guide.md)** — repository layout, code style, new pipeline stages, plugins, debugging, and the full pull-request checklist.
- **[docs/BUILD_REFERENCE.md](docs/BUILD_REFERENCE.md)** — CMake presets, directory layout (`build/linux`, `build/windows`), install, and packaging.
- **[docs/README.md](docs/README.md)** — documentation index, CI table, and diagnostic environment variables.
- **[docs/CLAIMS.md](docs/CLAIMS.md)** — do not invent F1, CUDA-pipeline, or speed figures.

## Build locally

`CMakeLists.txt` requires **CMake 3.13**. [CMakePresets.json](CMakePresets.json)
requires **CMake 3.26**. CUDA acceleration is **off** (`RETDEC_ENABLE_CUDA_ACCEL`)
and is not wired into the decompiler.

**Linux / WSL / macOS** (Unix presets write `build/linux/`)

```bash
bash scripts/wsl_configure_nosudo.sh   # cmake --preset full-linux-debug (needs Qt 6)
cmake --build build/linux -j"$(nproc)"
```

On macOS the same `full-linux-*` presets apply. `ctest-macos` uses
`full-linux-release` on `macos-latest` (arm64).

**Windows (MSVC + Qt6)**

```powershell
.\scripts\Install-RetdecWindowsDeps.ps1
.\scripts\windows_native_configure.ps1
.\scripts\windows_native_build.ps1
```

See [docs/WINDOWS_NATIVE_BUILD.md](docs/WINDOWS_NATIVE_BUILD.md) for Qt
deployment. CUDA Toolkit is optional and unused by the default pipeline.

CLI-only without mandating Qt: `cmake --preset core-debug` (or
`core-release`).

## Run tests locally

```bash
ctest --test-dir build/linux --output-on-failure
```

On Windows, use `build/windows` instead. GUI tests need a display or
`RETDEC_GUI_HEADLESS=1` / `--headless` — copy variables from
[.env.example](.env.example).

LLVM-free gates (no network): `./scripts/standalone_check.sh`,
`./scripts/standalone_fuzz.sh --replay`, `./scripts/verify_esbmc.sh`.
See [docs/STANDALONE_CHECK.md](docs/STANDALONE_CHECK.md).

**Do not delete, skip, `DISABLED_`-prefix, or loosen a test.** A failing
test is a finding — report it.

CI that mirrors a full build:

| Workflow | When it runs |
|----------|----------------|
| [ctest-linux.yml](.github/workflows/ctest-linux.yml) | Push / PR to `main` + dispatch |
| [ctest-macos.yml](.github/workflows/ctest-macos.yml) | Push / PR to `main` + dispatch |
| [ctest-windows.yml](.github/workflows/ctest-windows.yml) | Nightly schedule + dispatch |

Every public workflow is listed in [docs/README.md](docs/README.md).

## Commit and pull-request style

- Branch names: `feat/short-description`, `fix/issue-number`, `docs/topic`.
- Commit messages: **imperative mood**, present tense (e.g. `Fix loop-bound jump analysis`).
- One logical change per commit.
- Run `bash scripts/check_format.sh` before committing.
- Pull requests should include tests for new behaviour and update docs when adding stages or public APIs.
- Never edit `deps/llvm/`. Never change a public header only to make an implementation compile.

Full checklist: [docs/developer_guide.md#contributing](docs/developer_guide.md#contributing).

## Licence

By submitting a pull request, you agree to [CLA.md](CLA.md) (outbound
relicensing grant to Imortek). Without that grant, a contribution cannot be
included in the commercial licence. The **CLA Assistant** check comments on
the PR; sign with the exact sentence in `CLA.md`. Dependabot is allowlisted.

See [LICENSE](LICENSE) (dual AGPL-3.0+ / commercial).

## Security and conduct

- Report security issues as described in [SECURITY.md](SECURITY.md).
  Decompiling untrusted binaries is intended use.
- Community expectations: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
