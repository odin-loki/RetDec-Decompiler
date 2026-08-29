# GUI Phase D decisions (closed)

## CUDA in hot analysis paths

**Decision:** CPU-only default for analysis and CI. CUDA remains optional for
neural inference (`RETDEC_ENABLE_CUDA_ACCEL`, llama.cpp CUDA backend) but is
**not** wired into container/sort/algo detector hot paths in v2.

**Rationale:** Detectors run on LLVM IR and SSA; GPU offload needs batching and
`LLVMContext` threading constraints (see `docs/PERFORMANCE.md`). CI presets use
`-DRETDEC_ENABLE_CUDA_ACCEL=OFF`.

**Future:** Revisit when profiling (`flamegraph_profile.sh`) shows a detector
pass dominates wall time on target hardware.

## AI assistant in GUI

**Decision:** The GUI ships an **AI Assistant** Tools window (`AIAssistantPanel`).
It is not a permanent bottom dock. Presence is not a neural-chat quality claim.
There is no in-tree Qwen3/FlashAttention stack (`C-QWEN3-GPU` withdrawn).

| Path | Tool |
|------|------|
| GUI | Tools → AI Assistant (`AIAssistantPanel`) — local GGUF load when configured |
| CLI / batch | `RETDEC_NEURAL_REFINE=1` + `RETDEC_NEURAL_MODEL` (GGUF). There is no `retdec-qwen3-runner`. |
| Optional HTTP | User-run Ollama on localhost (not bundled) |
| In-process | `RETDEC_ENABLE_LLAMACPP=ON` for neural refinement hook only |

**Rationale:** The old in-tree Qwen panel is gone (`C-QWEN3-GPU` withdrawn).
The Tools window is a local chat surface; post-decompile refine stays
env-gated (`docs/NEURAL_REFINEMENT.md`). Do not treat either path as a
quality guarantee.
