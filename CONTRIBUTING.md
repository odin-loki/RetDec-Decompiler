# Contributing to RetDec

Thank you for your interest in contributing. RetDec is maintained by **Odin Loch, trading as Imortek**.

## Where to start

- **[docs/developer_guide.md](docs/developer_guide.md)** — repository layout, code style, new pipeline stages, plugins, debugging, and the full pull-request checklist.
- **[docs/BUILD_REFERENCE.md](docs/BUILD_REFERENCE.md)** — CMake presets, directory layout (`build/linux`, `build/windows`), install, and packaging.
- **[docs/README.md](docs/README.md)** — documentation index and diagnostic environment variables.

## Build locally

**Linux / WSL**

```bash
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"
```

**Windows (MSVC + Qt6)**

```powershell
.\scripts\Install-RetdecWindowsDeps.ps1
.\scripts\windows_native_configure.ps1
.\scripts\windows_native_build.ps1
```

See [docs/WINDOWS_NATIVE_BUILD.md](docs/WINDOWS_NATIVE_BUILD.md) for CUDA and GUI deployment details.

## Run tests locally

```bash
ctest --test-dir build/linux --output-on-failure
```

On Windows, use `build/windows` instead. GUI tests require a display or headless mode — copy variables from [.env.example](.env.example).

CI workflows [ctest-linux.yml](.github/workflows/ctest-linux.yml) and [ctest-windows.yml](.github/workflows/ctest-windows.yml) mirror this but are **manual-only** in GitHub Actions.

## Commit and pull-request style

- Branch names: `feat/short-description`, `fix/issue-number`, `docs/topic`.
- Commit messages: **imperative mood**, present tense (e.g. `Add RISC-V lifting frontend`).
- One logical change per commit.
- Run `bash scripts/check_format.sh` before committing.
- Pull requests should include tests for new behaviour and update docs when adding stages or public APIs.

Full checklist: [docs/developer_guide.md#contributing](docs/developer_guide.md#contributing).

## Licence

By submitting a pull request, you agree that your contribution is licensed under the same terms as this project (**AGPL-3.0+ with Imortek Section 7 additions**, or your existing commercial licence from Imortek). See [LICENSE](LICENSE).

## Security and conduct

- Report security issues as described in [SECURITY.md](SECURITY.md).
- Community expectations: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
