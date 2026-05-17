/**
 * @file full_coverage.c
 * @brief Comprehensive test exercising every optimizer in the retdec pipeline.
 *
 * Compile: gcc -O1 -o full_coverage full_coverage.c
 * Decompile: retdec-decompiler full_coverage -s -o full_coverage_out.c
 *
 * Each section documents which optimizer(s) it targets and what pattern
 * the compiled binary will present to the decompiler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * SECTION A: Global variables
 * Targets: UnusedGlobalVarOptimizer
 * - g_counter is read/written → kept
 * - g_unused is written at definition but never read → optimizer removes it
 * ========================================================================= */

int g_counter = 0;

/* Non-static so gcc keeps it in the binary, but retdec finds no reads
   of it in the lifted IR → UnusedGlobalVarOptimizer removes it */
int g_never_read = 42;

/* Global char arrays recovered as ConstArray by retdec from the ELF data
   section.  These trigger CharArrayToStringOptimizer (printable bytes → string
   literal) and EmptyArrayToStringOptimizer (all-zero arrays → "") */
char g_hello_bytes[] = {72, 105, 33, 10, 0};  /* "Hi!\n" - printable ASCII */
char g_empty_buf[8]  = {0, 0, 0, 0, 0, 0, 0, 0};  /* all zeros */

/* 2D char array: mix of printable and empty entries.
   After CharArrayToStringOptimizer converts printable sub-arrays to ConstString,
   EmptyArrayToStringOptimizer converts the all-zero sub-arrays to "". */
char g_name_table[4][8] = {
    {65, 108, 105, 99, 101, 0, 0, 0},  /* "Alice" */
    {0,  0,   0,   0,  0,  0, 0, 0},  /* empty → "" */
    {66, 111, 98,  0,  0,  0, 0, 0},  /* "Bob" */
    {0,  0,   0,   0,  0,  0, 0, 0}   /* empty → "" */
};

/* =========================================================================
 * SECTION B: Cast patterns
 * Targets: CCastOptimizer, CastSimplifierOptimizer, RemoveUselessCastsOptimizer
 *
 * The LLVM IR produced by retdec's lifter uses explicit cast nodes everywhere.
 * CCastOptimizer removes casts that are implicit in C (e.g. int16→int32).
 * CastSimplifierOptimizer collapses chained casts like (int32)(int16)x → (int32)x.
 * ========================================================================= */

/* Widening int cast — implicit in C, CCastOptimizer strips the explicit node */
int32_t widen_i16(int16_t x) {
    int32_t a = (int32_t)x;
    return a * 2;
}

/* Chained casts — CastSimplifierOptimizer collapses the chain */
int32_t chained_cast(int32_t x) {
    int16_t narrow = (int16_t)x;
    int32_t wide   = (int32_t)narrow;   /* chain: int32→int16→int32 */
    return wide + 1;
}

/* Saturating add: uint8 result — exercises narrowing TruncCast */
uint8_t saturate_add_u8(uint8_t a, uint8_t b) {
    uint16_t sum = (uint16_t)a + (uint16_t)b;
    return (uint8_t)(sum > 255u ? 255u : sum);
}

/* =========================================================================
 * SECTION C: if-to-switch conversion
 * Targets: IfToSwitchOptimizer
 *
 * A long if/else-if chain comparing the same variable to integer constants
 * meets the requirements for conversion to a switch statement.
 * ========================================================================= */

const char *weekday_name(int d) {
    if (d == 0) return "Sunday";
    else if (d == 1) return "Monday";
    else if (d == 2) return "Tuesday";
    else if (d == 3) return "Wednesday";
    else if (d == 4) return "Thursday";
    else if (d == 5) return "Friday";
    else if (d == 6) return "Saturday";
    else return "Unknown";
}

const char *status_name(int code) {
    if (code == 0) return "OK";
    else if (code == 1) return "INFO";
    else if (code == 2) return "WARNING";
    else if (code == 3) return "ERROR";
    else if (code == 4) return "FATAL";
    else return "UNKNOWN";
}

