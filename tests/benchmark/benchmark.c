/**
 * @file benchmark.c
 * @brief Canonical decompiler benchmark — exercises every optimizer in the
 *        retdec llvmir2hll pipeline and provides stable inputs for performance
 *        and quality regression testing.
 *
 * Design principles:
 *  - Every section is an __attribute__((noinline)) function so GCC cannot
 *    inline or constant-fold it away.
 *  - volatile is used where needed to defeat GCC's constant propagation while
 *    still letting retdec see the real pattern.
 *  - Comments identify which optimizer each section targets and what the
 *    expected output pattern is.
 *
 * Compile:
 *   gcc -O1 -o benchmark_O1 benchmark.c
 *   gcc -O1 -s -o benchmark_O1_stripped benchmark.c   # strip at link time
 *   gcc -O2 -o benchmark_O2 benchmark.c
 *
 * Decompile (example):
 *   retdec-decompiler benchmark_O1 -o benchmark_O1.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ============================================================
 * GLOBALS
 * Target: UnusedGlobalVarOptimizer
 * g_sink is referenced so the compiler keeps side-effects.
 * g_write_only is written but never read → removed by optimizer.
 * ============================================================ */

volatile int g_sink = 0;          /* read/written — must survive */
int g_write_only = 0;             /* never read  — optimizer removes */

char g_hello_bytes[] = {72, 101, 108, 108, 111, 0};          /* "Hello" */
char g_empty_buf[8]  = {0, 0, 0, 0, 0, 0, 0, 0};            /* all zeros */
char g_name_table[3][8] = {
    {65, 108, 105, 99, 101, 0, 0, 0},   /* "Alice" */
    {0,  0,   0,   0,  0,  0, 0, 0},    /* ""      */
    {66, 111, 98,  0,  0,  0, 0, 0},    /* "Bob"   */
};

/* ============================================================
 * SECTION 1 — IfToSwitchOptimizer
 * Pattern: long if/else-if chain on one variable → switch
 * ============================================================ */
__attribute__((noinline))
const char *day_name(int d) {
    if      (d == 0) return "Sunday";
    else if (d == 1) return "Monday";
    else if (d == 2) return "Tuesday";
    else if (d == 3) return "Wednesday";
    else if (d == 4) return "Thursday";
    else if (d == 5) return "Friday";
    else if (d == 6) return "Saturday";
    else             return "Unknown";
}

__attribute__((noinline))
int classify_char(int c) {
    if      (c == ' ')  return 0;
    else if (c == '\t') return 0;
    else if (c == '\n') return 0;
    else if (c == '.')  return 1;
    else if (c == ',')  return 1;
    else if (c == '!')  return 1;
    else if (c == '?')  return 1;
    else if (c == ';')  return 1;
    else                return 2;
}

/* ============================================================
 * SECTION 2 — WhileTrueToForLoopOptimizer + VarDefForLoopOptimizer
 * Pattern: while(true) { if(i>=n) break; body; i++; } → for loop
 * Using volatile n to prevent GCC from producing a head-controlled loop.
 * ============================================================ */
__attribute__((noinline))
int sum_range(volatile int n) {
    int i = 0;
    int total = 0;
    while (1) {
        total += i;
        if (i >= n) break;
        i++;
    }
    return total;
}

__attribute__((noinline))
int count_positive(volatile int n, volatile int step) {
    int i = 0;
    int count = 0;
    while (1) {
        count++;
        if (i >= n) break;
        i += step;
    }
    return count;
}

/* ============================================================
 * SECTION 3 — BreakContinueReturnOptimizer
 * Pattern: redundant break/continue at end of loop body is removed.
 * ============================================================ */
__attribute__((noinline))
int find_first_zero(const int *arr, volatile int len) {
    for (int i = 0; i < len; i++) {
        if (arr[i] == 0) return i;
        continue;   /* trailing continue — optimizer removes */
    }
    return -1;
}

/* ============================================================
 * SECTION 4 — CopyPropagationOptimizer
 * Pattern: tmp = expr; use(tmp); → use(expr) when tmp is used once.
 * ============================================================ */
__attribute__((noinline))
int copy_prop_demo(int a, int b, int c) {
    int t1 = a + b;
    int t2 = t1 * c;     /* t1 used only here */
    int t3 = t2 - a;     /* t2 used only here */
    return t3;
}

__attribute__((noinline))
int copy_prop_chain(volatile int x) {
    int p = x * 3;
    int q = p + 7;
    int r = q ^ 0xFF;
    int s = r & 0x0F;
    return s;
}

