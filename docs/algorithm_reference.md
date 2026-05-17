# RetDec Algorithm Reference

This document describes the key algorithms used in RetDec, their complexity,
and implementation notes.

---

## Table of Contents

1. [Louvain Community Detection (Module Clustering)](#louvain)
2. [Myers Diff (Diff/Compare View)](#myers)
3. [FlashAttention-2 (AI Inference)](#flashattn)
4. [Mixture-of-Experts Routing (AI Inference)](#moe)
5. [Byte-Pair Encoding Tokeniser (AI Inference)](#bpe)
6. [Online Softmax (FlashAttention)](#softmax)

---

## Louvain Community Detection {#louvain}

**Location**: `include/retdec/module_cluster/module_cluster.h`

### Problem

Given a call graph G = (V, E) where V = functions and E = call edges
(weighted by call frequency), partition V into modules that maximise the
Newman-Girvan modularity Q.

### Algorithm

Modularity of a partition:

```
Q = (1 / 2m) Σ_{ij} [ A_{ij} - γ * k_i * k_j / 2m ] δ(c_i, c_j)
```

where:
- m = total edge weight
- A_{ij} = edge weight between i and j
- k_i = degree of node i
- γ = resolution parameter (default 1.0)
- δ(c_i, c_j) = 1 if i and j are in the same community

**Phase 1 (node moving)**: For each node i, compute the modularity gain ΔQ
of moving i from its current community to each neighbouring community.
Move to the community with maximum positive ΔQ.  Repeat until no improvement.

```
ΔQ(i → C) = k_{i,in}(C) / m - γ * Σ_C * k_i / (2m²)
```

**Phase 2 (graph compression)**: Each community becomes a super-node.
Inter-community edges are summed.  Phase 1 repeats on the compressed graph.

**Refinements applied after Louvain**:
1. *String locality*: functions sharing string-pool references are merged.
2. *Debug symbols*: functions with identical `sourceFile` are merged.

**Complexity**: O((N + M) · D) where D = number of Louvain passes (typically ≤ 20).

---

## Myers Diff Algorithm {#myers}

**Location**: `include/retdec/gui/panels/diff_panel.h`

### Problem

Given two sequences A (length N) and B (length M), compute the shortest edit
script (SES): the minimum number of insertions and deletions to transform A into B.

### Algorithm

The edit distance D is the minimum number of edits (insert or delete).
Myers defines a D-path as a path from (0,0) to (N,M) in an edit graph with
exactly D non-diagonal edges.

**Forward phase**: For d = 0, 1, 2, ...:
  For each diagonal k = −d, −d+2, ..., d:
    Compute the furthest-reaching D-path along diagonal k.
    Extend diagonally (snake) as far as possible.
    If we reach (N, M): done.

**Hirschberg midpoint** (divide and conquer, O(N+M) space):
  Find the midpoint of the shortest edit path by running forward and
  backward DP simultaneously until they meet.
  Recurse on the two halves.

This gives O((N+M)D) time and O(N+M) space (vs O(ND) for the basic algorithm).

**Complexity**:
- Time: O((N + M) · D)
- Space: O(N + M)
- D = edit distance ≤ N + M

---

## FlashAttention-2 {#flashattn}

**Location**: `include/retdec/qwen3/qwen3_attention.h`

### Problem

Standard attention: O(n²) memory for the N×N attention matrix, which becomes
prohibitive for long sequences.

### Algorithm

FlashAttention-2 tiles the computation into blocks of size B_r × B_c to avoid
materialising the full N×N matrix.

For each row tile of Q (size B_r):
  For each column tile of K, V (size B_c):
    Compute S = Q_tile · K_tile^T   (B_r × B_c)
    Apply RoPE to Q and K heads
    Running online softmax (numerically stable):
      m_new = max(m_old, rowmax(S))
      l_new = exp(m_old - m_new) · l_old + rowsum(exp(S - m_new))
      O_new = diag(exp(m_old - m_new)) · O_old + exp(S - m_new) · V_tile
  Normalise: O = O / l

**Grouped-Query Attention (GQA)**: K and V heads are shared across groups of Q
heads, reducing KV cache memory by factor `n_q_heads / n_kv_heads`.

**Paged KV Cache**: K/V entries are stored in fixed-size blocks allocated from
a pool.  A block table maps (layer, sequence_position / block_size) to block
indices.  This eliminates fragmentation for variable-length sequences.

**RoPE (Rotary Position Embeddings)**:
```
x_rot = x · cos(θ) + rotate_half(x) · sin(θ)
θ_i = position / base^(2i / d_head)
```

**Complexity**:
- Time: O(N · d · B_r · B_c) = O(N² · d) — same asymptotic as standard, but
  HBM accesses are reduced from O(N² + Nd) to O(Nd).
- Memory: O(N · d) — no N×N materialisation.

---

## Mixture-of-Experts Routing {#moe}

**Location**: `include/retdec/qwen3/qwen3_moe.h`

### Problem

In a MoE transformer layer, the FFN is replaced by E expert networks.
For each token, only the top-K experts are activated.

### Algorithm

**Router**: Linear projection W_g ∈ ℝ^(d_model × E):
```
logits = x · W_g                    (E-dim)
probs  = softmax(logits)             (E-dim)
```

**Top-K selection**: `std::nth_element` in O(E) time selects K largest.

**Weight normalisation** (sum to 1 over selected experts):
```
w_i = probs[top_k[i]] / Σ_j probs[top_k[j]]
```

**Expert FFN** (SwiGLU):
```
y_i = W_down · (SiLU(W_gate · x) ⊙ W_up · x)
```

**Output**:
```
output = Σ_i w_i · y_i + shared_expert(x)
```

The shared expert is always active regardless of routing.

**Load balancing**: `MoeLoadMonitor` tracks per-expert activation fraction
to detect hot/cold experts during inference.

**Complexity**: O(K · d_model · d_ff) per token — vs O(E · d_model · d_ff)
for dense FFN.  Typically K = 4, E = 64: 16× reduction in FFN FLOPs.

---

## Byte-Pair Encoding Tokeniser {#bpe}

**Location**: `include/retdec/qwen3/qwen3_tokenizer.h`

### Problem

Map text ↔ integer token IDs using a vocabulary learned by BPE.

### Algorithm

**Encoding**:
1. Byte-level pre-tokenisation: split on regex `\p{L}+|\p{N}+|\p{P}+|.`
2. For each word: start with character-level tokens.
3. Iteratively merge the most frequent adjacent pair per the merge table.
4. Lookup final tokens in the vocabulary table.

**Chat template** (ChatML format):
```
<|im_start|>system\n{system}\n<|im_end|>\n
<|im_start|>user\n{user}\n<|im_end|>\n
<|im_start|>assistant\n
```

**Special tokens**:
- `<|im_start|>` = 151644
- `<|im_end|>` = 151645
- EOS = 151645

**Complexity**: O(N · V) worst case, O(N log V) with merge priority queue.

---

## Online Softmax {#softmax}

**Location**: Used inside FlashAttention-2 (see above)

### Problem

Compute softmax(x₁, ..., xₙ) numerically stably in a single pass,
accumulating into an output sum.

### Algorithm (three-pass → two-pass → one-pass online)

Standard two-pass:
```
m = max(x_i)
s = Σ exp(x_i - m)
y_i = exp(x_i - m) / s
```

Online single-pass (Dao et al.):
```
On seeing x_j:
  m' = max(m, x_j)
  s' = s · exp(m - m') + exp(x_j - m')
  O' = O · exp(m - m') + V_j · exp(x_j - m')
  m  = m'
  s  = s'
```

This allows computing FlashAttention tiles without reading the data twice.

**Numerical stability**: exp(x_j - m') ≤ 1 always, so no overflow.