/* =========================================================================
 * SECTION D: If-structure optimizations
 * Targets: IfStructureOptimizer (patterns 1-5), IfStructureOptimizerExt (6-7),
 *          GotoCFGOptimizer, GotoStmtOptimizer
 *
 * Complex cascading conditionals generate goto-laden IR after lifting;
 * GotoCFG/GotoStmt clean up the goto web first, then IfStructure reorganises
 * the resulting if chains into readable nested/flat form.
 * ========================================================================= */

/* Cascading range checks — IfStructureOptimizer flattens/reorganises */
int classify_score(int score) {
    if (score < 0)   return -1;
    if (score < 60)  return 0;
    if (score < 70)  return 1;
    if (score < 80)  return 2;
    if (score < 90)  return 3;
    return 4;
}

/* Nested conditions with early exit */
int check_bounds(int x, int lo, int hi) {
    if (x < lo) return -1;
    if (x > hi) return 1;
    return 0;
}

/* Complex boolean with multiple returns — exercises IfStructure patterns 1-3 */
const char *classify_char(char c) {
    if (c >= 'A' && c <= 'Z') return "upper";
    else if (c >= 'a' && c <= 'z') return "lower";
    else if (c >= '0' && c <= '9') return "digit";
    else if (c == ' ' || c == '\t') return "space";
    else return "other";
}

/* =========================================================================
 * SECTION E: Loop transformations
 * Targets: WhileTrueToForLoopOptimizer, WhileTrueToWhileCondOptimizer,
 *          LoopLastContinueOptimizer, IfBeforeLoopOptimizer,
 *          PreWhileTrueLoopConvOptimizer
 *
 * The compiler emits while-true loops at the IR level for many constructs.
 * These optimizers recognise the patterns and emit cleaner for/while loops.
 * ========================================================================= */

/* Simple accumulator — in IR becomes while(true)+counter → for loop */
int sum_n(int n) {
    int s = 0, i = 0;
    while (i < n) {
        s += i;
        i++;
    }
    return s;
}

/* do-while loop — different lifting path from while */
int collatz_steps(int n) {
    int steps = 0;
    do {
        if (n % 2 == 0) n /= 2;
        else n = 3 * n + 1;
        steps++;
    } while (n != 1);
    return steps;
}

/* Infinite loop with guard break → WhileTrueToWhileCondOptimizer */
int find_first_nonzero(const int *arr, int n) {
    int i = 0;
    for (;;) {
        if (i >= n) return -1;
        if (arr[i] != 0) return i;
        i++;
    }
}

/* Loop with continue at the end of body → LoopLastContinueOptimizer removes
   the trailing `continue` because it is redundant as the last statement */
int sum_odds(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) continue;   /* skip evens */
        s += i;
        /* redundant continue here is removed by LoopLastContinueOptimizer */
    }
    return s;
}

/* Nested loops with multi-level early exit */
int find_in_matrix(int m[4][4], int target) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (m[i][j] == target)
                return i * 4 + j;
        }
    }
    return -1;
}

/* WhileTrueToForLoopOptimizer: counter-controlled while(1) with increment and
   a break-at-end pattern */
int next_multiple(int start, int divisor) {
    int n = start;
    while (1) {
        if (n % divisor == 0) return n;
        n++;
    }
}

/* Classic WhileTrueToForLoopOptimizer target:
   while(true) { guard-break; body; counter++; }
   → for(int i=0; i<n; i++) { body; }
   Uses volatile to prevent gcc from generating a different loop structure. */
int count_upward(volatile int n) {
    int i = 0;
    while (1) {
        if (i >= n) break;
        g_counter += i;   /* body: side-effect so gcc keeps the loop */
        i++;
    }
    return i;
}

/* =========================================================================
 * SECTION F: Arithmetic expression simplification
 * Targets: SimplifyArithmExprOptimizer and all sub-optimizers:
 *          ZeroSubOptimizer, OneSubOptimizer, EqualOperandsSubOptimizer,
 *          NegationOperatorSubOptimizer, ConstOperatorConstSubOptimizer,
 *          ThreeOperandsSubOptimizer, ChangeOrderSubOptimizer,
 *          MBASubOptimizer, BitfieldSubOptimizer, Pow2SubOptimizer,
 *          TernaryOperatorSubOptimizer, BoolComparisonSubOptimizer
 * ========================================================================= */

/* MBA identity: (x^y) + 2*(x&y) == x+y — MBASubOptimizer simplifies */
uint32_t mba_add(uint32_t x, uint32_t y) {
    return (x ^ y) + 2u * (x & y);
}

