#!/usr/bin/env python3
"""forge::security wired into the programs (docs/security-api.md "Integration"): `forge run`, `forge script`,
`forge rpc-serve` and `forge-serve` with --policy. No policy and a permissive policy give the same output; a strict
policy confines scripts (fs path-scoped, no net), requires signed models, and the servers lock down after binding
and still serve on loopback. CPU only; the tiny golden model.
  security_cli_test.py <forge> <forge-serve> <tiny.fpk>"""
import json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.request
forge, serve, fpk = sys.argv[1], sys.argv[2], sys.argv[3]
runner = os.environ.get("FORGE_RUNNER", "").split()
env = {k: v for k, v in os.environ.items() if not k.startswith("FORGE_SECURITY_")}
env["FORGE_DEVICE"] = "cpu"

top = tempfile.mkdtemp(prefix="forge_seccli_")
work = os.path.join(top, "work"); outside = os.path.join(top, "outside")
os.makedirs(work); os.makedirs(outside)

def run(*a, ok=True, cwd=work, exe=None, stdin=None):
    r = subprocess.run(runner + [exe or forge, *a], capture_output=True, text=True, errors="replace", cwd=cwd, env=env, input=stdin, timeout=120)
    if ok is not None and (r.returncode == 0) != ok:
        sys.exit(f"unexpected exit {r.returncode}: {a}\n--- stdout\n{r.stdout}--- stderr\n{r.stderr}")
    return r

def policy(name, doc):
    p = os.path.join(work, name)
    base = {"audit": {"os": False, "file": os.path.join(top, name + ".audit.jsonl")}, "allow_root": True}
    base.update(doc)
    with open(p, "w") as f: json.dump(base, f)
    os.chmod(p, 0o600)   # FIO-2: a strict policy must not be writable by group/other (umask 002 would make it 0664)
    return p

def audit(name):
    p = os.path.join(top, name + ".audit.jsonl")
    return [json.loads(l) for l in open(p)] if os.path.exists(p) else []

def script(pol, code, ok=True):
    return run("script", *(["--policy", pol] if pol else []), "-e", code, ok=ok)

shutil.copy(fpk, os.path.join(work, "m.fpk"))
run("keygen", "-o", os.path.join(top, "k"))
pub = open(os.path.join(top, "k.pub")).read().strip()
run("sign", "m.fpk", "--key", os.path.join(top, "k.key"), "-o", "s.fpk")

permissive = policy("permissive.json", {"profile": "permissive"})
strict = policy("strict.json", {"profile": "strict", "fs": {"read": ["."], "write": ["."]}, "integrity": {"trusted_keys": [pub]}})
strict_exec = policy("strict_exec.json", {"profile": "strict", "fs": {"read": ["."], "write": ["."]}, "exec": True,
                                          "integrity": {"models": "off", "trusted_keys": [pub]}})
scripts_req = policy("scripts_req.json", {"profile": "permissive", "integrity": {"scripts": "required"}})

# ---- no policy vs permissive: same output
gen = ["run", "m.fpk", "--prompt", "the", "-n", "6", "--ids"]
base = run(*gen).stdout
assert run(*gen, "--policy", permissive).stdout == base
code = 'fs.write("a.txt", "1"); std::println("{} {}", fs.read("a.txt"), 6 * 7);'
assert script(None, code).stdout == script(permissive, code).stdout == "1 42\n"
print("no policy == permissive: ok")

