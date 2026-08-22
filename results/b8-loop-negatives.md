# B8 loop-containing negatives

FIR / histogram / Bresenham / box-blur / HTTP-verb / UTF-8 scan /
sliding-max / transpose / saturating-add / dot-product. Empty labels.
These *do* have loops, so they stress `strlen` / `atoi` / `dfs` heuristics
and the sort opcode bag. Any extracted label is a false positive.

- binaries requested: 100
- decompiled: 100
- binaries with any label: **10**
- false-positive rate: **0.100**

Remasured after `RingBufferDetector` required an `And` wrap mask
instead of any `Div`. Box-blur `/ 3` is gone. Remaining FPs are
HTTP-verb → `Copy`/`Memcpy`. Recovered SSA has empty And uses, so
true `ring_buffer` binaries also miss until `llvm_to_ssa` attaches
immediates.

A4: confidences are **not fitted**. The table below is an observation
on this negative set only. Detector constants were not changed.

## False-positive confidence (A4 observation)

| Detection | n | mean conf | min | max |
|-----------|---|-----------|-----|-----|
| `algorithm:std::copy` | 10 | 0.850 | 0.850 | 0.850 |
| `container:std::unordered_map<uint32_t, uint32_t>` | 10 | 0.450 | 0.450 | 0.450 |

## Positives (false positives)

- `http_verb_00-gcc-O0`: Copy, Memcpy
- `http_verb_01-gcc-O0`: Copy, Memcpy
- `http_verb_02-gcc-O0`: Copy, Memcpy
- `http_verb_03-gcc-O0`: Copy, Memcpy
- `http_verb_04-gcc-O0`: Copy, Memcpy
- `http_verb_05-gcc-O0`: Copy, Memcpy
- `http_verb_06-gcc-O0`: Copy, Memcpy
- `http_verb_07-gcc-O0`: Copy, Memcpy
- `http_verb_08-gcc-O0`: Copy, Memcpy
- `http_verb_09-gcc-O0`: Copy, Memcpy
