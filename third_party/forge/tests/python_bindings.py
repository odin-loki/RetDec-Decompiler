#!/usr/bin/env python3
"""Python binding test: compares against the CLI (greedy ids) and checks forward/forward_all/rewind/grammar/draft."""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))
import numpy as np
fpk, cli, draft = sys.argv[1], sys.argv[2], sys.argv[3]
import forge
assert forge.abi_version() >= 2
m = forge.Model(fpk)
ids = m.tokenize("the weather")
assert ids[0] == 290 and m.detokenize(ids[1:]) == "the weather", (ids, m.detokenize(ids))
want = [int(x) for x in subprocess.run([cli, "run", fpk, "--prompt", "the weather", "-n", "12", "--ids"], capture_output=True, text=True, check=True).stdout.split()]
got = m.generate(ids, max_tokens=12)
assert got == want, (got, want)
m.reset(); a = m.forward_all(ids); assert a.shape == (len(ids), m.vocab_size)
m.reset(); l = m.forward(ids); assert np.abs(a[-1] - l).max() < 1e-4
m.rewind(2); assert m.position == 2
d = forge.Model(draft)
assert m.generate(ids, max_tokens=12, draft=d, spec_k=3) == want
seen = []
txt = m.detokenize(m.generate(ids, max_tokens=30, temperature=0.9, seed=4, grammar='root ::= "yes" | "no" | "maybe"', on_token=lambda t, s: seen.append(s) and False))
assert txt in ("yes", "no", "maybe"), txt
assert "".join(seen) == txt
stopped = m.generate(ids, max_tokens=12, on_token=lambda t, s: True)
assert len(stopped) == 1
try:
    forge.Model("/nonexistent.fpk"); raise SystemExit("expected error")
except forge.ForgeError as e:
    assert "nonexistent" in str(e)
assert forge.abi_version() >= 3
a = m.generate(ids, max_tokens=10, temperature=0.8, seed=5, typical_p=0.5)
assert a == m.generate(ids, max_tokens=10, temperature=0.8, seed=5, typical_p=0.5) and len(a) == 10
b = m.generate(ids, max_tokens=10, temperature=1.0, seed=5, mirostat_tau=3.0, mirostat_eta=0.2)
assert len(b) == 10
print("python bindings ok:", forge.version(), "abi", forge.abi_version(), "grammar ->", txt)
