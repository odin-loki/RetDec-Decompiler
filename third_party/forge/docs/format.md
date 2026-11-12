# .fpk file format (version 1)

The authoritative description is the header comment of `modelfile/include/forge/modelfile/fpk.hpp`; this page restates it. All integers are little-endian.

```
[header 64 B] [sections, each aligned to 64 B] [section table]

header : "FORGEPK1" | u32 version | u32 n_sections | u64 table_offset | u64 file_size | 32 B reserved
table  : n_sections x { char name[32]; u64 offset; u64 size; u8 blake3[32]; u8 pad[16] }   (96 B per entry)
```

Readers find sections through the table, so section order on disk does not matter. `writer` writes the other sections in name order, then `data`. `stream_writer` (used by the Hugging Face importer) writes `data` first, then the other sections, and patches the header last.

## Sections

| Name | Required | Contents |
|------|----------|----------|
| `manifest` | yes | JSON. Every importer writes `format` (`"fpk"`), `forge_format_version`, `architecture` and `session`. HF imports add `template`, `codec`, `bos_token_id`, `eos_token_id`, `max_position_embeddings`. GGUF imports add `source: "gguf"`, `template` and the same token/position fields. Both add `add_bos` (bool) when the source says whether prompts start with BOS (GGUF `tokenizer.ggml.add_bos_token`, HF `tokenizer_config.json` `add_bos_token`); without it, Qwen-family models get no BOS and others do. ONNX imports add `source: "onnx"`, `producer`, `onnx_opset`, `onnx_inputs`, `onnx_outputs` |
| `blueprint` | yes | Graph text (`modelfile/blueprint.hpp`): name, opset, params, states, inputs, graph, outputs and session kind (e.g. `generative_sequence`) |
| `tokenizer` | text models | Hugging Face style `tokenizer.json`. HF imports copy it; GGUF imports synthesise one from the GGUF vocabulary |
| `tensors` | yes | JSON array `[{name, codec, shape, offset, bytes, blake3}]`; `offset` is relative to the start of `data` |
| `data` | yes | Tensor bytes, each tensor aligned to 64 B, in the codec's block layout (`graph/codecs.hpp`) |
| `experts` | no | Minor revision 1 (manifest `forge_format_minor: 1`). JSON `{version, align: 4096, tensors: [{layer, proj (gate/up/down), name, codec, n_experts, offset, file_offset, expert_bytes, aligned}]}` indexing the stacked MoE expert tensors `[n_experts, rows, k]` read by `moe_ffn`. Expert e is bytes `[offset + e * expert_bytes, + expert_bytes)` of `data`; `aligned` means every expert slice starts on a 4 KiB file offset (one direct read per expert, groundwork for expert streaming). Older readers ignore the section |
| `signature` | no | `{"alg":"ed25519", "scheme":"forge-fpk-v1", "digest", "signatures":[{key_id, public_key, sig}]}` |
| `encryption` | no | `{alg, nonce, mac, key_id}`. `data` is sealed with XChaCha20-Poly1305; the `tensors` JSON is bound as associated data |

## Hashing and signing
- Each table entry carries the BLAKE3 hash of its section bytes as stored. Each `tensors` entry carries the hash of its (plaintext) bytes.
- `reader::verify()` checks both. Tensor hashes of an encrypted file can be checked only with the key.
- The signature covers a BLAKE3 digest of the scheme string followed by, for every section except `signature` in name order: name length, name, size and the BLAKE3 of its stored bytes. The digest is recomputed from the bytes on disk; the stored table hashes are not trusted.
- `key_id` is the first 4 bytes (8 hex digits) of the BLAKE3 of the public key.
- Several signatures can be present. `forge sign` on a signed file adds a signature if the digest still matches, and replaces an older signature by the same key.
- A file whose hashes fail `verify()` is not signed (`forge sign` refuses it).

## Encryption
- Only `data` is sealed. The manifest, blueprint, tokenizer, tensor index and section table stay readable, so `forge inspect`, section-hash checks and signature checks work without the key.
- Readers take the key from the constructor, or from `FORGE_MODEL_KEY` (hex) or `FORGE_MODEL_KEY_FILE`. Without one the reader opens in the `locked()` state, and reading `data` raises an error naming the key id.

## Tools
- `forge inspect <file> [--tensors] [--blueprint] [--verify]` shows the layout.
- `forge dump <file> <tensor> out.f32` extracts one tensor as raw f32.
- `forge keygen`, `forge sign`, `forge verify`, `forge encrypt`: see [modelfile.md](modelfile.md#examples).
