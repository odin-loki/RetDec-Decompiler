/**
 * egraph_simplify.cl — Parallel equality-saturation over decompiled expressions.
 *
 * Each work-item processes one e-node.  Rewrite rules are encoded as
 * integer comparisons (no pointers, no recursion).  Saturation iterates
 * until no dirty flags remain.  A separate extraction pass assigns each
 * e-class its minimum C-distance representative.
 *
 * ─── Data model ────────────────────────────────────────────────────────────
 *  ENode  – elementary expression node stored in global arrays.
 *  EClass – equivalence class identified by a representative ID (union-find).
 *
 *  Opcodes (must stay in sync with EOpcode in ocl_egraph_simplifier.h):
 *    0  ENOP      – placeholder / invalidated node
 *    1  ELIT      – integer literal (lit_val)
 *    2  EVAR      – variable reference (aux = var id)
 *    3  EADD
 *    4  ESUB
 *    5  EMUL
 *    6  EDIV (unsigned)
 *    7  EAND
 *    8  EOR
 *    9  EXOR
 *   10  ESHL      – shift-left  (rhs = shift e-class, aux = constant shift if known)
 *   11  ESHR      – logical shift-right
 *   12  EASHR     – arithmetic shift-right
 *   13  ECAST     – width cast  (aux = target width in bytes)
 *   14  EDEREF    – pointer dereference  (*lhs)
 *   15  EARRAY    – array index  (lhs[rhs])
 *   16  EFIELD    – struct field (lhs->aux)
 *   17  EBITFIELD – bit-field extract (lhs >> aux) & mask_arg
 *
 *  C-distance per opcode (lower = more idiomatic C):
 *    ELIT=0, EVAR=0, EARRAY=1, EFIELD=1, EBITFIELD=2,
 *    EADD=3, ESUB=3, EDEREF=3, EMUL=4, EDIV=4,
 *    ECAST=5, EAND=6, EOR=6, EXOR=6, ESHL=6, ESHR=6, EASHR=6,
 *    ENOP=99
 */

/* ── Opcode constants ───────────────────────────────────────────────────── */
#define ENOP      0u
#define ELIT      1u
#define EVAR      2u
#define EADD      3u
#define ESUB      4u
#define EMUL      5u
#define EDIV      6u
#define EAND      7u
#define EOR       8u
#define EXOR      9u
#define ESHL      10u
#define ESHR      11u
#define EASHR     12u
#define ECAST     13u
#define EDEREF    14u
#define EARRAY    15u
#define EFIELD    16u
#define EBITFIELD 17u

#define NO_CLASS  0xFFFFFFFFu
#define MAX_SCORE 99u

/* ── C-distance score per opcode ────────────────────────────────────────── */
static uint cdist(uint opcode)
{
    switch (opcode) {
        case ELIT:      return 0u;
        case EVAR:      return 0u;
        case EARRAY:    return 1u;
        case EFIELD:    return 1u;
        case EBITFIELD: return 2u;
        case EADD:      return 3u;
        case ESUB:      return 3u;
        case EDEREF:    return 3u;
        case EMUL:      return 4u;
        case EDIV:      return 4u;
        case ECAST:     return 5u;
        case EAND:      return 6u;
        case EOR:       return 6u;
        case EXOR:      return 6u;
        case ESHL:      return 6u;
        case ESHR:      return 6u;
        case EASHR:     return 6u;
        default:        return MAX_SCORE;
    }
}

/* ── Union-find helpers ─────────────────────────────────────────────────── */
static uint uf_find(__global uint *parent, uint x)
{
    while (parent[x] != x) {
        /* path halving */
        uint gp = parent[parent[x]];
        parent[x] = gp;
        x = gp;
    }
    return x;
}

/* Returns true if a merge actually happened (ra != rb). */
static bool uf_union(__global uint *parent, __global uint *rank, uint ra, uint rb)
{
    ra = uf_find(parent, ra);
    rb = uf_find(parent, rb);
    if (ra == rb) return false;
    if (rank[ra] < rank[rb]) { uint t = ra; ra = rb; rb = t; }
    parent[rb] = ra;
    if (rank[ra] == rank[rb]) rank[ra]++;
    return true;
}

/* ── Literal-value query ────────────────────────────────────────────────── */
/**
 * Returns true and writes *val if e-class cls has exactly one ELIT node
 * whose e-class (after find) equals cls.  Scans up to n_nodes.
 */
