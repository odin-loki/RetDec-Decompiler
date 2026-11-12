"""FORGE Python bindings (ctypes over the stable C ABI in libforge.so).

    import forge
    m = forge.Model("model.fpk")
    print(m.complete("Hello", max_tokens=16))
    for piece in m.stream("Once upon", max_tokens=32, temperature=0.8): print(piece, end="")
    logits = m.forward(m.tokenize("abc"))            # numpy array [vocab] (if numpy is installed)

The library is found via $FORGE_LIB, next to this package, or on the loader path.
"""
import ctypes, os
from ctypes import c_char_p, c_float, c_int, c_int32, c_int64, c_uint32, c_uint64, c_void_p, POINTER

__all__ = ["Model", "ForgeError", "abi_version", "version"]

class ForgeError(RuntimeError):
    pass

def _load():
    cands = []
    if os.environ.get("FORGE_LIB"): cands.append(os.environ["FORGE_LIB"])
    here = os.path.dirname(os.path.abspath(__file__))
    cands += [os.path.join(here, "libforge.so"), os.path.join(here, "..", "..", "build", "dev", "libforge.so"), "libforge.so"]
    err = None
    for c in cands:
        try: return ctypes.CDLL(c)
        except OSError as e: err = e
    raise ForgeError(f"cannot load libforge.so (set FORGE_LIB): {err}")

_lib = _load()

class _Sampling(ctypes.Structure):
    _fields_ = [("temperature", c_float), ("top_k", c_int32), ("top_p", c_float), ("min_p", c_float),
                ("repetition_penalty", c_float), ("seed", c_uint64)]

class _GenOpts(ctypes.Structure):
    _fields_ = [("grammar", c_char_p), ("draft", c_void_p), ("spec_k", c_int32)]

_CB = ctypes.CFUNCTYPE(c_int, c_int32, c_char_p, c_void_p)

def _sig(name, res, *args):
    f = getattr(_lib, name); f.restype = res; f.argtypes = list(args); return f

_abi = _sig("forge_abi_version", c_uint32)
_ver = _sig("forge_version", c_char_p)
_err = _sig("forge_last_error", c_char_p)
_open = _sig("forge_open", c_void_p, c_char_p, c_int64, c_int)
_close = _sig("forge_close", None, c_void_p)
_reset = _sig("forge_reset", None, c_void_p)
_vocab = _sig("forge_vocab_size", c_int64, c_void_p)
_tok = _sig("forge_tokenize", c_int64, c_void_p, c_char_p, c_int, POINTER(c_int32), c_int64)
_detok = _sig("forge_detokenize", c_int64, c_void_p, POINTER(c_int32), c_int64, c_char_p, c_int64)
_fwd = _sig("forge_forward", c_int, c_void_p, POINTER(c_int32), c_int64, POINTER(c_float), c_int64)
_pos = _sig("forge_position", c_int64, c_void_p)
_rew = _sig("forge_rewind", c_int, c_void_p, c_int64)
_fwd_all = _sig("forge_forward_all", c_int, c_void_p, POINTER(c_int32), c_int64, POINTER(c_float), c_int64)
_gen = _sig("forge_generate_ex", c_int64, c_void_p, POINTER(c_int32), c_int64, c_int32, POINTER(_Sampling), POINTER(_GenOpts),
            POINTER(c_int32), c_int64, _CB, c_void_p)

class _SamplingExt(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("typical_p", c_float), ("mirostat_tau", c_float), ("mirostat_eta", c_float)]

_gen2 = None
if _abi() >= 3:
    _gen2 = _sig("forge_generate_ex2", c_int64, c_void_p, POINTER(c_int32), c_int64, c_int32, POINTER(_Sampling), POINTER(_GenOpts),
                 POINTER(_SamplingExt), POINTER(c_int32), c_int64, _CB, c_void_p)

if _abi() < 2:
    raise ForgeError(f"libforge ABI {_abi()} is too old (need >= 2)")

def abi_version(): return int(_abi())
def version(): return _ver().decode()

def _check(ok):
    if not ok: raise ForgeError(_err().decode(errors="replace"))

def _ids(seq):
    seq = list(seq); return (c_int32 * max(len(seq), 1))(*seq), len(seq)

