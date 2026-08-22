# B8 loop-containing negatives

FIR / histogram / Bresenham / box-blur / HTTP-verb / UTF-8 scan /
sliding-max / transpose / saturating-add / dot-product. Empty labels.
These *do* have loops, so they stress `strlen` / `atoi` / `dfs` heuristics
and the sort opcode bag. Any extracted label is a false positive.

- binaries requested: 100
- decompiled: 100
- binaries with any label: **0**
- false-positive rate: **0.000**

Remasured after `TransformDetector` rejected state-machine loops
(CondBranch ≥ 3 and Add < 3). HTTP-verb copy is gone.
`memcpy_loop-gcc-O0` still extracts Copy/Memcpy.

A4: confidences are **not fitted**. Configs may still contain
`std::transform` / `unordered_map` below extract thresholds.

No labels extracted (0 false positives on this run).