/* MBA subtraction: x + (~y) + 1 == x - y */
uint32_t mba_sub(uint32_t x, uint32_t y) {
    return x + (~y) + 1u;
}

/* Power-of-two arithmetic — Pow2SubOptimizer converts mul/div to shifts */
int32_t pow2_ops(int32_t x) {
    int32_t a = x * 4;    /* → x << 2 */
    int32_t b = x / 8;    /* → x >> 3 */
    int32_t c = x % 16;   /* → x & 15 */
    return a + b + c;
}

/* Bit field extraction — BitfieldSubOptimizer */
uint32_t extract_bits(uint32_t word) {
    uint32_t lo  = word & 0x0Fu;           /* bits 3:0 */
    uint32_t mid = (word >> 4) & 0x0Fu;   /* bits 7:4 */
    uint32_t hi  = (word >> 8) & 0xFFu;   /* bits 15:8 */
    return lo | (mid << 8) | (hi << 16);
}

/* Equal operands: a - a == 0, a / a == 1, a & a == a */
uint32_t equal_operand_patterns(uint32_t a) {
    uint32_t zero = a - a;     /* → 0 */
    uint32_t same = a & a;     /* → a */
    return same + zero;
}

/* Branchless absolute value (MBA-like: (x + mask) ^ mask) */
int32_t branchless_abs(int32_t x) {
    int32_t mask = x >> 31;
    return (x + mask) ^ mask;
}

/* Nested ternaries → TernaryOperatorSubOptimizer */
int grade(int score) {
    return score >= 90 ? 4 :
           score >= 80 ? 3 :
           score >= 70 ? 2 :
           score >= 60 ? 1 : 0;
}

/* =========================================================================
 * SECTION G: Bit operations → logical operations, shift optimizations
 * Targets: BitOpToLogOpOptimizer, BitShiftOptimizer
 *
 * Single-bit & and | in boolean context → && and ||.
 * Arithmetic shifts simplified to multiplication/division where clearer.
 * ========================================================================= */

/* Bit-and / bit-or of boolean values IN an if-condition.
   BitOpToLogOpOptimizer fires specifically when BitAndOpExpr / BitOrOpExpr
   appears as the condition of an if/while statement (not in a return value).
   __attribute__((noinline)) prevents gcc from constant-folding call sites. */
__attribute__((noinline))
int bit_and_condition(int a, int b) {
    /* (a!=0) & (b!=0) in an if → BitOpToLogOpOptimizer converts to && */
    if ((a != 0) & (b != 0)) {
        return 1;
    }
    return 0;
}

__attribute__((noinline))
int bit_or_condition(int a, int b) {
    /* (a!=0) | (b!=0) in an if → BitOpToLogOpOptimizer converts to || */
    if ((a != 0) | (b != 0)) {
        return 1;
    }
    return 0;
}

/* Bit-and/or outside a condition (return value) — these do NOT trigger the
   optimizer (it only runs on conditions); kept for completeness */
int both_conditions(int a, int b) {
    return (a > 0) & (b > 0);
}

int either_condition(int a, int b) {
    return (a > 0) | (b > 0);
}

/* Shift patterns */
int shift_multiply(int x) {
    return (x << 3) - (x << 1);   /* 8x - 2x = 6x */
}

/* =========================================================================
 * SECTION H: Copy propagation and dead assignments
 * Targets: CopyPropagationOptimizer, SimpleCopyPropagationOptimizer,
 *          DeadLocalAssignOptimizer, DeadLocalAssignCallOptimizer,
 *          SelfAssignOptimizer
 * ========================================================================= */

static int helper(int x) { return x * 3 + 7; }

/* Simple copy: tmp = a+b; result = tmp*2 → result = (a+b)*2 */
int simple_copy_prop(int a, int b) {
    int tmp = a + b;
    int result = tmp * 2;
    return result;
}

/* Dead local assign: `unused` is written twice but never read */
int dead_assign(int x) {
    int unused = x + 99;     /* written, never read → DeadLocalAssignOptimizer */
    int val = x * 2;
    unused = val + 1;         /* overwritten without read → still dead */
    return val;
}

