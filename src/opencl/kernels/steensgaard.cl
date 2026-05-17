/**
 * @file src/opencl/kernels/steensgaard.cl
 * @brief Parallel Steensgaard points-to analysis (heap alias layer).
 *
 * ## Constraint types
 *   CON_COPY    (0): a := b       — copy; union ECR(a) with ECR(b)
 *   CON_ADDR_OF (1): a := &b      — address-of; union pts_to(ECR(a)) with ECR(b)
 *   CON_STORE   (2): *a := b      — store;  union pts_to(ECR(a)) with ECR(b)
 *   CON_LOAD    (3): a := *b      — load;   union ECR(a) with pts_to(ECR(b))
 *
 * ## Data layout
 *   parent[N]        – DSU parent; parent[i]==i → i is an ECR root
 *   rank[N]          – DSU rank for union-by-rank
 *   pts_to[N]        – points-to target ECR; N means "no target / not a pointer"
 *   con_type[C]      – uchar: 0/1/2/3 (CON_* above)
 *   con_a[C]         – variable index (lhs or *lhs depending on type)
 *   con_b[C]         – variable index (rhs)
 *   changed          – set to 1 by any work-item that caused a union; host polls
 *
 * ## Algorithm (chaotic iteration)
 *   The host launches three kernel variants each pass:
 *     1. retdec_steensgaard_copy   — processes CON_COPY
 *     2. retdec_steensgaard_addr   — processes CON_ADDR_OF
 *     3. retdec_steensgaard_deref  — processes CON_STORE + CON_LOAD
 *   Each work-item processes one constraint.
 *   After each full pass the host checks `changed`; if 0, analysis has converged.
 *
 * ## Notes on join vs union
 *   For copy constraints, a correct Steensgaard analysis would recursively join
 *   the points-to targets when two ECRs merge.  Since OpenCL kernels cannot
 *   recurse and global memory races on pts_to[] would be hard to manage, we
 *   instead store "merge pending" pairs in a second buffer and resolve them
 *   in the next iteration.  This is equivalent — it just takes one extra pass
 *   per level of indirection, which is bounded by the program's maximum pointer
 *   depth (very low in practice).
 */

#define CON_COPY    0u
#define CON_ADDR_OF 1u
#define CON_STORE   2u
#define CON_LOAD    3u
#define NO_TARGET   0xFFFFFFFFu

/* ─── Iterative path compression ─────────────────────────────────────────── */
static uint ecr_find(__global uint *parent, uint x)
{
    uint root = x;
    for (uint d = 0; d < 64u; ++d) {
        uint p = parent[root];
        if (p == root) break;
        root = p;
    }
    uint cur = x;
    for (uint d = 0; d < 64u; ++d) {
        uint p = parent[cur];
        if (p == cur) break;
        parent[cur] = root;
        cur = p;
    }
    return root;
}

/* ─── Union by rank; returns true if a merge happened ───────────────────── */
static bool ecr_union(__global uint *parent,
                      __global uint *rnk,
                      uint ra, uint rb)
{
    if (ra == rb) return false;
    uint winner, loser;
    if (rnk[ra] > rnk[rb]) { winner = ra; loser = rb; }
    else if (rnk[rb] > rnk[ra]) { winner = rb; loser = ra; }
    else { winner = ra; loser = rb; rnk[ra] += 1u; }
    parent[loser] = winner;
    return true;
}

