import json, sys

with open(sys.argv[1]) as f:
    cmds = json.load(f)

target = sys.argv[2] if len(sys.argv) > 2 else 'llvmir2hll'
matches = [c for c in cmds if target in c['file'] and c['file'].endswith('.cpp')]

if matches:
    print("Sample command:")
    print(matches[0]['command'][:500])
    print("...")
    print(f"\nTotal {target} source files: {len(matches)}")
    # Extract flags
    cmd = matches[0]['command']
    parts = cmd.split()
    includes = [p for p in parts if p.startswith('-I')]
    defines = [p for p in parts if p.startswith('-D')]
    print(f"\nIncludes ({len(includes)}):")
    for i in includes[:5]:
        print(' ', i)
    print(f"\nDefines ({len(defines)}):")
    for d in defines[:5]:
        print(' ', d)