# ---- forge run: integrity.models required (strict)
assert run("run", "s.fpk", "--prompt", "the", "-n", "6", "--ids", "--policy", strict).stdout == base
r = run(*gen, "--policy", strict, ok=False)
assert "signature" in r.stderr, r.stderr
tampered = bytearray(open(os.path.join(work, "s.fpk"), "rb").read()); tampered[len(tampered) // 2] ^= 0x5A
open(os.path.join(work, "t.fpk"), "wb").write(tampered)
r = run("run", "t.fpk", "--prompt", "the", "-n", "2", "--ids", "--policy", strict, ok=False)
assert "hash mismatch" in r.stderr or "signature" in r.stderr, r.stderr
assert any(e["control"] == "INT-4" and e["decision"] == "fail" for e in audit("strict.json"))
print("forge run integrity: ok")

# ---- forge script under strict: fs path-scoped, no net / proc / env; lockdown applied
out_file = os.path.join(outside, "x.txt").replace("\\", "/")
r = script(strict, 'fs.write("in.txt", "x"); std::println("{}", fs.read("in.txt"));\n'
                   f'try {{ fs.write("{out_file}", "y"); std::println("wrote"); }} catch (const PermissionError& e) {{ std::println("denied"); }}\n')
assert r.stdout == "x\ndenied\n", r.stdout + r.stderr
# net / proc / env are denied outright, so the static checker refuses scripts that use them before they run.
for cap, use in (("net", 'net.port_open("127.0.0.1", 9, 100);'), ("proc", 'run("echo hi");'), ("env", 'os.env("PATH");')):
    rr = script(strict, use, ok=False)
    assert f"needs the '{cap}' capability" in rr.stderr, cap + ": " + rr.stderr
assert not os.path.exists(out_file)
ev = audit("strict.json")
assert any(e["control"] == "SBX-1" and e["decision"] == "allow" for e in ev), ev
assert any(e["category"] == "fs" and e["decision"] == "deny" and "x.txt" in e["target"] for e in ev), ev
r = script(scripts_req, 'std::println("ran");', ok=False)
assert "integrity.scripts" in r.stderr and "ran" not in r.stdout, r.stderr
print("forge script: ok")

# ---- rpc-serve: strict, locked down after bind, still serves loopback
def start(args, exe=None):
    p = subprocess.Popen(runner + [exe or forge, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=work, env=env)
    line = p.stdout.readline()
    if not line:
        p.wait(10); sys.exit(f"{args}: exited {p.returncode}: {p.stderr.read()}")
    return p, line

p, line = start(["rpc-serve", "--port", "0", "--policy", strict])
try:
    port = int(line.split("listening on 127.0.0.1:")[1].split()[0])
    socket.create_connection(("127.0.0.1", port), timeout=10).close()
    time.sleep(0.3); assert p.poll() is None
    assert any(e["control"] == "SBX-1" and "forge rpc" in e.get("detail", "") for e in audit("strict.json"))
finally:
    p.kill(); p.wait()
r = run("rpc-serve", "--port", "0", "--policy", strict, "--allow-scripts", "--token", "tok", ok=False)
assert "denies exec" in r.stderr, r.stderr
r = run("rpc-serve", "--port", "0", "--bind", "0.0.0.0", "--policy", strict, ok=False)
assert "net_listen" in r.stderr, r.stderr
# scripts allowed (exec granted): the child script runs under the same policy, confined to the work dir
p, line = start(["rpc-serve", "--port", "0", "--policy", strict_exec, "--allow-scripts", "--token", "tok"])
try:
    port = int(line.split("listening on 127.0.0.1:")[1].split()[0])
    remote = f'fs.write(\\"r.txt\\", \\"z\\"); std::println(\\"{{}}\\", fs.read(\\"r.txt\\")); fs.write(\\"{out_file}\\", \\"no\\");'
    r = script(None, f'auto r = forge::remote("127.0.0.1:{port}", "{remote}", {{{{"token", "tok"}}}});\n'
                     'std::println("{} {} {}", r["ok"], r["error_type"], r["output"]);')
    assert r.stdout.startswith("false PermissionError z"), r.stdout + r.stderr
    assert not os.path.exists(out_file)
finally:
    p.kill(); p.wait()
print("rpc-serve: ok")

# ---- forge-serve: strict with a signed model; refuses an unsigned one
def post(base, body):
    req = urllib.request.Request(base + "/v1/completions", data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=60).read())
want = [t for t in map(int, base.split()) if t != 291]
p, line = start(["s.fpk", "--port", "0", "--max-active", "2", "--policy", strict], exe=serve)
try:
    url = "http://127.0.0.1:" + line.rsplit(":", 1)[1].split()[0]
    assert json.loads(urllib.request.urlopen(url + "/health", timeout=30).read())["status"] == "ok"
    got = post(url, {"prompt": "the", "max_tokens": 6})["choices"][0]["tokens"]
    assert got == want[:len(got)] and got, (got, want)
    assert any(e["control"] == "SBX-1" and "forge-serve" in e.get("detail", "") for e in audit("strict.json"))
finally:
    p.kill(); p.wait()
r = run("m.fpk", "--port", "0", "--policy", strict, exe=serve, ok=False)
assert "signature" in r.stderr, r.stderr
r = run("s.fpk", "--port", "0", "--host", "0.0.0.0", "--policy", strict, exe=serve, ok=False)
assert "net_listen" in r.stderr, r.stderr
print("forge-serve: ok")
shutil.rmtree(top, ignore_errors=True)
print("security cli ok")
