# Contributing

## Changing code
1. Build and test (see [docs/building.md](docs/building.md) and [docs/testing.md](docs/testing.md)):
   ```sh
   cmake --preset dev && cmake --build build/dev -j4
   ctest --test-dir build/dev -LE "slow|emu" --output-on-failure
   ```
   This is the test set CI runs on Linux, macOS and Windows.
2. Code is C++23. Cover each change with a test under `tests/`. Tests that need hardware, a runtime or an SDK exit 77 when it is missing, so ctest reports them as skipped.
3. Ops are declared in `ops/defs/*.fspec`. Generated code is not edited by hand; see [ops/README.md](ops/README.md).
4. Keep the code portable: CI builds Linux (GCC and Clang), macOS, iOS, Windows (MSVC and MinGW), FreeBSD and Android on every pull request.

## Changing docs
1. `docs/reference.md` is generated. After changing public headers or the op specs, run `python3 tools/gen_docs.py`; the `docs_up_to_date` test fails otherwise.
2. The guides' examples in `docs/examples/` build and run as `doc_example_*` tests. Keep them compiling when you change an API.
3. Write what the code does now. Label anything that has not been run. Update [docs/STATUS.md](docs/STATUS.md) when you test on new hardware or a new platform.

## Merging
Do not run `gh pr merge` directly. Use:
```sh
ci/merge-when-green.sh <pr-number> [gh pr merge options, e.g. --merge]
```
It refuses to merge when `gh pr checks` reports any check as failed or still pending, when no checks have been reported yet, or when any check's state is other than `SUCCESS` or `SKIPPED`. This covers every CI job, including iOS, FreeBSD and other jobs that are not marked required. If all checks passed it runs `gh pr merge <pr-number>` with the remaining arguments.

## Licensing
FORGE is dual licensed (see [LICENSE](LICENSE) and [COMMERCIAL-LICENCE.md](COMMERCIAL-LICENCE.md)). By contributing you agree that your contribution can be distributed under both.
