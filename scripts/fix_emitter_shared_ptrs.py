#!/usr/bin/env python3
"""Fix shared_ptr dereferences and paramTypes in emitter files."""
import re
import os
from pathlib import Path

R = Path(__file__).resolve().parent.parent
files = [
    R / "src/vbnet_emitter/vb_type_emitter.cpp",
    R / "src/csharp_emitter/cs_type_emitter.cpp",
]

for path in files:
    if not path.exists():
        print(f"Skipping {path} (not found)")
        continue
    with open(path) as f:
        c = f.read()
    orig = c

    # Fix paramTypes -> params
    c = c.replace('.paramTypes', '.params')

    # Fix typeStr(pts[i]) -> typeStr(pts[i] ? *pts[i] : BcType{})
    c = re.sub(r'typeStr\(pts\[(\w+)\]\)',
               r'typeStr(pts[\1] ? *pts[\1] : BcType{})',
               c)

    # Fix typeStr(X.descriptor.returnType) -> typeStr(X.descriptor.returnType ? *X.descriptor.returnType : BcType{})
    c = re.sub(r'typeStr\((\w+)\.descriptor\.returnType\)',
               r'typeStr(\1.descriptor.returnType ? *\1.descriptor.returnType : BcType{})',
               c)

    # Fix: pg.type = X.descriptor.returnType;
    c = re.sub(r'(\w+\.type\s*=\s*)(\w+\.descriptor\.returnType);',
               r'\1\2 ? *\2 : BcType{};',
               c)

    if c != orig:
        with open(path, 'w') as f:
            f.write(c)
        print(f'Fixed {os.path.basename(path)}')
    else:
        print(f'No changes in {os.path.basename(path)}')

print("Done")
