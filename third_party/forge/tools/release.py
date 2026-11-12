#!/usr/bin/env python3
"""FORGE release driver (v1.0 tooling).

  tools/release.py version                 print the version from CMakeLists.txt
  tools/release.py bump 1.0.0              set project VERSION (CMakeLists.txt, Android/Swift metadata)
  tools/release.py changelog [--since TAG] prepend a section to CHANGELOG.md from git history
  tools/release.py build [--preset release] [--skip-tests]
                                           configure/build/test, cpack binary + source archives,
                                           SBOM, SHA256SUMS, optional detached Ed25519 signatures
  tools/release.py verify DIR              re-check SHA256SUMS (and signatures if a public key is given)
  tools/release.py tag                     create the annotated vX.Y.Z tag (never pushes)

Signing: --sign-key PEM (Ed25519 private key, OpenSSL format). Uses `openssl pkeyutl -rawin`; produces
<file>.sig next to each artifact. Reproducibility: SOURCE_DATE_EPOCH is set from the release commit.
"""
import argparse, hashlib, os, pathlib, re, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SEMVER = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$")

def sh(*cmd, cwd=ROOT, capture=False, env=None):
    print("+", " ".join(map(str, cmd)), flush=True)
    r = subprocess.run(list(map(str, cmd)), cwd=cwd, check=True, text=True, capture_output=capture, env=env)
    return r.stdout if capture else None

def version():
    m = re.search(r"project\(FORGE VERSION (\S+)", (ROOT / "CMakeLists.txt").read_text())
    if not m: sys.exit("release: no project VERSION in CMakeLists.txt")
    return m.group(1)

def bump(v):
    core = v.split("-")[0]
    if not SEMVER.match(v): sys.exit(f"release: '{v}' is not semver")
    edits = [(ROOT / "CMakeLists.txt", r"(project\(FORGE VERSION )\S+", rf"\g<1>{core}"),
             (ROOT / "bindings/android/gradle.properties", r"(?m)^(VERSION_NAME=).*$", rf"\g<1>{v}"),
             (ROOT / "inference/include/forge.h", r"(#define FORGE_VERSION_STRING \")[^\"]*", rf"\g<1>{v}")]
    for path, pat, rep in edits:
        if not path.exists(): continue
        s = path.read_text(); n = re.sub(pat, rep, s)
        if n != s: path.write_text(n); print(f"bumped {path.relative_to(ROOT)}")
    print(f"version {version()}")

def git(*a): return sh("git", *a, capture=True).strip()

def changelog(since=None):
    if since is None:
        try: since = git("describe", "--tags", "--abbrev=0")
        except subprocess.CalledProcessError: since = None
    rng = f"{since}..HEAD" if since else "HEAD"
    log = git("log", "--no-merges", "--format=%s (%h)", rng).splitlines()
    groups = {"Features": [], "Fixes": [], "Platforms": [], "Docs & tooling": [], "Other": []}
    for l in log:
        low = l.lower()
        k = ("Fixes" if re.search(r"\bfix|bug|crash", low) else
             "Platforms" if re.search(r"windows|macos|android|ios|mingw|wine|qemu|sve|rvv|neon", low) else
             "Docs & tooling" if re.search(r"docs?|status|readme|tool|ci\b|release", low) else
             "Features")
        groups[k].append(l)
    out = [f"## {version()} — {git('log', '-1', '--format=%cs')}\n"]
    for k, v in groups.items():
        if v: out += [f"\n### {k}\n"] + [f"- {x}\n" for x in v]
    p = ROOT / "CHANGELOG.md"
    old = p.read_text() if p.exists() else "# Changelog\n\n"
    head, _, rest = old.partition("\n\n")
    p.write_text(head + "\n\n" + "".join(out) + "\n" + rest)
    print(f"CHANGELOG.md: {len(log)} commit(s) since {since or 'the beginning'}")

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()

def sign(path, key):
    sh("openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key, "-in", path, "-out", f"{path}.sig")

def verify_sig(path, pub):
    r = subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin", "-inkey", pub, "-in", path, "-sigfile", f"{path}.sig"], capture_output=True, text=True)
    return r.returncode == 0

def build(preset, skip_tests, sign_key, out):
    env = dict(os.environ, SOURCE_DATE_EPOCH=git("log", "-1", "--format=%ct"))
    bdir = ROOT / "build" / preset
    sh("cmake", "--preset", preset, env=env)
    sh("cmake", "--build", "--preset", preset, "-j", str(min(4, os.cpu_count() or 1)), env=env)
    if not skip_tests: sh("ctest", "--test-dir", bdir, "-LE", "slow", "-j", str(os.cpu_count() or 1), "--output-on-failure", env=env)
    sh("cpack", "--config", bdir / "CPackConfig.cmake", "-B", bdir / "pkg", env=env)
    sh("cpack", "--config", bdir / "CPackSourceConfig.cmake", "-B", bdir / "pkg", env=env)
    out = pathlib.Path(out or ROOT / "dist" / version()); out.mkdir(parents=True, exist_ok=True)
    arts = []
    for p in sorted((bdir / "pkg").glob("forge-*")):
        if p.is_file() and p.suffix in (".gz", ".zip", ".deb"): shutil.copy2(p, out / p.name); arts.append(out / p.name)
    sh(sys.executable, ROOT / "tools/sbom.py", "--out", out / f"forge-{version()}.spdx.json", env=env)
    arts.append(out / f"forge-{version()}.spdx.json")
    (out / "SHA256SUMS").write_text("".join(f"{sha256(a)}  {a.name}\n" for a in arts))
    if sign_key:
        for a in arts + [out / "SHA256SUMS"]: sign(a, sign_key)
    print(f"release artifacts in {out}:"); [print("  ", a.name) for a in sorted(out.iterdir())]

def verify(d, pub):
    d = pathlib.Path(d); bad = 0
    for line in (d / "SHA256SUMS").read_text().splitlines():
        h, name = line.split("  ", 1); ok = sha256(d / name) == h; bad += not ok
        print(f"{'ok ' if ok else 'BAD'} sha256 {name}")
    if pub:
        for s in sorted(d.glob("*.sig")):
            ok = verify_sig(s.with_suffix(""), pub); bad += not ok
            print(f"{'ok ' if ok else 'BAD'} sig    {s.with_suffix('').name}")
    sys.exit(1 if bad else 0)

def tag():
    v = version(); dirty = git("status", "--porcelain")
    if dirty: sys.exit("release: working tree not clean")
    sh("git", "tag", "-a", f"v{v}", "-m", f"FORGE {v}")
    print(f"tagged v{v} (push with: git push origin v{v})")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("version"); b = sp.add_parser("bump"); b.add_argument("v")
    c = sp.add_parser("changelog"); c.add_argument("--since")
    bl = sp.add_parser("build"); bl.add_argument("--preset", default="release"); bl.add_argument("--skip-tests", action="store_true")
    bl.add_argument("--sign-key"); bl.add_argument("--out")
    v = sp.add_parser("verify"); v.add_argument("dir"); v.add_argument("--pub")
    sp.add_parser("tag")
    a = ap.parse_args()
    if a.cmd == "version": print(version())
    elif a.cmd == "bump": bump(a.v)
    elif a.cmd == "changelog": changelog(a.since)
    elif a.cmd == "build": build(a.preset, a.skip_tests, a.sign_key, a.out)
    elif a.cmd == "verify": verify(a.dir, a.pub)
    elif a.cmd == "tag": tag()

if __name__ == "__main__": main()