/* Dead call result: return value of helper() discarded */
int dead_call_result(int x) {
    int ignored = helper(x);  /* call result never used → DeadLocalAssignCall */
    (void)ignored;
    return x + 1;
}

/* Extended copy propagation: call result used across intervening statements */
int extended_copy(int x) {
    int r = helper(x);       /* r = helper(x) */
    int a = x + 5;           /* intervening: doesn't use or clobber r */
    int b = a * 2;           /* intervening: doesn't use or clobber r */
    return r + b;            /* r finally used → propagate through */
}

/* =========================================================================
 * SECTION I: Array and string optimizations
 * Targets: CharArrayToStringOptimizer, EmptyArrayToStringOptimizer,
 *          DerefToArrayIndexOptimizer, DerefAddressOptimizer,
 *          CArrayArgOptimizer
 * ========================================================================= */

/* Char array built element-by-element → CharArrayToStringOptimizer
   converts the individual byte assignments into a string literal */
void print_hello(void) {
    char msg[6];
    msg[0] = 'H'; msg[1] = 'i'; msg[2] = '!';
    msg[3] = '\n'; msg[4] = '\0';
    fputs(msg, stdout);
}

/* Zero-initialized char array → EmptyArrayToStringOptimizer → "" literal */
void use_empty_array(void) {
    char buf[16] = {0};
    buf[0] = 'X';
    printf("%s\n", buf);
}

/* Pointer arithmetic *(ptr+i) → ptr[i] via DerefToArrayIndexOptimizer */
int sum_via_ptr_arith(const int *ptr, int n) {
    int s = 0;
    for (int i = 0; i < n; i++)
        s += *(ptr + i);   /* → ptr[i] */
    return s;
}

/* Array argument to function → CArrayArgOptimizer adjusts the call site */
static void fill(int arr[], int n, int v) {
    for (int i = 0; i < n; i++) arr[i] = v;
}

/* =========================================================================
 * SECTION J: LLVM intrinsics
 * Targets: LLVMIntrinsicsOptimizer
 *
 * memcpy/memmove/memset are lowered to LLVM intrinsics by the front-end;
 * LLVMIntrinsicsOptimizer replaces them with readable library-call equivalents.
 * ========================================================================= */

/* Use volatile n to prevent constant-folding of the sizes, which would
   cause gcc to expand these into SSE vector instructions that retdec
   cannot handle (the LLVMConstantConverter rejects <4 x float> consts) */
void buffer_ops(char *dst, const char *src, volatile int n) {
    memcpy(dst, src, (size_t)n);        /* llvm.memcpy → memcpy() call */
    memmove(dst + 1, dst, (size_t)n);   /* llvm.memmove → memmove() call */
    memset(dst, 0, (size_t)n);          /* llvm.memset → memset() call */
}

/* =========================================================================
 * SECTION K: Void return and dead-code elimination
 * Targets: VoidReturnOptimizer, BreakContinueReturnOptimizer, DeadCodeOptimizer
 *
 * VoidReturnOptimizer removes a trailing `return;` at end of a void function.
 * DeadCodeOptimizer removes statements after unconditional return/break.
 * BreakContinueReturnOptimizer removes dead code after break/continue.
 * ========================================================================= */

/* Trailing void return — VoidReturnOptimizer removes the `return;` */
void bump_counter(int delta) {
    g_counter += delta;
    return;   /* redundant — removed */
}

/* Dead code after return */
int always_positive(int x) {
    if (x < 0) x = -x;
    return x;
    /* unreachable below — DeadCodeOptimizer removes */
    x = x * 2;
    return x;
}

/* Dead code after break in loop */
int find_positive(const int *arr, int n) {
    for (int i = 0; i < n; i++) {
        if (arr[i] > 0) {
            return i;
            /* dead: */ i++;  /* BreakContinueReturnOptimizer removes */
        }
    }
    return -1;
}

/* =========================================================================
 * SECTION L: Variable definition placement
 * Targets: VarDefStmtOptimizer, VarDefForLoopOptimizer, NoInitVarDefOptimizer
 *
 * VarDefForLoopOptimizer: int i; for(i=0;...) → for(int i=0;...)
 * VarDefStmtOptimizer: int x; x=expr; → int x=expr;
 * NoInitVarDefOptimizer: remove `int x;` with no initializer that becomes dead
 * ========================================================================= */

