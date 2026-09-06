# Formal verification of the arithmetic every parser depends on

`scripts/verify_esbmc.sh` proves the verified kernels under
`include/retdec/utils/` correct with
[ESBMC](https://github.com/esbmc/esbmc), a bounded model checker, using an SMT
solver rather than test cases.

**272 properties across 13 verified kernels, all discharged.**

```bash
./scripts/verify_esbmc.sh              # every proof
./scripts/verify_esbmc.sh --syntax     # type-check the harnesses, no solver
./scripts/verify_esbmc.sh --list
./scripts/verify_esbmc.sh proof_page_count
./scripts/verify_esbmc.sh --cross      # every proof under two backends, verdicts diffed
./scripts/verify_esbmc.sh --routing    # does anything CALL each proved kernel?
./scripts/verify_esbmc.sh --optional   # include harnesses too big for this machine
SOLVER=--boolector ./scripts/verify_esbmc.sh
```

ESBMC is not packaged in Debian or Ubuntu. Take the release build:

```bash
curl -fsSL -o esbmc-linux.zip \
  https://github.com/esbmc/esbmc/releases/latest/download/esbmc-linux.zip
unzip -q esbmc-linux.zip && chmod +x release/bin/esbmc
export ESBMC=$PWD/release/bin/esbmc
```

## Why

Fuzzing shows a bug **exists**. It cannot show that one does not. Every crash
the fuzzer found in this tree was in the same few lines of arithmetic: a count,
length or offset read out of a file and used before anything checked the input
could supply it.

That arithmetic now lives in thirteen headers, and the harnesses under
`tests/verification/` prove it — for **all** inputs over the whole 64-bit range,
by SMT, not for sampled values. Where a harness has no loops no unwinding bound
applies at all and the result is a proof outright; where it walks a buffer the
bound is pinned in the harness with `// ESBMC-OPTIONS: --unwind N` and ESBMC's
unwinding assertions — on by default in 8.5.0 — make exceeding it a failure
rather than a silent truncation of the search.

The proofs cover the header the parsers call. A proof about a re-typed copy of
the logic would say nothing about the code that runs, so the rule is: do not
copy this arithmetic into a call site, call it.

## The kernels

Each of these exists because a survey found the same primitive re-derived at
several call sites with at least one of the copies wrong. The count is proofs
discharged, not properties claimed; the driver discovers them from the source,
so the table cannot drift from what runs.

| Header | The question it answers once | Proofs |
|---|---|---|
| `utils/bounds.h` | does this count, length or offset fit in what is left? | 30 |
| `utils/leb128.h` | where does this continuation-bit encoding stop? | 9 |
| `utils/bounded_string.h` | how long is a string the file need not terminate? | 6 |
| `utils/c_source_scan.h` | which characters of this source are comment or literal? | 5 |
| `utils/section_map.h` | which section holds this address, and where are its bytes? | 10 |
| `utils/byte_order.h` | what integer do these bytes spell, LE or BE? | 16 |
| `utils/scan_cursor.h` | does this walk over an untrusted buffer terminate? | 34 |
| `utils/index_translation.h` | this number the file supplied — is it a subscript? | 46 |
| `utils/align.h` | round a file-controlled value to a file-controlled alignment | 21 |
| `utils/text_transcode.h` | how many bytes does rendering this take, and did I have them? | 33 |
| `utils/branch_target.h` | where does this displacement branch to, and is it inside? | 16 |
| `utils/compressed_int.h` | ECMA-335 II.23.2 compressed integers | 19 |
| `utils/float_predicate.h` | decisions taken on floats that came out of a binary | 27 |
| | | **272** |

Every run enables `--overflow-check`, `--unsigned-overflow-check`,
`--ub-shift-check`, `--nan-check` and `--memory-leak-check`, so a proof fails if
its own arithmetic wraps. That is not a formality: several harnesses failed on
their first run because they formed the very overflow they were asserting about.

## What is proved

### `utils/bounds.h` — count, length and offset arithmetic

| Property | What it rules out |
|---|---|
| `remaining` is exact and saturating | a position past the end reporting `SIZE_MAX` bytes left |
| `addFits` is sound **and** complete | both a wrapping sum and a needlessly rejected one |
| `mulFits` is sound and complete (per multiplier) | a product that wraps before it is compared |
| `rangeFits` implies `addFits` | the classic `pos + len <= size`, which passes when the sum wraps |
| `rangeFits` is complete | a well-formed range being rejected |
| `rangeFits` refuses every wrapping range | the same bug reached from the other side |
| `countFits` composes with `rangeFits` (per element width) | a declared count that cannot be satisfied by the bytes left |
| `countFits` is not vacuous at zero width | a header declaring zero-width elements turning the bound off |
| `signedCountFits` rejects negatives before conversion | `-1` becoming `SIZE_MAX` on the way into `reserve()` |
| `pageCount` covers and is minimal | rounding the byte count up instead of the page count |
| `pageCount` does not divide by zero | a trap on a zero page size |
| `reserveFor` is bounded by both the cap and the count | committing memory for a count the file merely claimed |
| `arrayFits` is sound (per element size) | `count * elementSize` wrapping before the bounds check |

### `utils/leb128.h` — continuation-bit decoding

LEB128 has no length prefix, so the decoder decides for itself when to stop.
Hand-written ones get that wrong in two ways, both undefined behaviour rather
than merely wrong answers. The buffer is symbolic in every proof below — each
byte an unconstrained choice — and the decode loop is bounded by `kMaxBytes`,
with ESBMC's unwinding assertions making that bound part of the proof rather
than an assumption.

| Property | What it rules out |
|---|---|
| The decoder never indexes outside the buffer | a stream that never clears the continuation bit walking off the end |
| No shift by 64 or more, and no lossy shift | `x << shift` past the accumulator width, which is undefined |
| A failed decode reports `bytesRead == 0` | a caller advancing its cursor into nowhere after a refusal |
| An unterminated encoding is refused, never guessed at | a truncated table decoding as a plausible value |
| An over-long encoding is refused | bits silently discarded from a value that did not fit |
| One- and two-byte encodings equal their definition | a safety-only proof that says nothing about correctness |
| Sign extension is exact | the signed-accumulator formulation, undefined twice over |
| `toSigned` is total and reversible | a cast that assumes its input is in range |

### `utils/bounded_string.h` — measuring a string the file need not terminate

Every parser reaches a name eventually — a section name, a metadata version, a
symbol, a type — and both the bytes and the length that is supposed to bound
them come out of the file. `strlen` on such a pointer reads until it finds a
zero byte, which may be past the end of the mapping; taking `std::min` with the
declared length afterwards does not help, because the read has already happened.

That bug appeared four separate times in one review pass — in the .NET metadata
root, twice in the PDB symbol walker, and in the PDB type field list — and was
fixed four separate ways. It is one rule, so it is stated once and proved once,
and those four sites call it.

The property that matters is not only that the answer is within the bound, but
that no byte at or beyond it is read. That is not expressible as an assertion
about a return value, so the proof buffers are exactly the scanned size and
ESBMC's array-bounds checking carries it: a scan one byte too far is reported as
an out-of-bounds access. The bytes and the length are symbolic; the loop is
bounded, so this is a proof over every buffer up to that size.

| Property | What it rules out |
|---|---|
| The terminator found is the first one, and inside the bound | a length measured past an earlier NUL |
| `npos` is reported only when there genuinely is none | a silent zero length for an unterminated field |
| `boundedLength` never exceeds its bound | the `strlen`-then-`min` shape, where the read precedes the clamp |
| `boundedLength` stops at the bound or at a terminator, never before | a field padded with NULs measured short |
| The two entry points agree | one convention drifting from the other as callers pick between them |
| A null pointer is never dereferenced | a caller that computed its own offset wrongly |

### `utils/c_source_scan.h` — blanking comments and literals

The neural refinement gate compares the control-flow shape of the original and
refined function. Counting over raw text lets a refinement introduce a real
`system()` call while deleting a comment mentioning the same word, so the counts
cancel and the call passes; blanking comment and literal contents first closes
that. The scanner reads and writes one character ahead of its cursor, which is
where this shape of loop goes wrong.

The input buffer is fully symbolic, so ESBMC's array-bounds checking carries
these proofs: if any input at all could drive an index outside either array, the
run fails.

| Property | What it rules out |
|---|---|
| No index leaves either buffer | the one-character lookahead stepping past the last character |
| A short length is respected | writing into the caller's tail past `n` |
| Newlines survive | line numbers computed from the result drifting from the input |
| Every output character is the input character or a space | the scanner inventing a keyword, operator or quote |
| A null pointer or zero length is a no-op | a crash on `.data()` of an empty string |

### `utils/section_map.h` — address to file offset

Every format here is mapped differently from the way it is stored: a PE maps at
`SectionAlignment` and stores at `FileAlignment`, and every ELF segment after
the first is skewed the same way. Three translators had been written
independently — `PeReader::rvaToOffset`, `LoaderSim::vaToOffset`, and the
boundary test in `func_boundary` — and **two of the three tested containment
with `addr < s.address + max(virtSize, rawSize)` in 32-bit arithmetic.**

That sum wraps. ESBMC returns the witness `va = 2692743171`,
`span = 2675966078`, `rva = 3221225600`: the true end is `5368709249`, the
section does contain the address, the sum wrapped to `1073741953`, and the test
said no — so the address resolves into whichever *later* section happens to
match, and every byte read through it comes from the wrong part of the file.
`proof_containment_never_wraps` is that counterexample turned into a property.

The whole section table is symbolic in every proof: each field an unconstrained
64-bit choice, so these hold for every table a file could declare.

| Property | What it rules out |
|---|---|
| `contains` never forms the end address at all | the wrap above, in either direction |
| `contains` agrees with exact 128-bit-wide arithmetic | a merely-different answer passing as a fix |
| An empty section contains nothing | a zero-length section swallowing an address |
| The offset is readable or is `kUnmapped`, never anything else | a file-controlled sum returned as if it were an offset |
| A mapped answer comes from a section that really stored the byte | satisfying the sentinel by returning any in-range number |
| The virtual tail of a `.bss` is unmapped | reading whatever section follows on disk |
| A readable span stays inside the file | a length that fits the section but not the file |

### `utils/byte_order.h` — bytes to a fixed-width integer

Fourteen surveys found the same primitive under nine names —
`ByteValueStorage::createValueFromBytes`, `DynamicBuffer::readImpl`,
`PeReader::read16/32/64`, `MetadataTables::RowReader::u8/u16/u32`, JVM
`BinaryReader::u2/u4/u8`, DEX `readEncodedBits`, and more. Most get the shift
bound wrong in at least one direction and three are undefined behaviour today.

`lowMask` is built as `~leb128::maskFrom(bits)` so there is one mask
implementation in the tree, exact at both ends with no special case. Range
checking is delegated to `bounds::rangeFits` rather than re-derived.

Headline properties: `proof_read_le_accepts_exactly_the_reads_that_fit` (sound
*and* complete, so a correct read is never refused either),
`proof_no_shift_count_reaches_the_width_of_its_operand` (the UB),
`proof_reads_touch_no_byte_outside_an_exactly_sized_buffer`,
`proof_le_round_trip` / `proof_be_round_trip`, and
`proof_sign_extend_fills_the_top_from_the_sign_bit`.

### `utils/scan_cursor.h` — a walk that cannot fail to advance

`bounds.h` answers "does this read fit". It does not answer "does this loop
finish", and every `timeout-*` and `oom-*` artifact in this tree is the second
question. Nine independent instances of the same mistake were found, and they
are one rule: a walk over an untrusted buffer must advance strictly, must stay
in range, and a refusal must leave the cursor where the caller can see it.

The termination argument is proved rather than asserted:
`proof_the_walk_bound_holds_initially` and `proof_the_walk_bound_is_inductive`
are the base case and the step, and `proof_a_walk_takes_at_most_left_steps` is
the conclusion. `proof_max_steps_is_a_termination_bound` gives a caller a number
it can loop to. `proof_a_refused_advance_leaves_the_cursor_bitwise_unchanged`
closes the failure path, which is where the real bugs were.

### `utils/index_translation.h` — a number the file supplied, about to be a subscript

Two halves. The 1-based half is .NET metadata, where row 0 is the null token, so
every read is `index - 1` and every one of them underflows on a token the file
is free to set to zero. The coded-token half is the ECMA-335 tag/row split,
where the tag mask and the row shift have to agree and three call sites had them
transposed.

`proof_slot_refutes_both_off_by_one_spellings` states the two wrong forms
explicitly and refutes them, so the fix cannot regress to either.
`proof_the_guid_heap_wraps_at_index_16m` is a live defect turned into a
property: `(index - 1) * 16` formed in 32 bits wraps at index 2^28, and the
wrapped offset passes the subsequent bounds check.

The thirteen `*_stays_in_table` proofs cover every coded-token kind
`cli_tables.cpp` declares — eleven with a decoder, plus `HasFieldMarshal` and
`HasDeclSecurity`, which are reached through `RowReader::codedToken` instead and
had been missed. They split by shape, and the split is a `static_assert` rather
than a choice at the call site: where the mask admits exactly the table's
entries the subscript is proved in bounds **with no guard at all** — which is
why the four unguarded decoders in `cli_tables.cpp` are sound — and where the
mask is wider, the guard is proved to refuse a tag the mask really produces.

An earlier version of this block put every subscript inside the guard, which
made the array-bounds check satisfiable by construction: the adversarial audit
demonstrated it by widening a 2-bit tag to 5 bits over the same 3-entry table
and watching it still verify. Eleven proofs were passing without establishing
the property they were named for. That is the failure mode this whole exercise
is most exposed to, and it is why every kernel here is audited by someone whose
job is to disbelieve it.

### `utils/align.h` — rounding to an alignment the file chose

Alignment rounding appears six times in three spellings and three are wrong.
`alignDown(value + (alignment - 1), alignment)` forms the sum first, so for a
value within `alignment - 1` of `UINT64_MAX` **the result is smaller than the
input** — a cursor rounded forward moves backwards. `value & (alignment - 1)`
masks without asking whether that is a mask. Both are the body of the tree's own
public helpers in `src/utils/alignment.cpp`, which now call this kernel.

`proof_align_up_never_moves_backwards` and
`proof_align_up_is_the_least_aligned_value_at_or_above` are the two halves of
correctness; `proof_align_down_refuses_every_non_power_of_two` and
`proof_is_aligned_to_refuses_a_zero_alignment` cover the alignments a malformed
header can supply. `proof_no_arithmetic_in_the_header_wraps` calls every entry
point with unconstrained arguments and asserts nothing, which under
`--overflow-check` is the statement that no input at all makes the header wrap.

### `utils/text_transcode.h` — sizing a rendering before performing it

`bounded_string.h` owns "where does this string end". Nothing owned "how many
bytes does rendering it take, and did I have that many", so every converter
sized its own output and each sized it with an unchecked multiplication —
`size * 3 - 1` handed straight to `resize()`, `resize(size * 2)` then writes at
`i*2 + 1`, and so on.

Every entry point is `f(in, n, out, outCap, ...)` and refuses to write a byte
unless `outCap` covers the matching `...Capacity(n)`, so the capacity and the
conversion can no longer be stated in two places. The sizing lemmas are
loop-free over the whole 64-bit domain; the whole-function proofs run the real
loops over exactly-sized symbolic buffers, so array-bounds checking carries
"never writes outside the caller's buffer".

Covers hex out, hex in, bit strings, UTF-16LE → UTF-8, MUTF-8 → UTF-8 and
decimal runs. `proof_utf16_never_emits_a_surrogate` and
`proof_utf16_lone_surrogate_becomes_replacement` are the well-formedness
properties a `.NET` string reader needs;
`proof_mutf8_terminates_on_leads_that_encode_nothing` is the DEX one.

### `utils/branch_target.h` — where a displacement lands

Every bytecode lifter asks the same question and three answer it three ways, two
of them wrong. `cil_lifter.cpp` widens to `int64` — so the addition is right —
and then truncates back to `uint32` with no range check: `pos = 2`,
`delta = -128` gives `-126`, which truncates to `0xFFFFFF82` and is recorded as
a basic-block leader 4 GB away.

Every entry point is total — defined for `base = UINT64_MAX`,
`delta = INT64_MIN`, `codeSize = 0`, with no precondition a caller can violate —
and writes its out parameter **only** on success
(`proof_refusal_never_writes_the_target`). `proof_relative_at_int64_min_does_not_negate`
covers the one delta that has no positive counterpart. A `static_assert` on
`sizeof(size_t)` makes a 32-bit build fail to compile rather than silently
truncate a 64-bit offset into the checker.

### `utils/compressed_int.h` — ECMA-335 II.23.2

Every signature, blob-heap entry and user string in a .NET image begins with one
of these, and `src/cli_parser` reads them at sixteen call sites. They are *not*
LEB128 and deliberately do not live in `leb128.h`: LEB128 discovers its width
one byte at a time and fails by running on, this encoding declares its width in
the first byte's top bits and fails by being truncated.

The proofs found the tree's sign handling wrong at **all three widths**, not
just the one-byte form the survey had named. They also refuted the spec's own
formula for the signed decode — it omits the rotate — and the corrected form is
what is proved.

### `utils/float_predicate.h` — decisions taken on floats out of a binary

Four decisions in this tree are taken on doubles derived from an input file, and
in three of them a NaN or an out-of-range operand silently answers "no" instead
of being refused: `areEqualFPWithEpsilon`, `isNiceString`,
`isNiceAsciiWideString` and `GpuScanner::fileEntropy`.

Every operand is a fully symbolic `double` or `float` — every sign, exponent and
significand, so NaN, both infinities, both zeros and the subnormals are all in
range unless a proof says otherwise. Nothing here samples values.

`--nan-check` and `--overflow-check` do as much work as the assertions here:
`--overflow-check` reports any *floating-point* operation whose result is
infinite, so `proof_nearly_equal_never_overflows_or_nans` — which calls the
function with unconstrained arguments and asserts nothing about the answer — is
the real statement that no input at all makes it form an infinity or a NaN. The
same check refutes `equality.h:45` as written, reporting
`arithmetic overflow on floating-point ieee_sub` for `x = DBL_MAX`,
`y = -DBL_MAX`.

The logarithm inside `entropyBitsWith` is instantiated with a function returning
an *unconstrained* double, so the entropy proofs hold for every possible
behaviour of libm rather than for the one ESBMC models. That is also the only
way they discharge: a single symbolic call to ESBMC's `std::log2` model did not
return in 180s.

## Which solver, and why the answer is not "whichever"

A proof is a claim about a program, not about a solver, so a harness may pin its
backend with `// ESBMC-SOLVER: --z3` and must say why. Four backends are in
play and they are not interchangeable:

| Harness | Pinned | Why |
|---|---|---|
| `bounds_proof.cpp`, `scan_cursor_proof.cpp` | `--z3` | they divide — see below |
| `section_map_proof.cpp`, `align_proof.cpp`, `index_translation_proof.cpp`, `text_transcode_proof.cpp`, `compressed_int_proof.cpp` | `--boolector` | bitvector work, and the margin is not marginal: the four table-walking `section_map` properties run past 300s under z3 and discharge in 0–22s under boolector |
| `float_predicate_proof.cpp` | `--cvc5` | floating point. cvc5 discharges all 27, none over 80s; z3 manages 8 and runs two past 300s; bitwuzla reports "SMT solver failed" on the same two; boolector answers — ESBMC bit-blasts the float theory for a backend that lacks one — but pays for it, taking 67s on a proof cvc5 does in under 5 and not returning within 240s on another |
| everything else | default | the backends agree |

**The division artifact.** ESBMC emits an `arithmetic overflow on div` check for
*unsigned* division, which cannot overflow in C++. Boolector and bitwuzla find a
witness for it — the signed `INT64_MIN / -1` pair — and report a violation; z3
does not. The false alarm is in the safe direction, since it invents a bug
rather than hiding one, but a harness that divides has to say which backend its
verdict came from. It only bites when the *divisor* is symbolic, which is why
`index_translation.h` calls `bounds::mulFits(width, count)` rather than
`mulFits(count, width)` — the same predicate, since multiplication commutes, but
`mulFits` divides by its first argument, so the divisor is the concrete format
width. That argument order is documented in the header as deliberate.

**One proof per process, and one process at a time.** The driver verifies each
property in its own ESBMC run so a counterexample names the property rather than
the file, and `TIMEOUT` (default 300s) applies per property. Those budgets were
measured on an idle machine. The four table-walking `section_map` properties
discharge in 0–22s alone and time out at 300s when several ESBMC processes are
competing for the same cores, so a timeout on a loaded box is a scheduling
result and not a verdict — re-run it quietly, or raise `TIMEOUT`, before
recording it as a failure.

**`--cross` is how any of this is known.** It runs every proof under z3 *and*
boolector and reports each disagreement. Harnesses that pin a solver are
reported as expected disagreement; anything else is a finding and fails the run,
because a property two solvers disagree about is not proved. Everything above
was measured with it rather than assumed.

Two backends that both run out of time are reported as **no verdict**, not as
agreement. They have said nothing about the property, and counting silence as
consensus is how a cross-check comes back clean on a file it never checked. The
summary names the count, so a run where the budget was too small looks different
from one where it was enough.

## How the proofs are checked

A passing proof is evidence about a harness, not about a kernel. Three things
stand between the two, and all three are things that have actually caught
something here.

**A proof must be watched failing.** Break the property the kernel exists to
enforce — inject the defect, run the proof, see it fail, restore. A proof that
passes against a broken implementation is not load-bearing, and the only way to
know which kind you have written is to check. This is the rule that caught the
`index_translation` block described above: the five exactly-fitting coded-token
kinds now fail when the mask is dropped from `splitTag`, and under the previous
form that same defect passed.

**Two backends must agree.** `--cross` runs every proof under z3 and boolector.
A property two solvers disagree about is not proved, and the one disagreement
that is a tool artifact rather than a defect — the spurious unsigned-division
overflow — is declared by the harness that hits it rather than explained away
after the fact.

**Someone has to disbelieve it.** Each kernel is audited by an agent whose brief
is to refute the claims made for it: re-run every proof, inject defects the
author did not, and check every cross-file citation against the file it names.
The audit is not a formality — it returned "weak" on most of the kernels, and
what it found was the kind of thing that survives careful review:

* eleven proofs whose subscript sat inside the guard, so the array-bounds check
  they claimed as their property was satisfied by construction — demonstrated by
  widening a 2-bit tag to 5 bits over the same 3-entry table and watching the
  proof still pass;
* a proof named for `writeBE` that only ever called `writeLE`;
* a proof named "never writes past the cap" whose buffer was as large as the
  largest cap it allowed, so the check could not fire;
* a proof that re-typed the kernel's own guard into the harness and then proved
  a property of the copy;
* a proof whose two arms were both slack at the tolerance it was instantiated
  with, so it could not have failed for any implementation that answered
  "not equal" there;
* an explanation of a refutation that was confidently wrong about floating
  point — it blamed rounding at `hi = 3`, where a scan of all 2040 binades finds
  no normal value that rounds that way at all; the real mechanism is a subnormal
  operand, where the multiplication has no room to round down;
* a witness quoted in five files whose arithmetic was wrong — `alignUp` returned
  0 for that input, not 256 — and line citations, in four more places, pointing
  at code the same change had already moved.

The audit also runs `--routing`, and on one kernel that is how the gap was
caught: `float_predicate.h` was proved, and every fault it documents was still
live in shipping code because nothing called it.

None of those were unsound: no proof passed that should have failed. All of them
were claims stronger than what was discharged, which is the failure mode this
approach is most exposed to, because it is invisible from a green run.

## What is not proved

**The parsers themselves, mostly** — but the reason given here for a long time
was wrong, and the limit is narrower than it claimed.

This section used to say that whole-module verification is unavailable because
ESBMC's models of the C++ standard library do not stretch to this codebase.
Measured against ESBMC 8.5.0, that is not so. `std::vector` (with a symbolic
size), `std::string`, `std::optional`, `std::variant` and **classes with methods
and member state** all verify. Under `--std c++20`, so does `std::span`. ESBMC
parses the real `include/retdec/cli_parser/pe_reader.h`, instantiates a
`PeReader`, and checks 58 properties on it; given `src/cli_parser/pe_reader.cpp`
as well (`// ESBMC-LINK:` in the harness), it verifies `PeReader::open` over a
fully symbolic image.

What actually limits whole-function verification is two things:

* **`std::unique_ptr` is a parse error.** ESBMC's own C++ library model
  conflicts with the deleted copy constructor, so any header reaching a
  `unique_ptr` fails before verification starts. That excludes `retdec/ssa/ssa.h`
  and everything built on it.
* **Memory, for whole-function verification of a real parser.** This is the
  hard one, and it is worth stating precisely because the previous version of
  this section implied whole-function proofs were available and they are not.

  `tests/verification/pe_reader_proof.cpp` links the real
  `src/cli_parser/pe_reader.cpp` and states seven properties about
  `PeReader::open` — that it reads no byte outside the file, that a refusal
  leaves the reader invalid, that `rvaToOffset` answers in range or with its
  sentinel over a section table the parser built itself. Every backend this
  build has is **killed by the OOM killer** trying to discharge them, at
  `--unwind 2`, which is the smallest configuration that reaches `open` at all
  and leaves only 7,216 verification conditions:

  | Backend | Result |
  |---|---|
  | boolector | killed at 13.9 GB anon-rss |
  | z3 | killed |
  | bitwuzla | killed |
  | cvc5 | killed |

  Thirteen kills across the four, in `dmesg`, on a 4-core / 15 GB machine. Not
  a timeout: the solver never returned a verdict because the process ceased to
  exist. What blows up is not the parser — ESBMC symexes the whole translation
  unit, which pulls in its models of `std::vector`, `std::string` and
  `std::span` for `PeReader`'s members. Constructing a `PeReader` and calling
  `isValid()`, with the same file linked, discharges in 1.3s.

  So the harness carries `// ESBMC-OPTIONAL:` and does not run in the default
  suite. `--optional` runs it on a machine with more memory. It is not counted
  among the 272, and the driver prints the reason it was skipped on every run,
  so a harness that is not being verified says so rather than disappearing.

* **Cost, past a small symbolic input** — and the shape of the input matters
  more than its size. `PeReader::open` over a *fully* symbolic buffer discharges
  in 2.4s at 16 bytes and 2.4s at 24, and does not return within 240s at 64
  (boolector, with ESBMC's own `strlen`/`strcpy` loops bounded by name; see
  below). Those first two numbers look encouraging and are worth nothing:
  `open` refuses anything under `0x40` bytes at its first check, so at 16 and 24
  bytes the proof is about the refusal and not about the parser. 64 is the
  smallest size that reaches any of it, and 64 fully symbolic bytes is already
  out of reach.

  What is in reach is a buffer whose *fields* are symbolic and whose filler is
  not, which is what `tests/verification/pe_reader_proof.cpp` does: every value
  a malformed file actually controls at these paths — the MZ word, the PE
  offset, Machine, SizeOfOptionalHeader, and every field of the section table —
  is unconstrained, and the bytes no path reads are concrete.

  One more thing had to be dealt with before any of this worked. ESBMC's own C++
  library models contain loops: `strlen` walks to a NUL, `strcpy` and the
  `std::string` copy walk a length. The error strings this parser assigns on its
  refusal paths drive those past any `--unwind` small enough for the parser's
  own loops, and raising `--unwind` to suit them puts the section-table walk at
  the same bound and the run stops returning. `--unwindsetname` bounds those
  library loops by name and leaves `--unwind` free to be what the parser needs.
  Without it the first result is a refutation naming
  `unwinding assertion loop 46` inside `/esbmc-vfs/libc/library/string.c` — a
  bound that was too small, reported exactly as a defect would be.

So the position is: the arithmetic every parser depends on is proved over the
whole 64-bit domain, whole-function proofs of a real parser are written and
reproducible but need more memory than this machine has, and coverage of the
rest is the job of the unit suites and `scripts/standalone_fuzz.sh`. `std::min` with an
`initializer_list` is genuinely unsupported; use nested `std::min`.

**The connection between a proof and the code that runs.** This is the honest
weak point of the whole approach. A proof about a re-typed copy of the logic
says nothing about the copy that runs — and this tree has demonstrated it
repeatedly: `src/utils`, the module the proved headers live in, included none of
them while carrying eight re-derivations of what they prove, every one wrong in
the way the header documents. The rule is not "prove the arithmetic"; it is
"prove it once and call it".

Half of that is now checked rather than reviewed. `--routing` counts, for every
kernel with a harness, the files under `src/` and `include/` that include it,
and fails on a kernel with none: a proof about code nothing calls is a proof
about code that does not run, and the bug it was written to stop is still in the
tree in whichever copy nobody routed. A kernel may be listed in
`UNROUTED_KERNELS` in the driver, but the list takes a reason and not just a
name.

What `--routing` cannot check is the other half — that a file which *includes* a
kernel actually calls it everywhere it should, rather than at one site while
three others still spell the arithmetic out. That remains a review job, and the
`--audit` mode of `scripts/standalone_check.sh` plus the fuzzer are what stand
behind it.

**A large input.** The loop-bearing harnesses model an 8- to 24-byte buffer.
That is enough for every token the scanners recognise plus a character at each
end to sit the lookahead against, and the loops are structurally identical at
any length — but it is a bounded model, not an inductive proof over arbitrary
lengths. Two exceptions, where induction *is* done explicitly: the `scan_cursor`
walk bound has a base case and a step (`proof_the_walk_bound_holds_initially`,
`proof_the_walk_bound_is_inductive`), so its termination argument holds at any
length.

**A symbolic element width.** A query holding both a symbolic 64-bit
multiplication and a symbolic 64-bit division is nonlinear bitvector arithmetic,
and none of Z3, Boolector or Bitwuzla discharges it — they run out of time at 16
bits as readily as at 64. Properties that multiply a count by a width are
therefore proved once per width that actually occurs in these formats (1, 2, 4,
8, 12, 16, 24, 40, 46 bytes). Position, size and count stay fully symbolic
across the whole 64-bit range in every one of those.

**Absence of a product that fits.** `pageCount(bytes, pageSize) * pageSize` is
not always representable — `bytes = SIZE_MAX` with `pageSize = 2` gives a count
of 2^63 whose product wraps. ESBMC found that while refuting an earlier, wrong
version of the property. The header documents the contract; the proof states
coverage by division instead.

**The `section_map` permissiveness note.** `contains` uses
`max(virtSize, rawSize)` as the span. That is *more* permissive than the Windows
loader, which uses `VirtualSize` when it is non-zero. It is what both callers
did before, so routing them through the kernel preserves their behaviour
exactly and changes only the wrap; tightening it is a separate decision with its
own compatibility risk. `func_boundary` uses the stricter test deliberately and
is **not** routed through this kernel. The header says so.

## What it found

Writing the proofs was not a formality. The solver refuted claims about the
kernels themselves, refuted two of the harnesses, and — running the same
properties against the expressions as currently written in `src/` — produced
counterexamples for live defects. A selection, each with the witness ESBMC
returned:

**In the kernels, before they had callers:**

* `countFits` accepted a count of zero when the caller was already past the end
  of the buffer, because `remaining()` saturates. That broke composition with
  `rangeFits`, which requires `pos <= size`. Counterexample:
  `pos = SIZE_MAX - 34, size = 0, count = 0`. The header now checks
  `pos <= size` explicitly.
* `pageCount(...) * pageSize` was asserted representable. It is not, as above.
* The first LEB128 decoder shifted a byte's payload past the top of the
  accumulator. That is defined for an unsigned type — the bits are simply
  dropped — but dropping them silently is how an over-long encoding decodes as a
  plausible value. The header now masks the payload to the bits that still fit.
* The `compressed_int` specification's own formula for the signed decode is
  wrong: it omits the rotate. The corrected form is what is proved.

**In the harnesses:** one asserted `a + b < a` to show a sum wraps, and one
built a symbolic 64-bit value with `v = (v << 8) | byte`. Both commit the fault
they were meant to be proving absent. A proof that starts by discarding
information is not a proof, and `--overflow-check` was right to say so. A third
tried to state "the offset came from this section" as
`rawOffset + delta == off`, forming the very sum the kernel refuses to form
until it knows it fits; it is stated by subtraction instead.

**In code that ships:**

* **Section-end wrap, two of three address translators.**
  `va = 2692743171, span = 2675966078, rva = 3221225600`; true end `5368709249`
  wrapped to `1073741953`, so a containing section is passed over and the
  address resolves into a later one.
* **`PeReader::rvaToOffset` returned offset 4294901764 for a 608-byte file.**
* **`alignUp(18446744073709551440, 512)` returned 0** — the sum wrapped to 335
  and the mask took that to nothing, so a cursor rounded forward landed at the
  start of the buffer. `alignUp(18446744073709551614, 4)` returned 0 the same
  way. Run and confirmed, not only refuted.
* **`isAligned(128, 0x8000000000000003)` answered "aligned" with remainder 0**,
  while 128 mod that alignment is 128. `pe_format_parser.h:123` feeds this a raw
  PE `FileAlignment`, so a malformed file got to choose whether its own
  header anomaly was noticed.
* **`DynamicBuffer::writeRepeatingByte` wrote ~4 GB past a 100-byte vector**
  at capacity 100, position 200, repeat amount 1. ASan-confirmed.
* **The CIL exception-section walk runs backwards.**
  `sectStart = (sectStart + 3) & ~3ULL` at `cil_lifter.cpp:326`, witness
  `sectStart = 18446744073709551614`, result 0 — the walk restarts at the top of
  the method body.
* **A CIL switch's every case label is measured from a truncated base.**
  `codeSize = 8192, pos = 5, n = 3221234185`: the true end is `12884936745`,
  truncated to `34857`, which is 26665 bytes past the end of the method.
* **A .NET GUID heap index wraps at 2^28.** `(index - 1) * 16` formed in 32
  bits; the wrapped offset then *passes* the bounds check and 16 bytes are
  copied from the wrong place, with no caller able to tell.
* **An infinite loop introduced by an earlier security fix** in
  `PDBSymbols::parse_symbols`, where a `continue` skipped the position advance.
  Found by the `scan_cursor` termination properties, not by a crash.
* **A 2 GB allocation out of a five-byte signature.** `01 DF FF FF FF` in an
  ArrayShape: the one-byte `0x01` gives rank 1, the four-byte `DF FF FF FF`
  gives a size count of 536,870,911 with nothing left to read, and
  `.value_or(0)` on each subsequent refusal let the loop run its full declared
  length pushing an element per iteration.
* **`areEqual(+inf, -inf)` returned true.** `std::isinf(x) == std::isinf(y)`
  compares two `bool`s, so it holds whenever both are infinite regardless of
  sign. Two constants as far apart as floating point can express, reported
  equal, in a predicate `src/cpdetect/search.cpp:528` sorts by.
* **`isNiceString` answered "not nice" for every string** given a NaN ratio, and
  "nice" for a string of pure control bytes given a negative one — its only
  bound was an `assert`, which is compiled out of every release build.
* **`GpuScanner::fileEntropy` returned 0.0 for a window past the end of the
  file**, by underflowing `hi - lo + 1` and then running no histogram loop. 0.0
  is the strongest statement it can make about a range, and an invalid range was
  making it.
* **Overlong MUTF-8 was decoded and re-encoded short**: `C0 AF` and `E0 80 AF`
  both became `/`, `C1 BF` became `0x7F`. A name checked before decoding and
  used after it are then two different names.
* **A `#Blob` constant was read with `memcpy` into a `uint32_t`**, which is
  host-endian; ECMA-335 II.22.9 stores it little-endian, so on a big-endian host
  the constant came back byte-swapped.

The full set of SMT-confirmed defects, with the counterexample for each, is
tracked in the fix log rather than here.

## Adding a proof

Add an `extern "C" void proof_<name>()` to a file under `tests/verification/`.
The driver discovers entry points from the source, so nothing else needs
updating — a proof cannot be forgotten by leaving it out of a list.

Inside a harness: take inputs from `nondet_size()` / `nondet_u64()` /
`nondet_int64()`, constrain them with `__ESBMC_assume`, and `assert` the
property. Never form a value you are claiming does not overflow — state it as
the subtraction or division instead, or the overflow check will fail your proof
on its own arithmetic.

A harness may carry four directives. Each must begin a `//` line at the left
margin; the driver checks that, because a directive written inside a `/** */`
block is silently invisible and the harness then runs with the wrong options and
still reports a verdict. That happened once.

| Directive | Meaning |
|---|---|
| `// ESBMC-OPTIONS: --unwind 14` | extra flags. A loop-bearing harness needs an unwind bound, and it belongs next to the code whose bound it is. Never pass `--no-unwinding-assertions`: unwinding assertions are on by default in 8.5.0, and they are what makes too small a bound fail loudly instead of silently truncating the search. |
| `// ESBMC-SOLVER: --boolector` | pin a backend, with a comment saying why. See the solver section above. |
| `// ESBMC-STD: c++20` | the C++ standard for this harness. Defaults to `c++17`, matching the tree; `std::span` needs `c++20`. |
| `// ESBMC-LINK: src/cli_parser/pe_reader.cpp` | link the implementation. Without it a harness calling a real parser verifies against an empty body and proves nothing about it. The driver fails if the named file does not exist. |

A new proof is not finished when it passes. Break the property deliberately —
inject the defect the kernel exists to stop — and watch the proof fail; then
restore it. A property that passes against a broken implementation is not
load-bearing, and the only way to know which kind you have written is to check.
