# A4 confidence calibration (observation)

Detector constants were **not fitted**. A reported 0.90 is not 90%
correct on this set.

- observations: 240 detections on the 100 loop-negatives
- ground truth: empty (every detection is a false positive)

## Reliability bins

| Reported confidence | n | empirical precision |
|--------------------|---|---------------------|
| 0.4-0.6 | 70 | 0.000 |
| 0.8-1.0 | 170 | 0.000 |

## Per detection kind

| Detection | n | empirical precision |
|-----------|---|---------------------|
| `algorithm:std::copy` | 10 | 0.000 |
| `algorithm:std::transform` | 70 | 0.000 |
| `container:ring_buffer` | 10 | 0.000 |
| `container:std::unordered_map<uint32_t, uint32_t>` | 70 | 0.000 |
| `sort:quicksort` | 80 | 0.000 |

`sort:quicksort` at 0.90 has empirical precision **0.000** here.
Fitting would require changing detector constants and re-scoring
the 216-binary table; that was not done.
