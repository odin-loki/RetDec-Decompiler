# P1 — C ABI sketch (not shipped)

This is a **design sketch only**. No `libretdec.so` / `retdec.dll` export map,
version script, or public C header is added here. P2 (bindings) waits on a
real ABI. Names below are placeholders, not APIs in this tree.

## Existing C++ entry

In-process decompilation today is C++ `retdec::decompile` in
[`include/retdec/retdec/retdec.h`](../../include/retdec/retdec/retdec.h):

```cpp
bool decompile(
    retdec::config::Config& config,
    std::string* outString = nullptr
);
```

A future C ABI would call that (and the same `Config`) internally. It is not
a C API and must not be redeclared as one.

## Proposed opaque handles

| Handle (illustrative) | Role |
|-----------------------|------|
| session / context | Loaded binary and analysis state |
| function | One recovered function in that session |

Proposed operations (not implemented, no export map):

1. **Load binary** — open a path; return an opaque session or an error code.
2. **Decompile** — run analysis; implementation is `retdec::decompile`.
3. **Enumerate functions** — walk functions already recovered on the session.
4. **Get C text** — whole-unit or per-function text (`outString` today).
5. **Get detections** — read algorithm / semantic / crypto findings the C++
   pipeline already produced. Do not invent new detector entry points.

No `libretdec.so` symbol version script until this is actually implemented.

## Licence

Upstream RetDec v5.0 is Avast Software, MIT (`LICENSE-MIT`). Imortek
modifications follow the repository dual-licence (`LICENSE`). A future C ABI
would ship under those same terms and must retain the Avast MIT notice.
This sketch adds no royalty or commercial-fee language.
