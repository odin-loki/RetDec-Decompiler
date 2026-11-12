#!/usr/bin/env python3
"""Detached signatures from the command line (FSS INT-2, docs/cli.md "sign / verify"): `forge sign` / `forge verify` on
files that are not .fpk models, and `forge script FILE` / `forge rpc-serve` remote scripts under integrity.scripts.
No model, no third-party Python modules. The native-plugin cases are in tests/test_signing_detached.cpp.
  signing_cli_test.py <forge>"""
import json, os, shutil, subprocess, sys, tempfile
forge = os.path.abspath(sys.argv[1])
runner = os.environ.get("FORGE_RUNNER", "").split()
env = {k: v for k, v in os.environ.items() if not k.startswith("FORGE_SECURITY_")}
top = tempfile.mkdtemp(prefix="forge_signcli_")

def run(*a, ok=True, stdin=None):
    r = subprocess.run(runner + [forge, *a], capture_output=True, text=True, errors="replace", cwd=top, env=env, input=stdin, timeout=120)
    if ok is not None and (r.returncode == 0) != ok:
        sys.exit(f"unexpected exit {r.returncode}: {a}\n--- stdout\n{r.stdout}--- stderr\n{r.stderr}")
    return r

def write(name, text):
    with open(os.path.join(top, name), "w", newline="\n") as f: f.write(text)

run("keygen", "-o", "k"); run("keygen", "-o", "k2")
pub = open(os.path.join(top, "k.pub")).read().strip()

# ---- sign / verify any file
write("hello.fs", 'std::println("hello");\n')
r = run("sign", "hello.fs", "--key", "k.key")
assert "hello.fs.sig" in r.stdout, r.stdout
sig = json.load(open(os.path.join(top, "hello.fs.sig")))
assert sig["format"] == "forge-detached-sig" and sig["file"] == "hello.fs" and len(sig["signatures"]) == 1, sig
assert "VERIFIED" in run("verify", "hello.fs", "--pubkey", "k.pub").stdout
assert "VERIFIED" in run("verify", "hello.fs").stdout                       # no --pubkey: any valid signature
r = run("verify", "hello.fs", "--pubkey", "k2.pub", ok=False)                  # wrong key
assert "not by a trusted key" in r.stdout, r.stdout
run("sign", "hello.fs", "--key", "k2.key")                                     # second signer extends the .sig
assert len(json.load(open(os.path.join(top, "hello.fs.sig")))["signatures"]) == 2
assert "VERIFIED" in run("verify", "hello.fs", "--pubkey", "k2.pub").stdout
shutil.copy(os.path.join(top, "hello.fs"), os.path.join(top, "other.fs"))
r = run("verify", "other.fs", "--sig", "hello.fs.sig", ok=False)              # signature for another file name
assert "not 'other.fs'" in r.stdout, r.stdout
r = run("verify", "other.fs", ok=False)                                         # no .sig
assert "unsigned" in r.stdout, r.stdout
write("hello.fs", 'std::println("hellO");\n')                                  # tampered
r = run("verify", "hello.fs", ok=False)
assert "modified after signing" in r.stdout, r.stdout
print("sign / verify: ok")

# ---- forge script FILE under integrity.scripts
def policy(name, doc):
    p = os.path.join(top, name)
    base = {"profile": "permissive", "audit": {"os": False, "file": os.path.join(top, name + ".audit.jsonl")}, "allow_root": True}
    base.update(doc)
    with open(p, "w") as f: json.dump(base, f)
    return p
req = policy("req.json", {"integrity": {"scripts": "required", "trusted_keys": [pub]}})
opt = policy("opt.json", {"integrity": {"scripts": "optional", "trusted_keys": [pub]}})
write("main.fs", 'import "util" as u;\nstd::println("{}", u.twice(21));\n')
write("util.fs", "int twice(int x) { return 2 * x; }\n")
r = run("script", "--policy", req, "main.fs", ok=False)
assert "IntegrityError" in r.stderr and "42" not in r.stdout, r.stderr
run("sign", "main.fs", "--key", "k.key")
r = run("script", "--policy", req, "main.fs", ok=False)                       # the import is unsigned
assert "IntegrityError" in r.stderr and "util.fs" in r.stderr, r.stderr
run("sign", "util.fs", "--key", "k.key")
assert run("script", "--policy", req, "main.fs").stdout == "42\n"
r = run("script", "--policy", req, "-e", 'std::println("x");', ok=False)       # -e cannot carry a signature
assert "integrity.scripts" in r.stderr, r.stderr
os.remove(os.path.join(top, "util.fs.sig"))
assert run("script", "--policy", opt, "main.fs").stdout == "42\n"             # optional: unsigned import audited
ev = [json.loads(l) for l in open(os.path.join(top, "opt.json.audit.jsonl"))]
assert any(e["control"] == "INT-1" and e["decision"] == "weaken" and "util.fs" in e["target"] for e in ev), ev
assert any(e["control"] == "INT-2" and e["decision"] == "allow" and "main.fs" in e["target"] for e in ev), ev
print("forge script: ok")

# ---- rpc-serve: remote scripts carry their signature (FRPZ), the child verifies it
srv_pol = policy("srv.json", {"exec": True, "integrity": {"scripts": "required", "models": "off", "trusted_keys": [pub]}})
write("job.fs", 'std::println("remote {}", 6 * 7);\n')
run("sign", "job.fs", "--key", "k.key")
job = open(os.path.join(top, "job.fs")).read()
job_sig = open(os.path.join(top, "job.fs.sig")).read()
p = subprocess.Popen(runner + [forge, "rpc-serve", "--port", "0", "--policy", srv_pol, "--allow-scripts", "--token", "tok"],
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=top, env=env)
try:
    line = p.stdout.readline()
    if not line: p.wait(10); sys.exit("rpc-serve exited: " + p.stderr.read())
    port = int(line.split("listening on 127.0.0.1:")[1].split()[0])
    def remote(opts):
        code = (f'auto src = fs.read("job.fs"); auto sig = fs.read("job.fs.sig");\n'
                f'auto r = forge::remote("127.0.0.1:{port}", src, {{{{"token", "tok"}}{opts}}});\n'
                'std::println("{} {} {}", r["ok"], r["error_type"], r["output"]);')
        return run("script", "-e", code).stdout
    got = remote(', {"name", "job.fs"}, {"signature", sig}')
    assert got.startswith("true  remote 42"), got
    got = remote('')                                                            # unsigned
    assert got.startswith("false IntegrityError"), got
    got = remote(', {"name", "other.fs"}, {"signature", sig}')                  # signed for another name
    assert got.startswith("false IntegrityError"), got
    write("job2.fs", job.replace("6 * 7", "6 * 8"))
    got = run("script", "-e", f'auto r = forge::remote("127.0.0.1:{port}", fs.read("job2.fs"), {{{{"token", "tok"}}, {{"name", "job.fs"}}, '
                              f'{{"signature", fs.read("job.fs.sig")}}}}); std::println("{{}} {{}}", r["ok"], r["error_type"]);').stdout
    assert got.startswith("false IntegrityError"), got                          # tampered source
finally:
    p.kill(); p.wait()
print("rpc-serve signed scripts: ok")
shutil.rmtree(top, ignore_errors=True)
print("signing cli ok")