/* VarDefForLoopOptimizer: loop variable declared separately from for() */
int sum_range(int lo, int hi) {
    int i;
    int total = 0;
    for (i = lo; i < hi; i++)
        total += i;
    return total;
}

/* VarDefStmtOptimizer: declaration separated from first use by several stmts */
int separate_def_use(int a, int b) {
    int result;            /* naked declaration — no initializer */
    int t1 = a + b;
    int t2 = t1 * 2;
    result = t2 - a;       /* first (and only) use */
    return result;
}

/* =========================================================================
 * SECTION M: Struct and pointer type inference
 * Targets: UnknownTypeInferrer
 *
 * After lifting, struct member accesses and pointer dereferences appear as
 * operations on variables with UnknownType; the inferrer assigns concrete types.
 * ========================================================================= */

typedef struct {
    int x, y, z;
} Vec3;

Vec3 vec_add(Vec3 a, Vec3 b) {
    Vec3 r;
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    r.z = a.z + b.z;
    return r;
}

int vec_dot(const Vec3 *a, const Vec3 *b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

/* Pointer walk exercises NULL comparison → infer pointer type */
typedef struct Node { int val; struct Node *next; } Node;

int list_sum(const Node *head) {
    int total = 0;
    while (head != NULL) {
        total += head->val;
        head = head->next;
    }
    return total;
}

/* =========================================================================
 * SECTION N: Recursion
 * Targets: general pipeline; recursive functions stress the value analysis
 *          and call-info obtainer used by several optimizers
 * ========================================================================= */

int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

uint64_t factorial(uint32_t n) {
    if (n <= 1) return 1;
    return (uint64_t)n * factorial(n - 1);
}

/* Mutual recursion */
static int is_even_r(int n);
static int is_odd_r(int n);
static int is_even_r(int n) { return n == 0 ? 1 : is_odd_r(n - 1); }
static int is_odd_r(int n)  { return n == 0 ? 0 : is_even_r(n - 1); }

/* =========================================================================
 * SECTION O: GCD / number theory
 * Targets: general pipeline — exercises integer division, modulo, and
 *          the while-loop optimizer with a non-trivial termination condition
 * ========================================================================= */

uint64_t gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = b; b = a % b; a = t; }
    return a;
}

int is_prime(uint32_t n) {
    if (n < 2) return 0;
    if (n == 2) return 1;
    if (n % 2 == 0) return 0;
    for (uint32_t i = 3; (uint64_t)i * i <= n; i += 2)
        if (n % i == 0) return 0;
    return 1;
}

/* =========================================================================
 * SECTION P: Dynamic allocation and error handling
 * Targets: general pipeline — exercises malloc/free, pointer arithmetic,
 *          and NULL checks (which help UnknownTypeInferrer)
 * ========================================================================= */

typedef struct {
    int *data;
    int  size;
    int  cap;
} IntVec;

IntVec *vec_new(void) {
    IntVec *v = (IntVec *)malloc(sizeof(IntVec));
    if (!v) return NULL;
    v->data = (int *)malloc(4 * sizeof(int));
    if (!v->data) { free(v); return NULL; }
    v->size = 0;
    v->cap  = 4;
    return v;
}

void vec_push(IntVec *v, int val) {
    if (v->size == v->cap) {
        v->cap *= 2;
        v->data = (int *)realloc(v->data, (size_t)v->cap * sizeof(int));
    }
    v->data[v->size++] = val;
}

void vec_free(IntVec *v) {
    if (v) { free(v->data); free(v); }
}

/* =========================================================================
 * main(): call every section so the compiler doesn't eliminate them
 * ========================================================================= */