/* ============================================================
 * SECTION 5 — DeadLocalAssignOptimizer
 * Pattern: a = expr; a = other_expr; → first assignment is dead.
 * ============================================================ */
__attribute__((noinline))
int dead_assign_demo(volatile int x) {
    int a = x * 2;      /* overwritten immediately — dead */
    a = x + 1;          /* this is the live assignment */
    return a;
}

/* ============================================================
 * SECTION 6 — SimplifyArithmExprOptimizer / MBASubOptimizer
 * Pattern: redundant expressions like x+0, x*1, x-x → simplified.
 * And MBA-style obfuscation: (a ^ b) + 2*(a & b) → a + b
 * ============================================================ */
__attribute__((noinline))
int mba_add(volatile int a, volatile int b) {
    /* (a XOR b) + 2*(a AND b) = a + b */
    return (a ^ b) + 2 * (a & b);
}

__attribute__((noinline))
int mba_sub(volatile int a, volatile int b) {
    /* (a XOR b) - 2*(~a & b) = a - b  (equivalent form) */
    return (a | b) - (a & b) + (a ^ b) - (a ^ b)/2 - (a ^ b)/2;
}

__attribute__((noinline))
int arith_identity(volatile int x) {
    int a = x + 0;        /* +0 eliminated */
    int b = a * 1;        /* *1 eliminated */
    int c = b - 0;        /* -0 eliminated */
    int d = c | 0;        /* |0 eliminated */
    return d;
}

/* ============================================================
 * SECTION 7 — BitOpToLogOpOptimizer
 * Pattern: (a & b) != 0 → a && b  (and similar OR patterns)
 * Using noinline to prevent constant folding.
 * ============================================================ */
__attribute__((noinline))
int bit_and_as_logic(volatile int a, volatile int b) {
    return (a & b) != 0;    /* → a && b */
}

__attribute__((noinline))
int bit_or_as_logic(volatile int a, volatile int b) {
    return (a | b) != 0;    /* → a || b */
}

__attribute__((noinline))
int bit_xor_check(volatile int a, volatile int b) {
    return (a ^ b) == 0;    /* → a == b */
}

/* ============================================================
 * SECTION 8 — BitShiftOptimizer
 * Pattern: x * 2^n → x << n,  x / 2^n → x >> n
 * ============================================================ */
__attribute__((noinline))
int shift_mul(volatile int x) {
    return x * 8;       /* → x << 3 */
}

__attribute__((noinline))
int shift_div(volatile int x) {
    return x / 4;       /* → x >> 2 (signed) */
}

/* ============================================================
 * SECTION 9 — CastSimplifierOptimizer / CCastOptimizer
 * Pattern: chained casts, widening casts implicit in C.
 * ============================================================ */
__attribute__((noinline))
int32_t widen_i16(int16_t x) {
    return (int32_t)x * 2;   /* widening is implicit; cast node removed */
}

__attribute__((noinline))
int32_t chained_cast(int32_t x) {
    int16_t narrow = (int16_t)x;
    int32_t wide   = (int32_t)narrow;
    return wide + 1;         /* int32→int16→int32 chain simplified */
}

__attribute__((noinline))
uint8_t sat_add_u8(uint8_t a, uint8_t b) {
    uint16_t sum = (uint16_t)a + (uint16_t)b;
    return (uint8_t)(sum > 255u ? 255u : sum);
}

/* ============================================================
 * SECTION 10 — DerefToArrayIndexOptimizer
 * Pattern: *(arr + i) → arr[i]
 * ============================================================ */
__attribute__((noinline))
int deref_to_index(const int *arr, volatile int i) {
    return *(arr + i);        /* → arr[i] */
}

__attribute__((noinline))
void deref_write(int *arr, volatile int i, volatile int val) {
    *(arr + i) = val;         /* → arr[i] = val */
}

/* ============================================================
 * SECTION 11 — DerefAddressOptimizer
 * Pattern: *(&var) → var
 * ============================================================ */
__attribute__((noinline))
int deref_addr_demo(volatile int x) {
    int local = x;
    return *(&local);         /* → local */
}

/* ============================================================
 * SECTION 12 — IfStructureOptimizer
 * Patterns 1–5: if/else restructuring for readability.
 * ============================================================ */
__attribute__((noinline))
int classify_range(volatile int x) {
    if (x < 0) {
        return -1;
    } else {
        if (x == 0) {
            return 0;
        } else {
            return 1;
        }
    }
}

