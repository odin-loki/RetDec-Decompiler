# Neural model allowlist

`models.json` is the SHA-256 allowlist checked when a GGUF is loaded
(`RETDEC_NEURAL_REFINE=1` + `RETDEC_NEURAL_MODEL`). Verification is **on
by default at load**. Neural stays off unless those env vars are set.

The shipped file pins Unsloth `Qwen3.5-9B-Q4_K_M.gguf`
(`03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8`).
An empty list (or a missing file) **refuses** every model. Unknown hashes
are still refused.

## Adding a model

1. Compute the SHA-256 of the GGUF (lowercase hex, 64 characters).
2. Append an object to the `models` array:

```json
{
  "models": [
    {
      "name": "Qwen3.5-9B-Q4_K_M.gguf",
      "sha256": "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8"
    }
  ]
}
```

`name` is documentation only. Matching is by `sha256`.

## Release assets

The GGUF is **5.68 GB**. GitHub Release files are capped at 2 GB, so
`release-installers.yml` job `neural-gguf` (after the OS installer builds)
uploads `Qwen3.5-9B-Q4_K_M.gguf.partaa` / `.partab` / … plus
`Qwen3.5-9B-Q4_K_M.SHA256SUMS`. It is **not** inside the Windows zip,
Linux tarball, or macOS tarball.

Reassemble, then point the decompiler at the file:

```bash
bash scripts/join_qwen_gguf.sh
# or:  .\scripts\join_qwen_gguf.ps1
# or download whole from Hugging Face:
bash scripts/fetch_qwen_gguf.sh
```

## Overrides (load path only)

| Variable | Effect |
|---|---|
| `RETDEC_NEURAL_MODELS_JSON` | Use this allowlist file instead of `support/models.json`. |
| `RETDEC_NEURAL_MODEL_SHA256` | If set, the file hash must also equal this value. |
| `RETDEC_NEURAL_ALLOW_UNVERIFIED=1` | Skip the allowlist (and only the allowlist). Filename and GGUF-header multimodal checks still run. |

There is no `--neural-allow-unverified` CLI flag (the decompiler has no
`--neural-*` options). Use the env var.

Multimodal projectors are rejected from GGUF metadata
(`general.architecture` contains `clip` or `projector`, or
`general.name` contains `mmproj`) and from the filename (`mmproj`,
`-vl-`, `_vl_`).