def _floats(buf):
    try:
        import numpy as np
        return np.ctypeslib.as_array(buf).copy()
    except ImportError:
        return list(buf)

class Model:
    def __init__(self, path, context=0, verify=False):
        self._h = _open(os.fsencode(path), context, int(verify))
        _check(self._h)
        self.vocab_size = int(_vocab(self._h))

    def close(self):
        if getattr(self, "_h", None): _close(self._h); self._h = None
    __del__ = close
    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    @property
    def position(self): return int(_pos(self._h))
    def reset(self): _reset(self._h)
    def rewind(self, position): _check(_rew(self._h, position) == 0)

    def tokenize(self, text, add_bos=None):   # None: the model's default (no BOS for Qwen-family models)
        b = text.encode(); ab = -1 if add_bos is None else int(bool(add_bos))
        n = _tok(self._h, b, ab, None, 0); _check(n >= 0)
        out = (c_int32 * max(n, 1))(); _tok(self._h, b, ab, out, n)
        return list(out[:n])

    def detokenize(self, ids):
        arr, n = _ids(ids)
        need = _detok(self._h, arr, n, None, 0); _check(need >= 0)
        buf = ctypes.create_string_buffer(need + 1); _detok(self._h, arr, n, buf, need + 1)
        return buf.raw[:need].decode(errors="replace")

    def forward(self, ids):
        """Continue from the current position; returns last-position logits."""
        arr, n = _ids(ids); out = (c_float * self.vocab_size)()
        _check(_fwd(self._h, arr, n, out, self.vocab_size) == 0)
        return _floats(out)

    def forward_all(self, ids):
        """Logits for every position, shape [len(ids), vocab]."""
        arr, n = _ids(ids); out = (c_float * (n * self.vocab_size))()
        _check(_fwd_all(self._h, arr, n, out, n * self.vocab_size) == 0)
        r = _floats(out)
        return r.reshape(n, self.vocab_size) if hasattr(r, "reshape") else [r[i * self.vocab_size:(i + 1) * self.vocab_size] for i in range(n)]

    def generate(self, prompt, max_tokens=32, temperature=0.0, top_k=0, top_p=1.0, min_p=0.0, repetition_penalty=1.0,
                 seed=0, grammar=None, draft=None, spec_k=4, on_token=None, typical_p=1.0, mirostat_tau=0.0, mirostat_eta=0.1):
        """Generate from token ids (resets state first). on_token(id, text) -> True to stop early. Returns new ids."""
        if isinstance(prompt, str): prompt = self.tokenize(prompt)
        self.reset()
        s = _Sampling(temperature, top_k, top_p, min_p, repetition_penalty, seed)
        o = _GenOpts(grammar.encode() if grammar else None, draft._h if draft else None, spec_k)
        arr, n = _ids(prompt); out = (c_int32 * max(max_tokens, 1))()
        err = []
        def cb(t, txt, _u):
            if on_token is None: return 0
            try: return 1 if on_token(int(t), txt.decode(errors="replace") if txt else "") else 0
            except BaseException as e: err.append(e); return 1
        if typical_p != 1.0 or mirostat_tau:
            if _gen2 is None: raise ForgeError("typical_p / mirostat need libforge ABI >= 3")
            x = _SamplingExt(ctypes.sizeof(_SamplingExt), typical_p, mirostat_tau, mirostat_eta)
            k = _gen2(self._h, arr, n, max_tokens, ctypes.byref(s), ctypes.byref(o), ctypes.byref(x), out, max_tokens, _CB(cb), None)
        else:
            k = _gen(self._h, arr, n, max_tokens, ctypes.byref(s), ctypes.byref(o), out, max_tokens, _CB(cb), None)
        if err: raise err[0]
        _check(k >= 0)
        return list(out[:k])

    def complete(self, text, **kw):
        return self.detokenize(self.generate(self.tokenize(text), **kw))

    def stream(self, text, **kw):
        """Yields text pieces (runs generation fully, then yields; use on_token for true streaming)."""
        pieces = []
        self.generate(self.tokenize(text), on_token=lambda t, s: pieces.append(s) and False, **kw)
        yield from pieces