__attribute__((noinline))
int nested_if_flatten(volatile int a, volatile int b, volatile int c) {
    if (a > 0) {
        if (b > 0) {
            if (c > 0) {
                return 3;
            }
            return 2;
        }
        return 1;
    }
    return 0;
}

/* ============================================================
 * SECTION 13 — GotoCFGOptimizer (Pattern A, B, D)
 * Pattern A: if(cond) goto L; stmts; L: → if(!cond) { stmts }
 * Pattern D: goto loop_exit → break
 * These patterns emerge from GCC's IR for certain control flows.
 * ============================================================ */
__attribute__((noinline))
int find_nonzero(const int *arr, volatile int n) {
    int i = 0;
    while (i < n) {
        if (arr[i] != 0)
            return arr[i];
        i++;
    }
    return 0;
}

/* ============================================================
 * SECTION 14 — VarDefStmtOptimizer
 * Pattern: variables declared at function top with no init are moved
 *          to the point of first use.
 * ============================================================ */
__attribute__((noinline))
int vardef_placement(volatile int x, volatile int y) {
    int result;         /* declared early, used late → moved to first use */
    int temp;
    if (x > 0) {
        temp = x * 2;
        result = temp + y;
    } else {
        temp = y * 3;
        result = temp - x;
    }
    return result;
}

/* ============================================================
 * SECTION 15 — VarDefForLoopOptimizer
 * Pattern: int i; for(i=0;...) → for(int i=0;...)
 * ============================================================ */
__attribute__((noinline))
int sum_array(const int *arr, volatile int n) {
    int i;              /* VarDefForLoopOptimizer merges this into the for() */
    int total = 0;
    for (i = 0; i < n; i++) {
        total += arr[i];
    }
    return total;
}

/* ============================================================
 * SECTION 16 — NoInitVarDefOptimizer
 * Pattern: int x; that has no initializer and is used before being assigned
 *          only in dead branches → removed.
 * ============================================================ */
__attribute__((noinline))
int no_init_cleanup(volatile int a, volatile int b) {
    int unused_local;   /* declared but only used in a dead path → removed */
    int result = a + b;
    return result;      /* unused_local is never actually used */
}

/* ============================================================
 * SECTION 17 — UnknownTypeInferrer
 * Pattern: retdec lifts struct field accesses as pointer arithmetic;
 *          the inferrer assigns concrete struct/pointer types.
 * ============================================================ */
typedef struct Node {
    int value;
    struct Node *next;
} Node;

__attribute__((noinline))
int list_sum(Node *head) {
    int sum = 0;
    Node *cur = head;
    while (cur != NULL) {
        sum += cur->value;
        cur = cur->next;
    }
    return sum;
}

__attribute__((noinline))
int list_count(Node *head) {
    int count = 0;
    Node *cur = head;
    while (cur) {
        count++;
        cur = cur->next;
    }
    return count;
}

/* ============================================================
 * SECTION 18 — SelfAssignOptimizer
 * Pattern: x = x; → removed
 * ============================================================ */
__attribute__((noinline))
int self_assign_demo(volatile int x) {
    int a = x;
    a = a;          /* self-assignment — removed */
    return a + 1;
}

/* ============================================================
 * SECTION 19 — EmptyStmtOptimizer
 * Pattern: empty statements (labels with no body, redundant semicolons) removed.
 * ============================================================ */
__attribute__((noinline))
int empty_stmts(volatile int x) {
    int a = x;
    ;               /* empty statement — removed */
    ;
    return a * 2;
}

/* ============================================================
 * SECTION 20 — Recursion (call-graph + type analysis)
 * Tests that recursive call patterns are correctly reconstructed.
 * ============================================================ */
__attribute__((noinline))
int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

__attribute__((noinline))
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

__attribute__((noinline))
int gcd(int a, int b) {
    if (b == 0) return a;
    return gcd(b, a % b);
}

/* ============================================================
 * SECTION 21 — Complex control flow (nested loops, break, continue)
 * Tests CFG reconstruction for multi-level breaks.
 * ============================================================ */
__attribute__((noinline))
int matrix_search(const int *mat, volatile int rows, volatile int cols,
                  volatile int target) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (mat[r * cols + c] == target)
                return r * cols + c;
        }
    }
    return -1;
}

__attribute__((noinline))
int count_even_rows(const int *mat, volatile int rows, volatile int cols) {
    int count = 0;
    for (int r = 0; r < rows; r++) {
        int all_even = 1;
        for (int c = 0; c < cols; c++) {
            if (mat[r * cols + c] % 2 != 0) {
                all_even = 0;
                break;
            }
        }
        if (all_even) count++;
    }
    return count;
}