/* ─── Join two ECRs and enqueue a points-to merge if needed ─────────────── */
/* Returns true if any DSU change occurred. */
static bool ecr_join(__global uint *parent,
                     __global uint *rnk,
                     __global uint *pts_to,
                     __global uint *pending_a,    /* output: pts_to merge queue */
                     __global uint *pending_b,
                     __global uint *pending_count, /* atomic counter */
                     uint a, uint b,
                     uint max_pending)
{
    uint ra = ecr_find(parent, a);
    uint rb = ecr_find(parent, b);
    if (ra == rb) return false;

    uint pa = pts_to[ra];
    uint pb = pts_to[rb];

    bool changed = ecr_union(parent, rnk, ra, rb);

    /* If both ECRs had a points-to target, schedule a recursive join. */
    if (pa != NO_TARGET && pb != NO_TARGET) {
        uint idx = atomic_inc((__global atomic_uint*)pending_count);
        if (idx < max_pending) {
            pending_a[idx] = pa;
            pending_b[idx] = pb;
        }
    } else {
        /* Propagate the non-null pts_to to the winner */
        uint winner = ecr_find(parent, a);  /* winner after union */
        if (pts_to[winner] == NO_TARGET) {
            if (pa != NO_TARGET) pts_to[winner] = pa;
            else if (pb != NO_TARGET) pts_to[winner] = pb;
        }
    }
    return changed;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wave 1: COPY constraints  (a := b  →  join(ECR(a), ECR(b)))
 * ─────────────────────────────────────────────────────────────────────── */
__kernel void retdec_steensgaard_copy(
    __global uint       *parent,
    __global uint       *rnk,
    __global uint       *pts_to,
    __global const uchar *con_type,
    __global const uint  *con_a,
    __global const uint  *con_b,
    uint                  num_constraints,
    __global uint        *pending_a,
    __global uint        *pending_b,
    __global uint        *pending_count,
    uint                  max_pending,
    __global uint        *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_constraints) return;
    if (con_type[gid] != CON_COPY) return;

    bool ch = ecr_join(parent, rnk, pts_to,
                        pending_a, pending_b, pending_count,
                        con_a[gid], con_b[gid], max_pending);
    if (ch) *changed_flag = 1u;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wave 2: ADDR-OF constraints  (a := &b  →  union(pts_to(ECR(a)), ECR(b)))
 * ─────────────────────────────────────────────────────────────────────── */
__kernel void retdec_steensgaard_addr(
    __global uint       *parent,
    __global uint       *rnk,
    __global uint       *pts_to,
    __global const uchar *con_type,
    __global const uint  *con_a,
    __global const uint  *con_b,
    uint                  num_constraints,
    __global uint        *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_constraints) return;
    if (con_type[gid] != CON_ADDR_OF) return;

    uint a   = con_a[gid];
    uint b   = con_b[gid];
    uint ra  = ecr_find(parent, a);
    uint rb  = ecr_find(parent, b);
    uint pra = pts_to[ra];

    if (pra == NO_TARGET) {
        /* First address-of for this ECR: set pts_to to ECR(b). */
        pts_to[ra] = rb;
        *changed_flag = 1u;
    } else {
        /* Already has a pts_to: merge with ECR(b). */
        uint rpra = ecr_find(parent, pra);
        if (ecr_union(parent, rnk, rpra, rb)) {
            *changed_flag = 1u;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wave 3: DEREF constraints
 *   STORE  (*a := b)  →  union(pts_to(ECR(a)), ECR(b))
 *   LOAD   (a := *b)  →  union(ECR(a), pts_to(ECR(b)))
 * ─────────────────────────────────────────────────────────────────────── */
__kernel void retdec_steensgaard_deref(
    __global uint       *parent,
    __global uint       *rnk,
    __global uint       *pts_to,
    __global const uchar *con_type,
    __global const uint  *con_a,
    __global const uint  *con_b,
    uint                  num_constraints,
    __global uint        *pending_a,
    __global uint        *pending_b,
    __global uint        *pending_count,
    uint                  max_pending,
    __global uint        *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_constraints) return;

    uchar ctype = con_type[gid];
    if (ctype != CON_STORE && ctype != CON_LOAD) return;

    uint a  = con_a[gid];
    uint b  = con_b[gid];
    uint ra = ecr_find(parent, a);
    uint rb = ecr_find(parent, b);

    if (ctype == CON_STORE) {
        /* *a := b  —  need pts_to(ECR(a)) */
        uint pra = pts_to[ra];
        if (pra != NO_TARGET) {
            uint rpra = ecr_find(parent, pra);
            bool ch = ecr_join(parent, rnk, pts_to,
                                pending_a, pending_b, pending_count,
                                rpra, rb, max_pending);
            if (ch) *changed_flag = 1u;
        }
    } else {
        /* a := *b  —  need pts_to(ECR(b)) */
        uint prb = pts_to[rb];
        if (prb != NO_TARGET) {
            uint rprb = ecr_find(parent, prb);
            bool ch = ecr_join(parent, rnk, pts_to,
                                pending_a, pending_b, pending_count,
                                ra, rprb, max_pending);
            if (ch) *changed_flag = 1u;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Pending-join flush: processes the pending_a/b pairs queued by ecr_join.
 * Run after the main waves to complete recursive copy-constraint joins.
 * ─────────────────────────────────────────────────────────────────────── */
__kernel void retdec_steensgaard_flush_pending(
    __global uint *parent,
    __global uint *rnk,
    __global uint *pts_to,
    __global uint *pending_a,
    __global uint *pending_b,
    uint           num_pending,
    __global uint *next_pending_a,
    __global uint *next_pending_b,
    __global uint *next_pending_count,
    uint           max_next_pending,
    __global uint *changed_flag)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_pending) return;

    uint a  = pending_a[gid];
    uint b  = pending_b[gid];
    uint ra = ecr_find(parent, a);
    uint rb = ecr_find(parent, b);

    if (ra == rb) return;

    bool ch = ecr_join(parent, rnk, pts_to,
                        next_pending_a, next_pending_b, next_pending_count,
                        ra, rb, max_next_pending);
    if (ch) *changed_flag = 1u;
}
