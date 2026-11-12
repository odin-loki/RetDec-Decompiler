#!/usr/bin/env python3
"""Pack signing: forge keygen/sign/verify, cross-checked with an independent Ed25519 (pyca/cryptography),
tamper detection, multi-signer, and strict loading (FORGE_REQUIRE_SIGNED / FORGE_TRUSTED_KEYS)."""
import os, shutil, struct, subprocess, sys, tempfile, json
forge, fpk = sys.argv[1], sys.argv[2]
runner = os.environ.get("FORGE_RUNNER", "").split()
def run(*a, env=None, ok=True):
    r = subprocess.run(runner + [forge, *a], capture_output=True, text=True, errors="replace", env={**os.environ, **(env or {})})
    if ok is not None and (r.returncode == 0) != ok: sys.exit(f"unexpected exit {r.returncode}: {a}\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr
d = tempfile.mkdtemp()
src = os.path.join(d, "m.fpk"); shutil.copy(fpk, src)
run("keygen", "-o", os.path.join(d, "alice")); run("keygen", "-o", os.path.join(d, "bob"))
pub = {n: open(os.path.join(d, n + ".pub")).read().strip() for n in ("alice", "bob")}
signed = os.path.join(d, "s.fpk")
run("verify", src, ok=False)                                       # unsigned -> fails
run("sign", src, "--key", os.path.join(d, "alice.key"), "-o", signed)
print(run("verify", signed, "--pubkey", os.path.join(d, "alice.pub")).strip())
run("verify", signed, "--pubkey", os.path.join(d, "bob.pub"), ok=False)   # untrusted key

# independent check with pyca/cryptography: parse the container, recompute the digest, verify
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
import hashlib
def sections(p):
    b = open(p, "rb").read(); n, toff = struct.unpack_from("<IQ", b, 12)
    out = {}
    for i in range(n):
        r = b[toff + 96 * i: toff + 96 * (i + 1)]
        name = r[:32].split(b"\0")[0].decode(); off, size = struct.unpack_from("<QQ", r, 32)
        out[name] = b[off:off + size]
    return out
try:
    from blake3 import blake3 as B3
except ImportError:
    B3 = None
S = sections(signed); sig = json.loads(S["signature"])
assert sig["alg"] == "ed25519" and sig["scheme"] == "forge-fpk-v1"
if B3:
    h = B3(b"forge-fpk-v1")
    for name in sorted(S):
        if name == "signature": continue
        h.update(struct.pack("<Q", len(name)) + name.encode() + struct.pack("<Q", len(S[name])) + B3(S[name]).digest())
    assert h.hexdigest() == sig["digest"], "digest mismatch vs python blake3"
    print("[ok] digest recomputed independently (python blake3)")
for s in sig["signatures"]:
    Ed25519PublicKey.from_public_bytes(bytes.fromhex(s["public_key"])).verify(bytes.fromhex(s["sig"]), bytes.fromhex(sig["digest"]))
print("[ok] signature verified by pyca/cryptography Ed25519")

# multi-signer
run("sign", signed, "--key", os.path.join(d, "bob.key"))
assert len(json.loads(sections(signed)["signature"])["signatures"]) == 2
run("verify", signed, "--pubkey", os.path.join(d, "bob.pub")); run("verify", signed, "--pubkey", os.path.join(d, "alice.pub"))
print("[ok] two signers")

# tamper: flip one byte inside the data section (fix up nothing) -> hash + signature must fail
t = os.path.join(d, "t.fpk"); b = bytearray(open(signed, "rb").read())
n, toff = struct.unpack_from("<IQ", b, 12)
for i in range(n):
    r = b[toff + 96 * i: toff + 96 * (i + 1)]
    if r[:4] == b"data": off, size = struct.unpack_from("<QQ", r, 32); b[off + size // 2] ^= 1
open(t, "wb").write(b)
run("verify", t, "--pubkey", os.path.join(d, "alice.pub"), ok=False)
# tamper + consistent table hash (attacker rewrites the section hash) -> signature digest must fail
for i in range(n):
    r = b[toff + 96 * i: toff + 96 * (i + 1)]
    if r[:4] == b"data" and B3:
        off, size = struct.unpack_from("<QQ", r, 32); b[toff + 96 * i + 48: toff + 96 * i + 80] = B3(bytes(b[off:off + size])).digest()
open(t, "wb").write(b)
out = run("verify", t, "--pubkey", os.path.join(d, "alice.pub"), ok=False)
print("[ok] tampered files rejected:", out.strip().splitlines()[-1])

# strict loading
run("run", src, "--tokens", "1,2", "-n", "1", env={"FORGE_REQUIRE_SIGNED": "1"}, ok=False)
run("run", signed, "--tokens", "1,2", "-n", "1", env={"FORGE_REQUIRE_SIGNED": "1"})
run("run", signed, "--tokens", "1,2", "-n", "1", env={"FORGE_TRUSTED_KEYS": pub["alice"]})
run("run", t, "--tokens", "1,2", "-n", "1", env={"FORGE_TRUSTED_KEYS": pub["alice"]}, ok=False)
os.makedirs(os.path.join(d, "k"))
print("[ok] strict mode refuses unsigned / tampered files, accepts trusted signatures")
print("sign: PASS")