/* ============================================================
 * SECTION 22 — Sorting (classic algorithm, tests loop + conditional quality)
 * ============================================================ */
__attribute__((noinline))
void bubble_sort(int *arr, volatile int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (arr[j] > arr[j + 1]) {
                int tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
}

__attribute__((noinline))
int binary_search(const int *arr, volatile int n, volatile int target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) return mid;
        if (arr[mid] < target) lo = mid + 1;
        else                   hi = mid - 1;
    }
    return -1;
}

/* ============================================================
 * SECTION 23 — Memory / pointer patterns
 * Tests pointer arithmetic recovery and dynamic allocation patterns.
 * ============================================================ */
__attribute__((noinline))
int *alloc_and_fill(volatile int n) {
    int *p = malloc((size_t)n * sizeof(int));
    if (!p) return NULL;
    for (int i = 0; i < n; i++)
        p[i] = i * i;
    return p;
}

__attribute__((noinline))
int sum_ptr_walk(const int *begin, const int *end) {
    int sum = 0;
    for (const int *p = begin; p < end; p++)
        sum += *p;
    return sum;
}

/* ============================================================
 * SECTION 24 — String operations (tests char array / string literal recovery)
 * ============================================================ */
__attribute__((noinline))
int my_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

__attribute__((noinline))
int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

__attribute__((noinline))
void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

/* ============================================================
 * SECTION 25 — Bitfield / bitmask patterns
 * Tests that bitfield operations are cleanly reconstructed.
 * ============================================================ */
__attribute__((noinline))
uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) |
           ((uint32_t)b << 8)  |  (uint32_t)a;
}

__attribute__((noinline))
uint8_t extract_channel(uint32_t rgba, volatile int channel) {
    return (uint8_t)((rgba >> (channel * 8)) & 0xFF);
}

__attribute__((noinline))
uint32_t toggle_bits(volatile uint32_t mask, volatile uint32_t flags) {
    return mask ^ flags;
}

/* ============================================================
 * SECTION 26 — Integer arithmetic diversity
 * Tests proper type reconstruction for various integer widths.
 * ============================================================ */
__attribute__((noinline))
int64_t wide_mul(volatile int32_t a, volatile int32_t b) {
    return (int64_t)a * (int64_t)b;
}

__attribute__((noinline))
uint16_t clamp_u16(volatile int32_t x) {
    if (x < 0)      return 0;
    if (x > 65535)  return 65535;
    return (uint16_t)x;
}

__attribute__((noinline))
int32_t abs_val(volatile int32_t x) {
    return x < 0 ? -x : x;
}

__attribute__((noinline))
int32_t sign_of(volatile int32_t x) {
    return (x > 0) - (x < 0);    /* branchless sign */
}

/* ============================================================
 * SECTION 27 — Function pointers (tests indirect call reconstruction)
 * ============================================================ */
typedef int (*BinOp)(int, int);

__attribute__((noinline))
static int add(int a, int b) { return a + b; }
__attribute__((noinline))
static int sub(int a, int b) { return a - b; }
__attribute__((noinline))
static int mul(int a, int b) { return a * b; }

__attribute__((noinline))
int apply_op(BinOp op, volatile int x, volatile int y) {
    return op(x, y);
}

__attribute__((noinline))
int dispatch(volatile int choice, volatile int x, volatile int y) {
    BinOp ops[] = { add, sub, mul };
    if (choice < 0 || choice > 2) return 0;
    return ops[choice](x, y);
}

/* ============================================================
 * SECTION 28 — Error handling / early return patterns
 * Tests IfStructureOptimizer patterns 1 & 3 (early-return flattening).
 * ============================================================ */
__attribute__((noinline))
int safe_divide(volatile int num, volatile int den) {
    if (den == 0) return -1;
    if (num < 0 && den < 0) return (-num) / (-den);
    if (num < 0) return -((-num) / den);
    return num / den;
}

__attribute__((noinline))
int validate_and_process(volatile int x, volatile int lo,
                          volatile int hi) {
    if (x < lo) return -1;
    if (x > hi) return -1;
    if ((x % 2) == 0) return x / 2;
    return x * 3 + 1;   /* Collatz step */
}

/* ============================================================
 * main — calls every section so the linker keeps all symbols.
 * Uses g_sink to absorb all return values (prevents dead-code elim).
 * ============================================================ */
