# Formal verification of the bounds arithmetic

`scripts/verify_esbmc.sh` proves the arithmetic in
`include/retdec/utils/bounds.h` correct with
[ESBMC](https://github.com/esbmc/esbmc), a bounded model checker, using an SMT
solver rather than test cases.

```bash
./scripts/verify_esbmc.sh              # every proof (~10 min)
./scripts/verify_esbmc.sh --syntax     # type-check the harnesses, no solver
./scripts/verify_esbmc.sh --list
./scripts/verify_esbmc.sh proof_page_count
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

That arithmetic now lives in one header, and the harnesses under
`tests/verification/` prove it — for **all** inputs over the whole 64-bit range,
by SMT, not for sampled values. The harnesses contain no loops, so no unwinding
bound applies and the results are proofs rather than bounded searches.

The proofs cover the header the parsers call. A proof about a re-typed copy of
the logic would say nothing about the code that runs, so the rule is: do not
copy this arithmetic into a call site, call it.

## What is proved

45 properties across three headers, all discharged.

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

Every run also enables `--overflow-check`, `--unsigned-overflow-check`,
`--nan-check` and `--memory-leak-check`, so a proof fails if its own arithmetic
wraps. That is not a formality: two harnesses failed on the first run because
they formed the very overflow they were asserting about.

## What is not proved

**The parsers themselves.** They call the verified headers rather than
re-deriving the arithmetic — `mini_emu` for page arithmetic, `eh_reconstruct`
for LEB128 accumulation and sign extension, and the bytecode parsers for their
declared counts — but the surrounding code is not itself verified. ESBMC's
operational models of the C++ standard
library do not stretch to this codebase — `std::variant` is capped at four
alternatives (`MarshalObject::Value` has eight) and `std::min` has no
`initializer_list` overload — so whole-module verification is unavailable. What
the proofs give is that the arithmetic every parser depends on is correct, and
the parsers call it instead of re-deriving it. Coverage of the call sites
themselves is the job of the unit suites and `scripts/standalone_fuzz.sh`.

**A large input.** The proofs model an 8- to 12-byte buffer. That is enough for
every token the scanners recognise plus a character at each end to sit the
lookahead against, and the loops are structurally identical at any length — but
it is a bounded model, not an inductive proof over arbitrary lengths.

**A symbolic element width.** A query holding both a symbolic 64-bit
multiplication and a symbolic 64-bit division is nonlinear bitvector arithmetic,
and none of Z3, Boolector or Bitwuzla discharges it — they run out of time at 16
bits as readily as at 64. Properties that multiply a count by a width are
therefore proved once per width that actually occurs in these formats (1, 2, 4,
8, 12, 16 bytes). Position, size and count stay fully symbolic across the whole
64-bit range in every one of those.

**Absence of a product that fits.** `pageCount(bytes, pageSize) * pageSize` is
not always representable — `bytes = SIZE_MAX` with `pageSize = 2` gives a count
of 2^63 whose product wraps. ESBMC found that while refuting an earlier, wrong
version of the property. The header documents the contract; the proof states
coverage by division instead.

## What it found

Writing the proofs was not a formality — the solver refuted two claims:

* `countFits` accepted a count of zero when the caller was already past the end
  of the buffer, because `remaining()` saturates. That broke composition with
  `rangeFits`, which requires `pos <= size`. Counterexample:
  `pos = SIZE_MAX - 34, size = 0, count = 0`. The header now checks
  `pos <= size` explicitly.
* `pageCount(...) * pageSize` was asserted representable. It is not, as above.

Both were found before either helper had a caller, which is the point of doing
this at the header rather than after the migration.

It also rejected the first LEB128 decoder for shifting a byte's payload past the
top of the accumulator. That is defined for an unsigned type — the bits are
simply dropped — but dropping them silently is how an over-long encoding decodes
as a plausible value. The header now masks the payload to the bits that still
fit, so the truncation is explicit and a lossy shift anywhere else is a real
finding rather than noise.

And it refuted two of the harnesses themselves: one asserted `a + b < a` to show
a sum wraps, and one built a symbolic 64-bit value with `v = (v << 8) | byte`.
Both commit the fault they were meant to be proving absent. A proof that starts
by discarding information is not a proof, and `--overflow-check` was right to
say so.

## Adding a proof

Add an `extern "C" void proof_<name>()` to a file under `tests/verification/`.
The driver discovers entry points from the source, so nothing else needs
updating — a proof cannot be forgotten by leaving it out of a list.

Inside a harness: take inputs from `nondet_size()` / `nondet_int64()`, constrain
them with `__ESBMC_assume`, and `assert` the property. Never form a value you are
claiming does not overflow — state it as the subtraction or division instead, or
the overflow check will fail your proof on its own arithmetic.
