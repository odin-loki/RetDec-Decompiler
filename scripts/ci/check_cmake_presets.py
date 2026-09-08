#!/usr/bin/env python3
"""Presets that share a build directory must set the same cache variables.

A CMake preset's cacheVariables are handed to the configure the way -D is, so
they overwrite whatever the cache already holds. A variable a preset does NOT
name is a different matter: it keeps the value the previous configure left in
that directory. When two presets share a binaryDir, every variable one of them
sets and the other does not leaks across.

That is not theoretical here. Measured on a two-option toy project, configuring
`san` (MY_SAN=ON) and then `rel` (which sets only MY_LTO) in the same directory:

    -- RESULT MY_SAN=ON MY_LTO=OFF
    -- RESULT MY_SAN=ON MY_LTO=ON      <-- a "release" configure, with ASan on

Six of this repository's presets shared ${sourceDir}/build/linux and four shared
${sourceDir}/build/windows, and CI reuses those directories across workflows
through the actions/cache steps in ctest-linux.yml, sanitizers.yml and
coverage.yml. The worst of it was not the sanitizer: full-linux-debug and
full-linux-release name no RETDEC_ENABLE_<component> at all, because they want
RETDEC_ENABLE_ALL. cmake/options.cmake turns ALL off as soon as any component
option is set -- so a core-* configure in that directory left nine of them ON in
the cache and the next "full" build silently became a core build.

The fix is not to move the directories: 389 references across the workflows,
scripts and docs name build/linux and build/windows directly. It is to give
every preset the same key set, which is what this checks.

Usage:
  python3 scripts/ci/check_cmake_presets.py [presets.json ...]
  python3 scripts/ci/check_cmake_presets.py --self-test

Exit status is 0 when no key can leak, 1 otherwise.
"""

import json
import os
import sys


def load(path):
	with open(path, encoding="utf-8") as fh:
		return json.load(fh)


def resolve(presets):
	"""Map each preset name to its resolved (binaryDir, cacheVariables keys).

	CMake resolves `inherits` left to right with earlier entries winning, and a
	preset's own fields winning over all of them. Only the key set matters here,
	so the values are carried along only to report them.
	"""
	by_name = {p["name"]: p for p in presets}
	memo = {}

	def one(name, seen):
		if name in memo:
			return memo[name]
		if name in seen:
			raise ValueError(f"inherits cycle at {name}")
		p = by_name[name]
		inherits = p.get("inherits") or []
		if isinstance(inherits, str):
			inherits = [inherits]
		binary = None
		cache = {}
		# Later entries lose to earlier ones, so apply them in reverse and let
		# the earlier ones overwrite.
		for parent in reversed(inherits):
			pb, pc = one(parent, seen | {name})
			if pb is not None:
				binary = pb
			cache.update(pc)
		if p.get("binaryDir") is not None:
			binary = p["binaryDir"]
		cache.update(p.get("cacheVariables") or {})
		memo[name] = (binary, cache)
		return memo[name]

	return {name: one(name, frozenset()) for name in by_name}


def check(path):
	doc = load(path)
	presets = doc.get("configurePresets") or []
	if not presets:
		return []
	resolved = resolve(presets)

	groups = {}
	for p in presets:
		if p.get("hidden"):
			continue
		binary, cache = resolved[p["name"]]
		if binary is None:
			continue
		# A binaryDir carrying ${presetName} expands to a different directory
		# for every preset, so those presets share nothing. cmake/superbuild's
		# presets are written that way; the top-level ones are not, which is
		# what this check is about.
		if "${presetName}" in binary:
			continue
		groups.setdefault(binary, []).append((p["name"], set(cache)))

	problems = []
	for binary, members in sorted(groups.items()):
		if len(members) < 2:
			continue
		union = set()
		for _, keys in members:
			union |= keys
		for name, keys in members:
			for missing in sorted(union - keys):
				owners = sorted(n for n, k in members if missing in k)
				problems.append(
					f"{path}: preset '{name}' shares {binary} but never sets "
					f"{missing}, so it inherits whatever {', '.join(owners)} "
					f"left in that cache")
	return problems


SELF_TEST = {
	"version": 3,
	"configurePresets": [
		{"name": "_p", "hidden": True, "binaryDir": "${sourceDir}/b"},
		{"name": "san", "inherits": ["_p"], "cacheVariables": {"S": "ON", "L": "OFF"}},
		{"name": "rel", "inherits": ["_p"], "cacheVariables": {"L": "ON"}},
		{"name": "alone", "binaryDir": "${sourceDir}/c", "cacheVariables": {"X": "1"}},
	],
}


def self_test():
	import tempfile

	failures = []
	with tempfile.TemporaryDirectory() as d:
		bad = os.path.join(d, "bad.json")
		with open(bad, "w", encoding="utf-8") as fh:
			json.dump(SELF_TEST, fh)
		got = check(bad)
		if len(got) != 1 or "'rel'" not in got[0] or " S," not in got[0] + ",":
			failures.append(f"expected one report about 'rel' missing S, got {got}")

		fixed = json.loads(json.dumps(SELF_TEST))
		fixed["configurePresets"][2]["cacheVariables"]["S"] = "OFF"
		good = os.path.join(d, "good.json")
		with open(good, "w", encoding="utf-8") as fh:
			json.dump(fixed, fh)
		got = check(good)
		if got:
			failures.append(f"expected no reports once the key is set, got {got}")

	for f in failures:
		print(f"SELF-TEST FAIL: {f}")
	if failures:
		return 1
	print("SELF-TEST OK: the check reports a leaking key and clears once it is set")
	return 0


def main(argv):
	if "--self-test" in argv:
		return self_test()
	paths = [a for a in argv if not a.startswith("-")] or ["CMakePresets.json"]
	problems = []
	for path in paths:
		problems.extend(check(path))
	for p in problems:
		print(p)
	if problems:
		print(f"FAIL: {len(problems)} cache variable(s) can leak between presets "
			  f"sharing a build directory")
		return 1
	print(f"OK: every preset sharing a build directory in {', '.join(paths)} "
		  f"sets the same cache variables")
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