int main(void) {
    /* Section 1: switch recovery */
    g_sink += (int)(intptr_t)day_name(3);
    g_sink += classify_char('.');

    /* Section 2: for-loop recovery */
    g_sink += sum_range(10);
    g_sink += count_positive(20, 3);

    /* Section 3: break/continue */
    int arr5[5] = {1, 2, 0, 4, 5};
    g_sink += find_first_zero(arr5, 5);

    /* Section 4: copy propagation */
    g_sink += copy_prop_demo(3, 4, 5);
    g_sink += copy_prop_chain(7);

    /* Section 5: dead assignment */
    g_sink += dead_assign_demo(9);

    /* Section 6: MBA / arithmetic */
    g_sink += mba_add(5, 3);
    g_sink += mba_sub(10, 3);
    g_sink += arith_identity(42);

    /* Section 7: bit→logic */
    g_sink += bit_and_as_logic(3, 5);
    g_sink += bit_or_as_logic(0, 1);
    g_sink += bit_xor_check(7, 7);

    /* Section 8: shift */
    g_sink += shift_mul(4);
    g_sink += shift_div(16);

    /* Section 9: casts */
    g_sink += widen_i16(100);
    g_sink += chained_cast(300);
    g_sink += sat_add_u8(200, 100);

    /* Section 10: deref→index */
    int arr10[4] = {10, 20, 30, 40};
    g_sink += deref_to_index(arr10, 2);
    deref_write(arr10, 3, 99);

    /* Section 11: *(&var) */
    g_sink += deref_addr_demo(55);

    /* Section 12: if-restructure */
    g_sink += classify_range(-5);
    g_sink += nested_if_flatten(1, 1, 1);

    /* Section 13: goto→break */
    int arr13[6] = {0, 0, 7, 0, 0, 0};
    g_sink += find_nonzero(arr13, 6);

    /* Section 14: vardef placement */
    g_sink += vardef_placement(3, 4);

    /* Section 15: vardef-for-loop merge */
    int arr15[4] = {1, 2, 3, 4};
    g_sink += sum_array(arr15, 4);

    /* Section 16: no-init cleanup */
    g_sink += no_init_cleanup(5, 6);

    /* Section 17: type inference (linked list) */
    Node n1 = {10, NULL};
    Node n2 = {20, &n1};
    Node n3 = {30, &n2};
    g_sink += list_sum(&n3);
    g_sink += list_count(&n3);

    /* Section 18: self-assign */
    g_sink += self_assign_demo(7);

    /* Section 19: empty stmts */
    g_sink += empty_stmts(3);

    /* Section 20: recursion */
    g_sink += fibonacci(10);
    g_sink += factorial(5);
    g_sink += gcd(48, 18);

    /* Section 21: complex control */
    int mat[2][3] = {{1, 2, 3}, {4, 5, 6}};
    g_sink += matrix_search(&mat[0][0], 2, 3, 5);
    g_sink += count_even_rows(&mat[0][0], 2, 3);

    /* Section 22: sorting */
    int sarr[5] = {5, 3, 1, 4, 2};
    bubble_sort(sarr, 5);
    g_sink += binary_search(sarr, 5, 4);

    /* Section 23: memory / pointers */
    int *dyn = alloc_and_fill(8);
    if (dyn) {
        g_sink += sum_ptr_walk(dyn, dyn + 8);
        free(dyn);
    }

    /* Section 24: strings */
    char buf[32];
    my_strcpy(buf, "benchmark");
    g_sink += my_strlen(buf);
    g_sink += my_strcmp(buf, "benchmark");
    g_sink += my_strlen(g_hello_bytes);

    /* Section 25: bitfields */
    g_sink += (int)pack_rgba(255, 128, 64, 32);
    g_sink += extract_channel(0xDEADBEEF, 2);
    g_sink += (int)toggle_bits(0xAA, 0x55);

    /* Section 26: integer widths */
    g_sink += (int)wide_mul(100000, 200000);
    g_sink += clamp_u16(-5);
    g_sink += abs_val(-42);
    g_sink += sign_of(-7);

    /* Section 27: function pointers */
    g_sink += apply_op(add, 10, 20);
    g_sink += dispatch(1, 50, 30);

    /* Section 28: early returns */
    g_sink += safe_divide(10, 3);
    g_sink += validate_and_process(7, 1, 100);

    /* Use globals so linker keeps them */
    g_sink += g_name_table[0][0];
    g_write_only = g_sink;

    return 0;
}
