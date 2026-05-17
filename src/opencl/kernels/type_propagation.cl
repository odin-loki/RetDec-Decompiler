/**
 * @file src/opencl/kernels/type_propagation.cl
 * @brief Parallel per-function type constraint propagation via union-find (DSU).
 *
 * ## Data layout (Structure-of-Arrays for cache efficiency)
 *
 *   func_offsets[F+1]   – slot range for function i = [func_offsets[i], func_offsets[i+1])
 *   parent[S]           – DSU parent; parent[i]==i means i is a root
 *   rank[S]             – DSU rank (for union-by-rank)
 *   width[S]            – type width: 0=unknown, 1=i8, 2=i16, 4=i32, 8=i64, 16=i128
 *   signedness[S]       – 0=unknown, 1=signed, 2=unsigned
 *   is_pointer[S]       – 0/1
 *
 *   con_offsets[F+1]    – constraint range for function i
 *   con_a[C], con_b[C]  – global slot indices of unified pairs
 *
 *   dirty_flags[F]      – 1 if work-item i made any change this iteration
 *   global_done         – host resets to 1; work-item clears to 0 on any change
 *
 * ## Algorithm per work-item (gid = function index)
 *   1. For each constraint (a, b) in [con_offsets[gid], con_offsets[gid+1]):
 *      ra = find(a), rb = find(b)
 *      if ra != rb:
 *        union(ra, rb) — merge width/signedness/is_pointer
 *        set dirty_flags[gid] = 1
 *        set *global_done = 0
 *
 * ## Notes
 *   - Iterative path compression: we loop up the parent chain up to 64 steps
 *     (x86-64 worst case stack depth is finite in practice).
 *   - The host re-launches the kernel until global_done stays 1.
 *   - Width merge rule: take the wider of two known widths; 0 is "unknown".
 *   - Signedness merge rule: if both known and different → signed wins.
 */

/* ─── Width merge: take max known, 0 = unknown ───────────────────────────── */
static uchar merge_width(uchar a, uchar b)
{
    if (a == 0) return b;
    if (b == 0) return a;
    return (a > b) ? a : b;
}

/* ─── Signedness merge: both must agree; mismatch → signed wins ──────────── */
static uchar merge_signedness(uchar a, uchar b)
{
    if (a == 0) return b;
    if (b == 0) return a;
    if (a == b) return a;
    return 1u;   /* signed wins on conflict */
}

/* ─── Iterative find with path compression ───────────────────────────────── */
static uint dsu_find(__global uint *parent, uint x)
{
    /* Walk to root */
    uint root = x;
    for (uint depth = 0; depth < 64u; ++depth) {
        uint p = parent[root];
        if (p == root) break;
        root = p;
    }
    /* Path compression: point visited nodes directly to root */
    uint cur = x;
    for (uint depth = 0; depth < 64u; ++depth) {
        uint p = parent[cur];
        if (p == cur) break;
        parent[cur] = root;
        cur = p;
    }
    return root;
}

/* ─── Union by rank with type attribute merge ────────────────────────────── */
static bool dsu_union(__global uint  *parent,
                      __global uint  *rnk,
                      __global uchar *width,
                      __global uchar *signedness,
                      __global uchar *is_pointer,
                      uint ra, uint rb)
{
    if (ra == rb) return false;

    /* Merge type attributes into the winner (higher rank). */
    uchar mw  = merge_width(width[ra], width[rb]);
    uchar ms  = merge_signedness(signedness[ra], signedness[rb]);
    uchar mp  = is_pointer[ra] | is_pointer[rb];

    uint winner, loser;
    if (rnk[ra] > rnk[rb]) {
        winner = ra; loser = rb;
    } else if (rnk[rb] > rnk[ra]) {
        winner = rb; loser = ra;
    } else {
        winner = ra; loser = rb;
        rnk[ra] += 1u;
    }

    parent[loser]      = winner;
    width[winner]      = mw;
    signedness[winner] = ms;
    is_pointer[winner] = mp;

    return true;
}

/* ─── Main kernel ─────────────────────────────────────────────────────────── */
__kernel void retdec_type_propagation(
    /* DSU arrays (S total slots across all functions) */
    __global uint  *parent,
    __global uint  *rnk,
    __global uchar *width,
    __global uchar *signedness,
    __global uchar *is_pointer,

    /* Constraints */
    __global const uint *con_offsets,   /* length F+1 */
    __global const uint *con_a,         /* length C */
    __global const uint *con_b,         /* length C */

    /* Per-function output */
    __global uint  *dirty_flags,        /* length F */

    /* Global convergence flag */
    __global uint  *global_done,

    /* Number of functions */
    uint num_funcs)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_funcs) return;

    uint c_start = con_offsets[gid];
    uint c_end   = con_offsets[gid + 1u];

    uint local_dirty = 0u;

    for (uint ci = c_start; ci < c_end; ++ci) {
        uint a  = con_a[ci];
        uint b  = con_b[ci];
        uint ra = dsu_find(parent, a);
        uint rb = dsu_find(parent, b);
        if (dsu_union(parent, rnk, width, signedness, is_pointer, ra, rb)) {
            local_dirty = 1u;
        }
    }

    if (local_dirty) {
        dirty_flags[gid] = 1u;
        /* Signal that another iteration is needed. */
        *global_done = 0u;
    }
}

/* ─── Width seeding kernel ───────────────────────────────────────────────── */
/*
 * Initialises the width/signedness/is_pointer arrays from a flat list of
 * instruction operand annotations.  Run once before the propagation loop.
 *
 *   operand_slot[I]   – which type slot this operand annotation belongs to
 *   operand_width[I]  – byte: 1/2/4/8/16 or 0 for unknown
 *   operand_sign[I]   – byte: 0=unknown, 1=signed, 2=unsigned
 *   operand_ptr[I]    – byte: 0/1
 *   num_operands      – total annotations
 */
__kernel void retdec_type_seed(
    __global       uchar *width,
    __global       uchar *signedness,
    __global       uchar *is_pointer,
    __global const uint  *operand_slot,
    __global const uchar *operand_width,
    __global const uchar *operand_sign,
    __global const uchar *operand_ptr,
    uint num_operands)
{
    uint gid = (uint)get_global_id(0);
    if (gid >= num_operands) return;

    uint slot = operand_slot[gid];
    width[slot]      = merge_width(width[slot], operand_width[gid]);
    signedness[slot] = merge_signedness(signedness[slot], operand_sign[gid]);
    is_pointer[slot] |= operand_ptr[gid];
}
