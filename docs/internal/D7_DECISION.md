# Decision D7 — Product positioning (CLOSED)

**Status:** Settled — option **(b)**  
**Date:** 2026-08-08  
**Version:** v1.8.0

## Decision

RetDec Imortek is a **specification-extraction tool that contains a decompiler**.

Algorithm recovery, semantic export, concurrency/container detection, and
verified offline neural refinement are the product. Recovered C pseudocode is a
supporting artefact for human review, not the primary benchmark headline.

## Rationale

1. Post-pipeline analysis (`algo_recover`, `sort_detect`, `container_detect`,
   etc.) feeds semantic JSON export — not `llvmir2hll` emission.
2. Pseudocode parity with stock RetDec is expected; competing on C quality alone
   is not defensible (MASTER-UPGRADE-PLAN Part 3).
3. Offline local model + verification gates are differentiators no cloud-API
   decompiler ships for defence buyers.

## Implications

| Area | Lead with |
|------|-----------|
| README / marketing | Specification extraction, algorithm recovery F1 |
| Benchmarks | `algorithm_recovery` metric, honest weak DecompileBench numbers |
| Demo (`scripts/demo.sh`) | Semantic detections + F1, not C diff |
| Whitepaper | Executive summary aligned to (b) |
| Roadmap | rellic/LLVM improve pseudocode; not the v1 headline |

## References

- README opening paragraph
- [docs/BENCHMARKS.md](../BENCHMARKS.md)
- [MASTER-UPGRADE-PLAN.md](MASTER-UPGRADE-PLAN.md) Part 3, Decision D7
