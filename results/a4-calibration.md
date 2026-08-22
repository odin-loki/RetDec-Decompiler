# A4 confidence calibration (observation)

Detector constants were **not fitted**. A reported 0.90 is not 90%
correct on this set.

- observations: 160 detections on the 100 loop-negatives
- ground truth: empty (every detection is a false positive)

## Reliability bins

| Reported confidence | n | empirical precision |
|--------------------|---|---------------------|
| 0.4-0.6 | 80 | 0.000 |
| 0.6-0.8 | 10 | 0.000 |
| 0.8-1.0 | 70 | 0.000 |

## Per detection kind

| Detection | n | empirical precision |
|-----------|---|---------------------|
| `algorithm:std::find_if` | 10 | 0.000 |
| `algorithm:std::transform` | 70 | 0.000 |
| `container:std::unordered_map<uint32_t, uint32_t>` | 80 | 0.000 |

Extract can still report B8 loop FP 0.000 while this table is
non-empty: generic `std::` algorithms are skipped and container
labels need confidence ≥ 0.8. Fitting would require changing
detector constants and re-scoring the 216-binary table; that
was not done.