static bool class_literal(__global const uint  *op,
                          __global const uint  *eclass,
                          __global const ulong *lit,
                          __global       uint  *parent,
                          uint n_nodes,
                          uint cls,
                          __private ulong *val)
{
    uint root = uf_find(parent, cls);
    for (uint i = 0u; i < n_nodes; i++) {
        if (op[i] == ELIT && uf_find(parent, eclass[i]) == root) {
            *val = lit[i];
            return true;
        }
    }
    return false;
}

/* ── Saturation kernel ──────────────────────────────────────────────────── */
/**
 * One work-item per e-node.
 *
 * Arrays (all length n_nodes):
 *   op[]     – opcode
 *   eclass[] – owning e-class
 *   lhs[]    – left-operand e-class (NO_CLASS if unused)
 *   rhs[]    – right-operand e-class (NO_CLASS if unused)
 *   aux[]    – auxiliary uint (shift amount / field offset / cast width)
 *   lit[]    – literal value (ELIT nodes)
 *
 * Union-find arrays (length n_classes):
 *   uf_parent[], uf_rank[]
 *
 * dirty[0] – set to 1 by any work-item that fires a rule.
 */
__kernel void retdec_egraph_saturate(
    __global       uint  *op,
    __global       uint  *eclass,
    __global       uint  *lhs,
    __global       uint  *rhs,
    __global       uint  *aux,
    __global       ulong *lit,
    __global       uint  *uf_parent,
    __global       uint  *uf_rank,
    __global       uint  *dirty,
                   uint   n_nodes,
                   uint   n_classes)
{
    uint id = (uint)get_global_id(0);
    if (id >= n_nodes) return;

    uint myop  = op[id];
    uint mycls = uf_find(uf_parent, eclass[id]);
    uint l     = (lhs[id] != NO_CLASS) ? uf_find(uf_parent, lhs[id]) : NO_CLASS;
    uint r     = (rhs[id] != NO_CLASS) ? uf_find(uf_parent, rhs[id]) : NO_CLASS;
    uint myaux = aux[id];

    bool fired = false;

    /* ── Rule 1: arithmetic identity with 0 ──────────────────────────── */
    /* x + 0 → x,  x - 0 → x,  x | 0 → x,  x ^ 0 → x */
    if ((myop == EADD || myop == ESUB || myop == EOR || myop == EXOR)
        && l != NO_CLASS && r != NO_CLASS)
    {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0UL) {
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
        }
    }

    /* x & ~0  → x  (mask = all-ones = 0xFFFFFFFFFFFFFFFF) */
    if (myop == EAND && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0xFFFFFFFFFFFFFFFFUL) {
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
        }
    }

    /* ── Rule 2: multiply by 1 ──────────────────────────────────────── */
    /* x * 1 → x */
    if (myop == EMUL && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 1UL) {
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
        }
        /* also: 1 * x → x */
        ulong lv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, l, &lv) && lv == 1UL) {
            fired |= uf_union(uf_parent, uf_rank, mycls, r);
        }
    }

    /* ── Rule 3: multiply by 0 ──────────────────────────────────────── */
    /* x * 0 → 0: find (or confirm) an ELIT(0) node; merge mycls with it */
    if (myop == EMUL && l != NO_CLASS && r != NO_CLASS) {
        ulong rv = 0, lv = 0;
        bool rz = class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0UL;
        bool lz = class_literal(op, eclass, lit, uf_parent, n_nodes, l, &lv) && lv == 0UL;
        if (rz || lz) {
            /* find any e-class that is ELIT(0) */
            for (uint i = 0u; i < n_nodes; i++) {
                if (op[i] == ELIT && lit[i] == 0UL) {
                    fired |= uf_union(uf_parent, uf_rank, mycls, uf_find(uf_parent, eclass[i]));
                    break;
                }
            }
        }
    }

    /* ── Rule 4: shift by 0 ─────────────────────────────────────────── */
    /* x >> 0 → x,  x << 0 → x */
    if ((myop == ESHL || myop == ESHR || myop == EASHR)
        && l != NO_CLASS && r != NO_CLASS)
    {
        ulong rv = 0;
        if (class_literal(op, eclass, lit, uf_parent, n_nodes, r, &rv) && rv == 0UL) {
            fired |= uf_union(uf_parent, uf_rank, mycls, l);
        }
    }

    /* ── Rule 5: bitfield extraction ────────────────────────────────── */
    /* (x >> k) & mask  →  EBITFIELD(x, k, popcount(mask))
     * Detect: EAND( ESHR/EASHR(x, k_class), mask_class )
     *   where k_class is ELIT(k), mask_class is ELIT(mask) with contiguous bits.
     */
    if (myop == EAND && l != NO_CLASS && r != NO_CLASS) {
        /* Check if l is a (logical/arithmetic) right-shift */
        for (uint si = 0u; si < n_nodes; si++) {
            if ((op[si] == ESHR || op[si] == EASHR)
                && uf_find(uf_parent, eclass[si]) == l
                && lhs[si] != NO_CLASS && rhs[si] != NO_CLASS)
            {
                ulong k_val = 0, mask_val = 0;
                bool have_k    = class_literal(op, eclass, lit, uf_parent, n_nodes, uf_find(uf_parent, rhs[si]), &k_val);
                bool have_mask = class_literal(op, eclass, lit, uf_parent, n_nodes, r, &mask_val);
                if (have_k && have_mask && mask_val != 0UL) {
                    /* Verify mask is a run of contiguous 1-bits. */
                    ulong m = mask_val;
                    /* Remove trailing zeros, then check if all-ones */
                    m >>= (uint)k_val;   /* shift to remove offset */
                    /* For a contiguous mask: m & (m+1) == 0 */
                    bool contiguous = (m != 0UL) && ((m & (m + 1UL)) == 0UL);
                    if (contiguous) {
                        /* Create a new EBITFIELD node if none already exists */
                        /* (We can't dynamically allocate; mark node id as rewritten) */
                        /* Instead reuse this AND node's opcode slot */
                        uint src_cls = uf_find(uf_parent, lhs[si]);
                        /* Rewrite this EAND node into EBITFIELD */
                        op[id]    = EBITFIELD;
                        lhs[id]   = src_cls;
                        rhs[id]   = NO_CLASS;
                        aux[id]   = (uint)k_val;
                        fired = true;
                    }
                }
                break;
            }
        }
    }

    /* ── Rule 6: cast collapsing ────────────────────────────────────── */
    /* CAST(CAST(x, w1), w2) → CAST(x, w2) when w2 <= w1 */
    if (myop == ECAST && l != NO_CLASS) {
        for (uint ci = 0u; ci < n_nodes; ci++) {
            if (op[ci] == ECAST
                && uf_find(uf_parent, eclass[ci]) == l
                && lhs[ci] != NO_CLASS)
            {
                if (myaux <= aux[ci]) {
                    /* outer cast is narrower or equal; bypass inner */
                    lhs[id] = lhs[ci];   /* point directly to inner operand */
                    fired = true;
                }
                break;
            }
        }
    }

    /* ── Rule 7: pointer arithmetic to array/field access ───────────── */
    /* DEREF(ADD(base, idx)) → ARRAY(base, idx) */
    if (myop == EDEREF && l != NO_CLASS) {
        for (uint ai = 0u; ai < n_nodes; ai++) {
            if (op[ai] == EADD
                && uf_find(uf_parent, eclass[ai]) == l
                && lhs[ai] != NO_CLASS && rhs[ai] != NO_CLASS)
            {
                /* Check if RHS is a literal (field offset) */
                ulong fld = 0;
                bool is_lit = class_literal(op, eclass, lit, uf_parent, n_nodes, uf_find(uf_parent, rhs[ai]), &fld);
                if (is_lit) {
                    op[id]  = EFIELD;
                    lhs[id] = uf_find(uf_parent, lhs[ai]);
                    rhs[id] = NO_CLASS;
                    aux[id] = (uint)fld;
                } else {
                    op[id]  = EARRAY;
                    lhs[id] = uf_find(uf_parent, lhs[ai]);
                    rhs[id] = uf_find(uf_parent, rhs[ai]);
                }
                fired = true;
                break;
            }
        }
    }

    if (fired) {
        atomic_or(dirty, 1u);
    }
}

/* ── Extraction kernel ──────────────────────────────────────────────────── */
/**
 * One work-item per e-class.  Scans all e-nodes to find the minimum
 * C-distance node in that class.  Writes best node index and score.
 *
 * best_node[cls]  – output: index of selected representative e-node
 * best_score[cls] – output: its C-distance score
 */
__kernel void retdec_egraph_extract(
    __global const uint *op,
    __global const uint *eclass,
    __global       uint *uf_parent,
    __global       uint *best_node,
    __global       uint *best_score,
                   uint  n_nodes,
                   uint  n_classes)
{
    uint cls = (uint)get_global_id(0);
    if (cls >= n_classes) return;

    uint root  = uf_find(uf_parent, cls);
    uint bnode = NO_CLASS;
    uint bsc   = MAX_SCORE + 1u;

    for (uint i = 0u; i < n_nodes; i++) {
        if (uf_find(uf_parent, eclass[i]) == root) {
            uint sc = cdist(op[i]);
            if (sc < bsc) {
                bsc   = sc;
                bnode = i;
            }
        }
    }

    best_node[cls]  = bnode;
    best_score[cls] = bsc;
}
