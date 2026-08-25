# experimental — scaffold only

Plan.md `DEAD-05`. Empty `task_*_scaffold` anchors for future tasks.json
items. This is **not** a product pipeline.

- CMake option `RETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD` defaults **OFF**.
- `src/CMakeLists.txt` adds this directory only when that option is ON.
- Nothing in `src/retdec` links `retdec::experimental`.

Include path remains `retdec/experimental/pipeline_stub_anchors.h`.
