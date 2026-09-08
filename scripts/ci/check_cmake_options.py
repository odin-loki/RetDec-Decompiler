#!/usr/bin/env python3
"""A build option nothing reads is a promise the build does not keep.

cmake/options.cmake declares every RETDEC_* switch. An option() call with no
reader anywhere in the tree still shows up in ccmake, in cmake -LH and in the
docs, and setting it changes nothing -- which is worse than the option not
existing, because the help text says what it would do. RETDEC_ENABLE_RELLIC was
one: "Build rellic evaluation hooks", read by no CMakeLists, no source, no
workflow and not by options.cmake itself. The evaluation it named is
script-driven (scripts/eval_rellic.sh looks for rellic-decompile on PATH) and
never needed a build option.

An option counts as read when its name appears, outside its own declaration, in
a file the BUILD consults: a .cmake, a CMakeLists.txt, a configure_file template
or a C/C++ source. Its own file counts -- options.cmake computes with several of
its switches, and RETDEC_DEV_TOOLS and RETDEC_ENABLE_LLVM_SUPPORT are both used
that way and are not dead.

A mention in a document, a shell script or a workflow does not count. Those name
an option to SET it or to describe it; neither makes the build do anything with
it, and letting them count would mean an option stays "read" precisely because
something promised it works. This file is deliberately in that category too: it
names the option it was written for.

Usage:
  python3 scripts/ci/check_cmake_options.py [--self-test]

Exit status is 0 when every declared option has a reader, 1 otherwise.
"""

import os
import re
import sys

DECL = re.compile(r"^[ \t]*option\([ \t]*([A-Za-z0-9_]+)", re.MULTILINE)

SKIP_DIRS = {".git", "build", "node_modules", "__pycache__", "install"}

# Files the build itself consults. A .md, .sh, .py or .yml that names an option
# is setting or describing it, not reading it.
READER_SUFFIXES = (".cmake", ".cmake.in", ".cpp", ".cc", ".cxx", ".c", ".h",
				   ".hpp", ".cu", ".in")
READER_NAMES = ("CMakeLists.txt",)

# An option a reader is not expected to exist for, with the reason. Empty today.
ALLOWED_UNREAD = {}


def declarations(root):
	found = {}
	for dirpath, dirnames, filenames in os.walk(os.path.join(root, "cmake")):
		dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
		for name in filenames:
			if not name.endswith(".cmake"):
				continue
			path = os.path.join(dirpath, name)
			with open(path, encoding="utf-8", errors="replace") as fh:
				text = fh.read()
			for m in DECL.finditer(text):
				found.setdefault(m.group(1), []).append(
					(os.path.relpath(path, root), text[:m.start()].count("\n") + 1))
	return found


def readers(root, names, decl_sites):
	"""For each name, the mentions that are not one of its own option() lines.

	One walk over the tree rather than one grep per option: with ~90 options the
	per-option grep took 28 seconds of mostly system time, and this takes under
	a second.
	"""
	hits = {n: [] for n in names}
	pattern = re.compile("|".join(re.escape(n) for n in sorted(names, key=len, reverse=True)))
	for dirpath, dirnames, filenames in os.walk(root):
		dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
		for fname in filenames:
			if not (fname.endswith(READER_SUFFIXES) or fname in READER_NAMES):
				continue
			path = os.path.join(dirpath, fname)
			rel = os.path.relpath(path, root).replace(os.sep, "/")
			try:
				with open(path, encoding="utf-8", errors="replace") as fh:
					lines = fh.read().splitlines()
			except OSError:
				continue
			for lineno, line in enumerate(lines, 1):
				for m in pattern.finditer(line):
					name = m.group(0)
					if (rel, lineno) in decl_sites.get(name, ()):
						continue
					hits[name].append(f"{rel}:{lineno}")
	return hits


def check(root):
	decls = declarations(root)
	names = [n for n in decls if n not in ALLOWED_UNREAD]
	if not names:
		return []
	decl_sites = {n: set(decls[n]) for n in names}
	hits = readers(root, names, decl_sites)
	problems = []
	for name in sorted(names):
		if hits[name]:
			continue
		where = ", ".join(f"{p}:{l}" for p, l in decls[name])
		problems.append(
			f"{where}: option {name} is declared and read by nothing, so "
			f"setting it changes nothing while its help text says it does")
	return problems


def self_test():
	import tempfile

	failures = []
	with tempfile.TemporaryDirectory() as d:
		os.mkdir(os.path.join(d, "cmake"))
		with open(os.path.join(d, "cmake", "options.cmake"), "w") as fh:
			fh.write('option(A_USED "" OFF)\noption(A_DEAD "" OFF)\n')
		with open(os.path.join(d, "CMakeLists.txt"), "w") as fh:
			fh.write("if(A_USED)\nendif()\n")
		got = check(d)
		if len(got) != 1 or "A_DEAD" not in got[0]:
			failures.append(f"expected one report about A_DEAD, got {got}")

		with open(os.path.join(d, "CMakeLists.txt"), "a") as fh:
			fh.write("if(A_DEAD)\nendif()\n")
		got = check(d)
		if got:
			failures.append(f"expected no reports once A_DEAD is read, got {got}")

	for f in failures:
		print(f"SELF-TEST FAIL: {f}")
	if failures:
		return 1
	print("SELF-TEST OK: the check reports an unread option and clears once it is read")
	return 0


def main(argv):
	if "--self-test" in argv:
		return self_test()
	root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
	problems = check(root)
	for p in problems:
		print(p)
	if problems:
		print(f"FAIL: {len(problems)} declared build option(s) that nothing reads")
		return 1
	print("OK: every option() declared under cmake/ is read somewhere")
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
