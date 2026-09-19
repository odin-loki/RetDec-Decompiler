# experimental — scaffold only

Plan.md `DEAD-05`. Empty `task_*_scaffold` anchors for future tasks.json
items. This is **not** a product pipeline at v2.0.22.

- CMake option `RETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD` defaults **OFF**.
- `src/CMakeLists.txt` adds this directory only when that option is ON.
- Nothing in `src/retdec` links `retdec::experimental`.

The specification-extraction decompiler (buildable C, semantic detections,
optional llama.cpp, Qt 6 GUI) does not use this target.

Research-only enable:

```bash
cmake --preset full-linux-debug -DRETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD=ON
```

Source: `pipeline_stub_anchors.cpp`. Include path remains
`retdec/experimental/pipeline_stub_anchors.h`.
