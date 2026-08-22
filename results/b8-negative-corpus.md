# B8 negative corpus

200+ binaries whose sources are not target algorithm-recovery labels
(unit conversion, clamp, lerp, date, BMI, PID, flags, IPv4, mortgage stub,
log level). Ground truth is empty. Any extracted label is a false positive.

- binaries requested: 220
- decompiled: 220
- binaries with any label: **0**
- false-positive rate: **0.000**

No labels extracted (0 false positives on this run).

These sources are loop-free numeric / clamp / flag / log-level programs, not
parsers or network stacks. They satisfy the 200+ empty-label requirement.
They do not stress `strlen`/`atoi`/`dfs` loop heuristics.