int main(void) {
    /* Section B: casts */
    printf("widen=%d chain=%d sat=%d\n",
        widen_i16(1000), chained_cast(70000),
        saturate_add_u8(200, 100));

    /* Section C: if-to-switch */
    for (int d = 0; d <= 7; d++) printf("%s ", weekday_name(d));
    printf("\n");
    for (int c = 0; c <= 5; c++) printf("%s ", status_name(c));
    printf("\n");

    /* Section D: if-structure */
    for (int s = -5; s <= 100; s += 15)
        printf("classify(%d)=%d ", s, classify_score(s));
    printf("\n");
    printf("bounds=%d %d %d\n",
        check_bounds(5, 0, 10), check_bounds(-1, 0, 10), check_bounds(11, 0, 10));
    printf("char='A'→%s '1'→%s\n",
        classify_char('A'), classify_char('1'));

    /* Section E: loops */
    printf("sum_n=%d collatz=%d sum_odds=%d\n",
        sum_n(10), collatz_steps(27), sum_odds(20));
    int arr5[5] = {0, 0, 3, 0, 5};
    printf("find_nonzero=%d\n", find_first_nonzero(arr5, 5));
    int mat[4][4] = {{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}};
    printf("find_2d(11)=%d\n", find_in_matrix(mat, 11));
    printf("next_mult(7,3)=%d count_up(5)=%d\n",
        next_multiple(7, 3), count_upward(5));

    /* Section F: arithmetic */
    printf("mba_add(5,3)=%u mba_sub(10,3)=%u\n", mba_add(5, 3), mba_sub(10, 3));
    printf("pow2_ops(12)=%d bits=0x%x\n", pow2_ops(12), extract_bits(0xABCD));
    printf("equal_ops(7)=%u abs(-9)=%d grade(85)=%d\n",
        equal_operand_patterns(7u), branchless_abs(-9), grade(85));

    /* Section G: bit ops — condition forms for BitOpToLogOpOptimizer */
    printf("bit_and_cond=%d bit_or_cond=%d both=%d either=%d shift=%d\n",
        bit_and_condition(3, 5), bit_or_condition(-1, 0),
        both_conditions(3, 5), either_condition(-1, 0), shift_multiply(7));

    /* Section H: copy propagation */
    printf("simple_copy=%d dead_assign=%d dead_call=%d ext_copy=%d\n",
        simple_copy_prop(3, 4), dead_assign(5),
        dead_call_result(7), extended_copy(3));

    /* Section I: arrays and strings */
    printf("hello_bytes=%s empty_buf[0]=%d name0=%s name1=%s\n",
        g_hello_bytes, (int)g_empty_buf[0],
        g_name_table[0], g_name_table[1]);
    print_hello();
    use_empty_array();
    int iarr[5] = {1, 2, 3, 4, 5};
    printf("sum_ptr=%d\n", sum_via_ptr_arith(iarr, 5));
    int farr[4] = {0, 0, 0, 0};
    fill(farr, 4, 7);
    printf("fill[0]=%d\n", farr[0]);

    /* Section J: intrinsics */
    char srcbuf[16] = "hello world!!\0";
    char dstbuf[16];
    buffer_ops(dstbuf, srcbuf, 14);
    printf("buf_op done\n");

    /* Section K: void return and dead code */
    bump_counter(5);
    printf("counter=%d always_pos=%d find_pos=%d\n",
        g_counter, always_positive(-3), find_positive(arr5, 5));

    /* Section L: variable defs */
    printf("sum_range=%d sep_def=%d\n",
        sum_range(1, 11), separate_def_use(3, 4));

    /* Section M: structs */
    Vec3 v1 = {1, 2, 3}, v2 = {4, 5, 6};
    Vec3 v3 = vec_add(v1, v2);
    printf("vec_add=(%d,%d,%d) dot=%d\n",
        v3.x, v3.y, v3.z, vec_dot(&v1, &v2));
    Node n3 = {30, NULL};
    Node n2 = {20, &n3};
    Node n1 = {10, &n2};
    printf("list_sum=%d\n", list_sum(&n1));

    /* Section N: recursion */
    printf("fib(8)=%d fact(10)=%llu even(4)=%d odd(5)=%d\n",
        fibonacci(8), (unsigned long long)factorial(10),
        is_even_r(4), is_odd_r(5));

    /* Section O: GCD / number theory */
    printf("gcd(48,18)=%llu prime(17)=%d prime(18)=%d\n",
        (unsigned long long)gcd(48, 18), is_prime(17), is_prime(18));

    /* Section P: dynamic allocation */
    IntVec *vec = vec_new();
    if (vec) {
        for (int i = 0; i < 8; i++) vec_push(vec, i * i);
        printf("vec[4]=%d size=%d\n", vec->data[4], vec->size);
        vec_free(vec);
    }

    return 0;
}
