#!/usr/bin/env python3
"""E9 / CI-03 — advertised tokens in public docs must resolve in the tree.

Forward (legacy): hardcoded TOKENS must exist in the named source trees.
Invert (CI-03): every `retdec-*` name, `--flag`, and `RETDEC_*` variable in
public docs must resolve to a CMake target, a CLI/script flag, or a getenv
site (or an explicit documented-absent / tooling allowlist).

Usage:
    python3 scripts/ci/check_doc_vs_code.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# token -> required path (file or directory). None = src/ or include/.
TOKENS: tuple[tuple[str, str | None], ...] = (
    ("RETDEC_NEURAL_REFINE", "src/neural"),
    ("RETDEC_EMIT_BUILDABLE", "src/retdec"),
    ("--buildable", "src/retdec-decompiler"),
    ("--no-buildable", "src/retdec-decompiler"),
    ("RETDEC_SKIP_SEMANTIC_RECOVERY", "src/retdec/retdec.cpp"),
    ("maybeRefineDecompilerOutput", None),
)

SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
)
TEXT_SUFFIXES = SOURCE_SUFFIXES | {
    ".py",
    ".sh",
    ".cmake",
    ".yml",
    ".yaml",
    ".md",
    ".txt",
    ".in",
}

BACKTICK_RE = re.compile(r"`([^`]+)`")
EXAMPLE_NAME_RE = re.compile(r"mystage", re.IGNORECASE)
RETDEC_NAME_RE = re.compile(r"^retdec-[a-z0-9]+(?:-[a-z0-9]+)*$")
RETDEC_ENV_RE = re.compile(r"^RETDEC_[A-Z0-9_]+$")
FLAG_RE = re.compile(r"^--[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")

CMAKE_TARGET_RE = re.compile(
    r"add_(?:library|executable|custom_target)\(\s*([A-Za-z0-9_:-]+)",
    re.MULTILINE,
)
CMAKE_ALIAS_RE = re.compile(
    r"add_library\(\s*([A-Za-z0-9_:-]+)\s+ALIAS\s+",
    re.MULTILINE,
)

# Names that public docs correctly say do not ship.
DOCUMENTED_ABSENT = frozenset(
    {
        "retdec-qwen3-runner",
        "retdec-qwen3",
    }
)

# Non-product flags that appear in public docs (cmake, git, docker, gcc, scripts).
TOOL_FLAGS = frozenset(
    {
        "--build",
        "--target",
        "--preset",
        "--install",
        "--parallel",
        "--config",
        "--test-dir",
        "--output-on-failure",
        "--build-type",
        "--fresh",
        "--help",
        "--version",
        "--rm",
        "--cross-compile-prefix",
        "--pull",
        "--no-cache",
        "--file",
        "--tag",
        "--progress",
        "--no-verify",
        "--amend",
        "--hard",
        "--soft",
        "--cached",
        "--stat",
        "--oneline",
        "--all",
        "--force",
        "--recursive",
        "--depth",
        "--branch",
        "--origin",
        "--upstream",
        "--rebase",
        "--ff-only",
        "--squash",
        "--no-ff",
        "--message",
        "--author",
        "--date",
        "--format",
        "--pretty",
        "--name-only",
        "--name-status",
        "--diff-filter",
        "--check",
        "--quiet",
        "--verbose",
        "--dry-run",
        "--yes",
        "--no",
        "--color",
        "--no-color",
        "--user",
        "--workdir",
        "--volume",
        "--env",
        "--name",
        "--network",
        "--publish",
        "--expose",
        "--entrypoint",
        "--platform",
        "--prefix",
        "--prefix-path",
        "--toolset",
        "--generator",
        "--warn-uninitialized",
        "--debug-output",
        "--trace",
        "--trace-expand",
        "--graphviz",
        "--system-information",
        "--list-presets",
        "--workflow",
        "--self-test",
        "--base-url",
        "--decompiler",
        "--corpus",
        "--manifest",
        "--ci-core",
        "--no-stem-fallback",
        "--stem-fallback",
        "--work",
        "--out",
        "--predictions",
        "--ground-truth",
        "--results",
        "--min-decompiled",
        "--min-mean-f1",
        "--min-mean-f1-raw",
        "--current",
        "--profile",
        "--limit",
        "--timeout",
        "--jobs",
        "--silent",
        "--mode",
        "--arch",
        "--endian",
        "--bit-size",
        "--output",
        "--output-format",
        "--output-lang",
        "--config",
        "--pdb",
        "--cleanup",
        "--select-functions",
        "--select-ranges",
        "--select-decode-only",
        "--keep-unreachable-funcs",
        "--raw-section-vma",
        "--raw-entry-point",
        "--max-memory",
        "--no-memory-limit",
        "--llvm-passes-json",
        "--disable-static-code-detection",
        "--backend-disabled-opts",
        "--backend-enabled-opts",
        "--backend-call-info-obtainer",
        "--backend-var-renamer",
        "--backend-no-opts",
        "--backend-emit-cfg",
        "--backend-emit-cg",
        "--backend-keep-all-brackets",
        "--backend-keep-library-funcs",
        "--backend-no-time-varying-info",
        "--backend-no-var-renaming",
        "--backend-no-compound-operators",
        "--backend-no-symbolic-names",
        "--ar-index",
        "--ar-name",
        "--static-code-sigfile",
        "--try-emulation",
        "--print-after-all",
        "--print-before-all",
        "--no-buildable",
        "--buildable",
        "--model",  # documented as absent CLI flag
        "--no-pie",
        "--coverage",
        "--std",
        "--sysroot",
        "--target-os",
        "--host",
        "--enable",
        "--disable",
        "--with",
        "--without",
        "--prefix",
        "--libdir",
        "--includedir",
        "--datadir",
        "--mandir",
        "--infodir",
        "--localstatedir",
        "--sysconfdir",
        "--sharedstatedir",
        "--oldincludedir",
        "--exec-prefix",
        "--program-prefix",
        "--program-suffix",
        "--program-transform-name",
        "--enable-shared",
        "--enable-static",
        "--disable-nls",
        "--static",
        "--shared",
        "--pic",
        "--no-pie",
        "--dumpmachine",
        "--print-file-name",
        "--print-search-dirs",
        "--save-temps",
        "--param",
        "--specs",
        "--sysroot",
        "--no-undefined",
        "--as-needed",
        "--no-as-needed",
        "--start-group",
        "--end-group",
        "--whole-archive",
        "--no-whole-archive",
        "--export-dynamic",
        "--gc-sections",
        "--strip-all",
        "--strip-debug",
        "--no-insert-timestamp",
        "--major-image-version",
        "--minor-image-version",
        "--subsystem",
        "--entry",
        "--out-implib",
        "--output-def",
        "--kill-at",
        "--add-stdcall-alias",
        "--enable-auto-import",
        "--disable-auto-import",
        "--dynamicbase",
        "--nxcompat",
        "--high-entropy-va",
        "--tsaware",
        "--wdmdriver",
        "--no-seh",
        "--large-address-aware",
        "--fixed",
        "--release",
        "--debug",
        "--pdb",
        "--incremental",
        "--ltcg",
        "--opt",
        "--machine",
        "--libpath",
        "--defaultlib",
        "--ignore",
        "--manifest",
        "--nologo",
        "--nowarn",
        "--warn",
        "--error",
        "--fatal-warnings",
        "--no-fatal-warnings",
        "--unresolved-symbols",
        "--allow-shlib-undefined",
        "--no-allow-shlib-undefined",
        "--rpath",
        "--rpath-link",
        "--soname",
        "--version-script",
        "--dynamic-list",
        "--export-dynamic-symbol",
        "--exclude-libs",
        "--no-undefined-version",
        "--default-symver",
        "--no-default-symver",
        "--hash-style",
        "--build-id",
        "--compress-debug-sections",
        "--eh-frame-hdr",
        "--no-eh-frame-hdr",
        "--ld-generated-unwind-info",
        "--no-ld-generated-unwind-info",
        "--nostdlib",
        "--nodefaultlibs",
        "--nolibc",
        "--nostartfiles",
        "--pie",
        "--no-pie",
        "--static-pie",
        "--no-dynamic-linker",
        "--dynamic-linker",
        "--sysroot",
        "--Bsymbolic",
        "--Bsymbolic-functions",
        "--wrap",
        "--defsym",
        "--undefined",
        "--require-defined",
        "--orphan-handling",
        "--unique",
        "--sort-common",
        "--sort-section",
        "--section-start",
        "--image-base",
        "--section-alignment",
        "--file-alignment",
        "--stack",
        "--heap",
        "--dll",
        "--out-implib",
        "--base-file",
        "--output-def",
        "--add-stdcall-alias",
        "--enable-stdcall-fixup",
        "--disable-stdcall-fixup",
        "--warn-duplicate-exports",
        "--compat-implib",
        "--no-leading-underscore",
        "--leading-underscore",
        "--no-insert-timestamp",
        "--enable-long-section-names",
        "--disable-long-section-names",
        "--major-os-version",
        "--minor-os-version",
        "--major-subsystem-version",
        "--minor-subsystem-version",
        "--subsystem",
        "--timestamp",
        "--dll-search-prefix",
        "--enable-auto-image-base",
        "--disable-auto-image-base",
        "--dll-search-prefix",
        "--exclude-all-symbols",
        "--exclude-symbols",
        "--exclude-modules-for-implib",
        "--file-alignment",
        "--heap",
        "--image-base",
        "--major-image-version",
        "--minor-image-version",
        "--major-os-version",
        "--minor-os-version",
        "--major-subsystem-version",
        "--minor-subsystem-version",
        "--out-implib",
        "--output-def",
        "--section-alignment",
        "--stack",
        "--subsystem",
        "--support-old-code",
        "--timestamp",
        "--warn-duplicate-exports",
        "--warn-multiple-gp",
        "--large-address-aware",
        "--disable-reloc-section",
        "--enable-reloc-section",
        "--dynamicbase",
        "--forceinteg",
        "--nxcompat",
        "--no-isolation",
        "--no-seh",
        "--no-bind",
        "--wdmdriver",
        "--tsaware",
        "--high-entropy-va",
        "--insert-timestamp",
        "--no-insert-timestamp",
        "--appcontainer",
        "--no-appcontainer",
        "--guard-cf",
        "--no-guard-cf",
        "--integrity-flags",
        "--no-integrity-flags",
        "--swap",
        "--swapfile",
        "--kill-after",
        "--preserve-status",
        "--foreground",
        "--signal",
        "--verbose",
        "--quiet",
        "--help",
        "--version",
        "--binary",
        "--decompiler",
    }
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def public_doc_files() -> list[Path]:
    files: list[Path] = []
    seen: set[Path] = set()

    def add(path: Path) -> None:
        if not path.is_file():
            return
        key = path.resolve()
        if key in seen:
            return
        seen.add(key)
        files.append(path)

    add(REPO_ROOT / "README.md")
    docs = REPO_ROOT / "docs"
    if docs.is_dir():
        for path in sorted(docs.iterdir()):
            if path.is_file() and path.suffix.lower() == ".md":
                add(path)
    for name in ("WHITEPAPER.md", "WHITEPAPER"):
        add(REPO_ROOT / name)
    # CI-03 leftover: REL-07 / legal / packaging surfaces outside docs/.
    for rel in (
        "QUICKSTART.md",
        "SECURITY.md",
        "CLA.md",
        "CONTRIBUTING.md",
        "LICENSING_FAQ.md",
        "CODE_OF_CONDUCT.md",
        "releases/README.md",
        "scripts/README.md",
        "ROADMAP.md",
    ):
        add(REPO_ROOT / rel)
    return files


def iter_code_files(*roots: Path) -> list[Path]:
    out: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        if root.is_file():
            out.append(root)
            continue
        try:
            children = root.rglob("*")
        except OSError:
            continue
        for path in children:
            if not path.is_file():
                continue
            if any(p in {"deps", "build", ".git"} for p in path.parts):
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES and path.name != "CMakeLists.txt":
                continue
            out.append(path)
    return out


def symbol_present(token: str, rel: str | None) -> bool:
    if rel is None:
        roots = (REPO_ROOT / "src", REPO_ROOT / "include")
    else:
        roots = (REPO_ROOT / rel,)

    for root in roots:
        if root.is_file():
            return token in read_text(root)
        if not root.is_dir():
            continue
        try:
            children = root.rglob("*")
        except OSError:
            continue
        for path in children:
            if not path.is_file():
                continue
            if path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if token in read_text(path):
                return True
    return False


def docs_mentioning(token: str, docs: list[Path]) -> list[str]:
    hits = []
    for path in docs:
        if token in read_text(path):
            hits.append(path.relative_to(REPO_ROOT).as_posix())
    return hits


def cmake_targets() -> set[str]:
    names: set[str] = set()
    for path in iter_code_files(REPO_ROOT / "src", REPO_ROOT / "tests", REPO_ROOT / "cmake"):
        if path.name != "CMakeLists.txt" and path.suffix.lower() != ".cmake":
            continue
        text = read_text(path)
        for m in CMAKE_TARGET_RE.finditer(text):
            names.add(m.group(1))
        for m in CMAKE_ALIAS_RE.finditer(text):
            names.add(m.group(1))
    names.add("retdec-decompiler")
    names.add("retdec-gui")
    return names


def blob_has(token: str, files: list[Path]) -> bool:
    for path in files:
        if token in read_text(path):
            return True
    return False


def invert_errors(docs: list[Path]) -> list[str]:
    errors: list[str] = []
    targets = cmake_targets()
    target_norm = {t.replace("_", "-") for t in targets} | targets
    code_files = iter_code_files(
        REPO_ROOT / "src",
        REPO_ROOT / "include",
        REPO_ROOT / "scripts",
        REPO_ROOT / "cmake",
        REPO_ROOT / "tests",
        REPO_ROOT / "releases",
    )
    seen_names: set[str] = set()
    seen_envs: set[str] = set()
    seen_flags: set[str] = set()

    for doc in docs:
        text = read_text(doc)
        rel = doc.relative_to(REPO_ROOT).as_posix()
        for span in BACKTICK_RE.findall(text):
            token = span.strip()
            if not token or EXAMPLE_NAME_RE.search(token):
                continue
            if "=" in token:
                token = token.split("=", 1)[0]
            if RETDEC_NAME_RE.fullmatch(token):
                if token in seen_names:
                    continue
                seen_names.add(token)
                if token in DOCUMENTED_ABSENT:
                    continue
                if token in target_norm or token in targets:
                    continue
                underscored = token.replace("-", "_")
                if underscored in targets:
                    continue
                if blob_has(token, code_files):
                    continue
                errors.append(
                    f"{rel}: `{token}` is documented but is not a CMake target under src/tests/cmake"
                )
                continue
            if RETDEC_ENV_RE.fullmatch(token):
                if token in seen_envs:
                    continue
                seen_envs.add(token)
                if blob_has(token, code_files):
                    continue
                errors.append(
                    f"{rel}: `{token}` is documented but has no getenv/CMake/script site"
                )
                continue
            if FLAG_RE.fullmatch(token):
                if token in seen_flags:
                    continue
                seen_flags.add(token)
                if token in TOOL_FLAGS:
                    continue
                if blob_has(token, code_files):
                    continue
                errors.append(
                    f"{rel}: `{token}` is documented but is not a CLI option, script flag, or tooling allowlist entry"
                )
    return errors


def main() -> int:
    docs = public_doc_files()
    errors: list[str] = []

    print("check_doc_vs_code: public docs scanned:")
    for path in docs:
        print(f"  {path.relative_to(REPO_ROOT).as_posix()}")

    for token, rel in TOKENS:
        where = rel or "src/ or include/"
        present = symbol_present(token, rel)
        advertised = docs_mentioning(token, docs)
        in_readme = "README.md" in advertised
        status = "present" if present else "MISSING"
        adv = f"; advertised in {', '.join(advertised)}" if advertised else ""
        print(f"  {token} -> {where}: {status}{adv}")

        if present:
            continue
        if in_readme:
            errors.append(
                f"{token} is advertised in README.md but the symbol is missing in {where}"
            )
        else:
            errors.append(f"{token} has no matching symbol in {where}")

    invert = invert_errors(docs)
    if invert:
        print("check_doc_vs_code: invert (CI-03) findings:")
        for line in invert:
            print(f"  {line}")
        errors.extend(invert)

    if errors:
        print("check_doc_vs_code: FAIL")
        for line in errors:
            print(f"  {line}")
        return 1

    print("check_doc_vs_code: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
