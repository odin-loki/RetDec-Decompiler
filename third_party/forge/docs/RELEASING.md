# Releasing FORGE

## Concepts
`tools/release.py` drives a release that is reproducible and can be signed. During `build`, `SOURCE_DATE_EPOCH` is set from the last commit, so rebuilding the same commit should give identical archives and SBOM as long as `FORGE_REPRODUCIBLE=ON` (the default). `ci/repro_check.sh` checks that two clean Release builds give bit-identical `forge`, `forge-serve` and `libforge.so`.

## Commands (`tools/release.py --help`)

| Command | Effect |
|---------|--------|
| `version` | Prints the version from `project(FORGE VERSION ...)` in `CMakeLists.txt` (currently `0.1.0`) |
| `bump X.Y.Z[-pre]` | Sets the project VERSION in `CMakeLists.txt` (without the pre-release suffix) and `VERSION_NAME` in `bindings/android/gradle.properties`. It also tries `FORGE_VERSION_STRING` in `inference/include/forge.h`, which does not exist, so that edit is skipped. `libforge` follows `${PROJECT_VERSION}` |
| `changelog [--since TAG]` | Adds a section at the top of `CHANGELOG.md` from `git log --no-merges` since the last tag (or the whole history), grouped by keywords in the subject. Creates the file if missing (there is none yet) |
| `build [--preset release] [--skip-tests] [--sign-key PEM] [--out DIR]` | `cmake --preset`, `cmake --build --preset` (`-j` up to 4), `ctest -LE slow`, CPack binary and source archives, the SPDX SBOM (`tools/sbom.py`), `SHA256SUMS`, and with `--sign-key` a detached Ed25519 `.sig` per file (`openssl pkeyutl -rawin`). Output goes to `dist/<version>/` |
| `verify DIR [--pub PUB]` | Re-checks `SHA256SUMS`, and every `.sig` if a public key is given. Exits 1 on any mismatch |
| `tag` | Creates the annotated tag `vX.Y.Z`. Refuses if the working tree is not clean. Never pushes |

## Procedure
1. `tools/release.py bump 1.0.0`
2. `tools/release.py changelog`, edit `CHANGELOG.md` by hand, and commit.
3. `tools/release.py build --sign-key release-ed25519.pem`
4. `tools/release.py verify dist/1.0.0 --pub release-ed25519.pub`
5. `tools/release.py tag`, then `git push origin v1.0.0`.

To generate a signing key:
```sh
openssl genpkey -algorithm ed25519 -out release-ed25519.pem
openssl pkey -in release-ed25519.pem -pubout -out release-ed25519.pub
```

## Platform notes and limits
- `build` looks for the build tree in `build/<preset>`. That matches `release`, `dev`, `asan` and `android-arm64`, but not `windows-mingw` (`build/win64`) or `macos-osxcross` (`build/macos`), so `build --preset` with those two fails at the test or package step. There is no build preset for `ios`.
- `cmake --workflow --preset release` runs configure, build, test (excluding label `slow`) and package (TGZ) without the SBOM, checksums or signatures.
- Signing needs an `openssl` with Ed25519 and `pkeyutl -rawin` (OpenSSL 3).
- **Only `version` was run while writing this page (it prints `0.1.0`).** The other commands have not been run, and the repository has no release tags yet.
