# FORGE TODO

## [x] Logging module (logger)
Grow the minimal stderr logger in `core/include/forge/core/log.hpp` into a logger module that PRISM can use instead of its own.

Done (`core/include/forge/core/log.hpp`, tests `test_log`, `test_log_strip`, `log_min_level_cmake`):
- [x] Levels trace, debug, info, warn, error, fatal, off; global threshold plus per-module overrides (`FORGE_LOG=warn,graph=debug`), settable at run time (`configure`, `set_level`, `set_module_level`).
- [x] Sinks: stderr (default), file with size rotation, syslog on POSIX (journald picks it up on systemd hosts), Windows event log (`add_eventlog_sink`), callback sink for embedders, JSON-lines format (`FORGE_LOG_FORMAT=json`). One locked write per line.
- [x] Async sink (`add_async_sink`): bounded queue and a drain thread; drops are counted and reported; `flush()`; drained at `remove_sink` and at exit.
- [x] Compile-time strip: `FORGE_LOG_MIN` / `FORGE_LOG_MIN_LEVEL` macros and the `FORGE_LOG_MIN_LEVEL` CMake cache variable; the `FORGE_LOG_*` macros skip argument evaluation.
- [x] `std::format` call style, documented in [docs/support.md](docs/support.md).
- [x] The Windows event log sink compiles and its `eventlog_sink` test passes in the `windows-msvc` and `windows-mingw` CI jobs (PR #53).


No open items.
