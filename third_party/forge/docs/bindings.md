# Language bindings

## Concepts
The Python, Kotlin and Swift bindings wrap the C ABI ([capi.md](capi.md), `forge.h`). They share its model: one object = one model plus one KV sequence, not thread-safe.
1. Open a `.fpk`.
2. Tokenize text.
3. `generate` with sampling settings and an optional per-token callback that returns true to stop.
4. Close the model.

| Binding | Location | Native library | ABI used | Status |
|---|---|---|---|---|
| Python | `python/forge` (ctypes), package `forge-inference` | `libforge` (`forge_c`) | ≥ 2; 3 for typical / Mirostat | **Tested** by the `python_bindings` ctest (greedy ids against `forge run`, `forward`/`forward_all`/`rewind`, grammar, draft model, early stop) |
| Kotlin | `bindings/android`, package `dev.forge` | `libforge_jni` + `libforge` | 1–2 | The JNI library builds with `-DFORGE_JNI=ON`; ctest `jni_syntax` compiles the bridge when a JDK is found. **The Kotlin and Gradle code has not been compiled here** |
| Swift | `bindings/apple`, module `Forge` | `FORGE.xcframework` | 1–2 | **Not compiled** (no Apple SDK here; the source says "HARDWARE-UNTESTED") |

**Reset behaviour differs.** Python's `generate` resets the KV state first. Kotlin and Swift call `forge_generate` directly, which continues from the current position; call `reset()` before an unrelated prompt.

## API reference

### Python (`import forge`)
- `forge.version()`, `forge.abi_version()`, `forge.ForgeError` (a `RuntimeError`).
- `forge.Model(path, context=0, verify=False)`; a context manager, also closed on garbage collection.

| Member | Meaning |
|---|---|
| `close()`, `reset()`, `rewind(pos)` | |
| `position` (property), `vocab_size` (attribute) | |
| `tokenize(text, add_bos=True)` → `list[int]`, `detokenize(ids)` → `str` | |
| `forward(ids)`, `forward_all(ids)` | Last-row logits / `[len(ids), vocab]` logits as numpy arrays (lists without numpy). Continue from the current position |
| `generate(prompt, max_tokens=32, temperature=0.0, top_k=0, top_p=1.0, min_p=0.0, repetition_penalty=1.0, seed=0, grammar=None, draft=None, spec_k=4, on_token=None, typical_p=1.0, mirostat_tau=0.0, mirostat_eta=0.1)` | `prompt` is a string or token ids. Resets first; returns the new ids. `grammar` is GBNF text, `draft` another `Model`. `on_token(id, text)` returning true stops; an exception in it is re-raised after generation |
| `complete(text, **kw)` | Generated text |
| `stream(text, **kw)` | Yields text pieces **after** generation finishes; use `on_token` for live output |

Library lookup order: `$FORGE_LIB`, `libforge.so` next to the package, `../../build/dev/libforge.so` relative to the package, then `libforge.so` on the loader path. Only `.so` names are searched, so on Windows and macOS set `FORGE_LIB` to `forge.dll` / `libforge.dylib`. Importing raises `ForgeError` if the library's ABI is below 2.

### Kotlin (`dev.forge.ForgeModel`, `AutoCloseable`)
- `ForgeModel.open(path, context = 2048, verify = false)`; `ForgeModel.version`, `ForgeModel.abi`.
- `tokenize(text, addBos = true)`, `detokenize(ids)`, `forward(ids)`, `reset()`, `rewind(position)`; properties `vocabSize`, `position`.
- `generate(prompt: IntArray, maxNew, sampling = Sampling(), listener = null)` → `IntArray`; `generateText(prompt: String, maxNew, sampling, listener)` → `String`. `TokenListener.onToken(token, text)` returns true to stop.
- `Sampling(temperature = 0f, topK = 0, topP = 1f, minP = 0f, repetitionPenalty = 1f, seed = 0)`.
- Errors throw `IllegalStateException` with the `forge_last_error()` message.
- No grammar, draft model, `forwardAll` or ABI 3 samplers. For the GPU, set `FORGE_DEVICE=vulkan` (e.g. `Os.setenv`) before `open`.

### Swift (`import Forge`)
- `ForgeModel(path:context: = 2048, verify: = false, device: = "metal") throws`. `device` is written to `FORGE_DEVICE` (process-wide) before opening.
- `tokenize(_:addBOS:)`, `detokenize(_:)`, `forward(_:)`, `reset()`, `rewind(to:)`; `vocabSize`, `position`; static `version`, `abi`.
- `generate(_:maxNew:sampling:onToken:)` takes `[Int32]` (returns ids) or a `String` (returns text). `onToken` returns true to stop.
- `Sampling(temperature:topK:topP:minP:repetitionPenalty:seed:)`; errors are `ForgeError`.
- No grammar, draft model, `forwardAll` or ABI 3 samplers.

## Examples

Python (shape of the `python_bindings` test, on `golden/tiny_f32.fpk`):
```python
import forge
with forge.Model("model.fpk", context=256) as m:
    ids = m.generate("Hello", max_tokens=8)
    text = m.complete("Hi", max_tokens=4, temperature=0.7, top_k=40)
    yn = m.complete("Answer:", max_tokens=4, grammar='root ::= "yes" | "no"')
    m.generate("Hello", max_tokens=16, on_token=lambda t, s: print(s, end="", flush=True) and False)
```

Kotlin (**not compiled here**):
```kotlin
ForgeModel.open("/sdcard/model.fpk", context = 2048).use { m ->
    val text = m.generateText("Hello", maxNew = 32, sampling = ForgeModel.Sampling(temperature = 0.7f, topK = 40))
}
```

Swift (**not compiled here**):
```swift
let m = try ForgeModel(path: "model.fpk", context: 2048, device: "metal")
let out = try m.generate("Hello", maxNew: 32, sampling: Sampling(temperature: 0.7, topK: 40))
```

## Platform notes and limits
- **Python packaging:** `python/pyproject.toml` declares `forge-inference` 0.1.0 with `libforge.so` as package data and `numpy` as an optional extra. Copy the built library into `python/forge/` before building a wheel.
- **Android** (not run here): `cd bindings/android && gradle :example:assembleDebug`. The AAR builds `forge_jni` through the NDK (minSdk 26, arm64-v8a and x86_64). Device tests: `gradle :forge:connectedAndroidTest` with `FORGE_TEST_FPK` set; the same test runs on a host JVM (`:forge:test`) when `libforge_jni` is on `java.library.path`.
- **Android without Gradle:** `tests/android_emulator.sh apk <build-dir>` builds, installs and launches the example APK and checks the JNI library loaded.
- **Android hooks:** forward system callbacks to the C++ side (see [os.md](os.md) and [mobile.md](mobile.md)):
  - `ComponentCallbacks2.onTrimMemory` → `forge::os::memory_pressure::instance().on_trim_memory(level)`,
  - battery and thermal state → `power_hooks::report(...)`,
  - a foreground service → `background::set_hooks(...)`,
  - the Android Keystore → `keystore::set_backend(...)`.
- **Apple** (not run here): `tools/build_xcframework.sh` on macOS with Xcode builds `bindings/apple/FORGE.xcframework`; then `swift test` in `bindings/apple` with `FORGE_TEST_FPK` set. The package targets iOS 16.3 / macOS 13.3.
