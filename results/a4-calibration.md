# A4 confidence calibration (observation)

Detector constants were **not fitted**. A reported 0.90 is not 90%
correct on this set.

- observations: 160 detections on the 100 loop-negatives
- ground truth: empty (every detection is a false positive)

## Reliability bins

| Reported confidence | n | empirical precision |
|--------------------|---|---------------------|
| 0.4-0.6 | 70 | 0.000 |
| 0.6-0.8 | 20 | 0.000 |
| 0.8-1.0 | 70 | 0.000 |

## Per detection kind

| Detection | n | empirical precision |
|-----------|---|---------------------|
| `algorithm:std::find_if` | 10 | 0.000 |
| `algorithm:std::transform` | 70 | 0.000 |
| `container:std::unordered_map<uint32_t, uint32_t>` | 80 | 0.000 |

Extract (`--no-stem-fallback`) reports 0 labelled binaries; the
rows above are raw `.config.json` detections that extract filters
(`std::transform`, `unordered_map` below container min 0.8).
Empirical precision is **0.000**. Fitting was not done.
