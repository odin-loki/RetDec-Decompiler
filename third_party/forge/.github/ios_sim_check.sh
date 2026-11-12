#!/bin/sh
# Runs command-line test executables built for the iOS simulator (arm64) inside a booted iPhone simulator.
#   sh .github/ios_sim_check.sh <build-dir> test_os test_text ...
# Picks the newest available iPhone simulator, boots it, and runs each test with `xcrun simctl spawn`
# (exit 77 = skip). Environment for the tests goes through SIMCTL_CHILD_* variables.
set -eu
b=$(cd "$1" && pwd); shift
dev=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
c = []
for rt, devs in json.load(sys.stdin)["devices"].items():
    if ".iOS-" not in rt: continue
    ver = [int(p) for p in rt.rsplit(".iOS-", 1)[1].split("-") if p.isdigit()]
    c += [(ver, d["name"], d["udid"]) for d in devs if d.get("isAvailable", True) and d["name"].startswith("iPhone")]
if not c:
    sys.exit("no iPhone simulator available")
ver, name, udid = max(c)
print(udid, "iOS " + ".".join(map(str, ver)), name)
')
udid=${dev%% *}; echo "simulator: ${dev#* }"
xcrun simctl bootstatus "$udid" -b >/dev/null
export SIMCTL_CHILD_FORGE_THREADS=${FORGE_THREADS:-2}
fail=0; pass=0; skip=0
for t in "$@"; do
  echo "::group::$t"
  exe="$b/$t"; [ -f "$exe" ] || exe="$b/$t.app/$t"   # CMake builds iOS executables as .app bundles
  rc=0; xcrun simctl spawn "$udid" "$exe" || rc=$?
  echo "::endgroup::"
  case $rc in 0) pass=$((pass+1)); echo "PASS $t";; 77) skip=$((skip+1)); echo "SKIP $t";;
    *) fail=$((fail+1)); echo "FAIL $t (rc=$rc)";; esac
done
xcrun simctl shutdown "$udid" || true
echo "iOS simulator: $pass passed, $skip skipped, $fail failed"
[ $fail -eq 0 ]
