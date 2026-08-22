# B8 loop-containing negatives

FIR / histogram / Bresenham / box-blur / HTTP-verb / UTF-8 scan /
sliding-max / transpose / saturating-add / dot-product. Empty labels.
These *do* have loops, so they stress `strlen` / `atoi` / `dfs` heuristics
and the sort opcode bag. Any extracted label is a false positive.

- binaries requested: 100
- decompiled: 100
- binaries with any label: **20**
- false-positive rate: **0.200**

Remasured after removing `MergesortDetector`'s merge-loop-only
0.55 floor. Sort labels are gone. Remaining FPs are `box_blur` →
`RingBuffer` and `http_verb` → `Copy`/`Memcpy`. Assigned idioms
(`Atoi`/`Strlen`/`DFS`/`Varint`) still did not fire.

A4: confidences are **not fitted**. The table below is an observation
on this negative set only. Detector constants were not changed.

## False-positive confidence (A4 observation)

| Detection | n | mean conf | min | max |
|-----------|---|-----------|-----|-----|
| `algorithm:std::copy` | 10 | 0.850 | 0.850 | 0.850 |
| `algorithm:std::transform` | 10 | 0.850 | 0.850 | 0.850 |
| `container:ring_buffer` | 10 | 1.000 | 1.000 | 1.000 |
| `container:std::unordered_map<uint32_t, uint32_t>` | 10 | 0.450 | 0.450 | 0.450 |

## Positives (false positives)

- `box_blur_00-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_01-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_02-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_03-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_04-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_05-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_06-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_07-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_08-gcc-O0`: CircularBuffer, RingBuffer
- `box_blur_09-gcc-O0`: CircularBuffer, RingBuffer
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
