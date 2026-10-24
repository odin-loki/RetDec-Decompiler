# MSVC incremental compile helpers

Windows `.cmd` scripts for **manual** capstone2llvmir rebuilds when iterating on a single ISA or leftover batch. They assume:

- Visual Studio **vcvars64** (paths are edited per machine in each script)
- An existing **`build/windows`** tree configured with the full Windows preset
- Working directory switched to `build/windows` inside the script

Run from the repo root, for example:

```bat
scripts\compile\compile_arm64_neon.cmd
```

These are maintainer/dev shortcuts, not part of CI or the main `cmake --build` flow.
