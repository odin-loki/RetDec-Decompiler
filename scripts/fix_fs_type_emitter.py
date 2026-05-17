#!/usr/bin/env python3
"""Fix shared_ptr dereferences in fs_type_emitter.cpp."""
import re
from pathlib import Path

R = Path(__file__).resolve().parent.parent
path = R / "src/fsharp_emitter/fs_type_emitter.cpp"
with open(path) as f:
    c = f.read()
orig = c

# Fix: typeStr(pts[i]) -> typeStr(pts[i] ? *pts[i] : BcType{})
c = re.sub(r'typeStr\(pts\[(\w+)\]\)',
           r'typeStr(pts[\1] ? *pts[\1] : BcType{})',
           c)

# Fix: typeStr(invoke.descriptor.returnType) -> typeStr(invoke.descriptor.returnType ? *invoke.descriptor.returnType : BcType{})
# and typeStr(m.descriptor.returnType) etc.
c = re.sub(r'typeStr\((\w+)\.descriptor\.returnType\)',
           r'typeStr(\1.descriptor.returnType ? *\1.descriptor.returnType : BcType{})',
           c)

# Fix: pg.type = m.descriptor.returnType;
c = c.replace('pg.type     = m.descriptor.returnType;',
              'pg.type     = m.descriptor.returnType ? *m.descriptor.returnType : BcType{};')

if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed fs_type_emitter.cpp')
else:
    print('No changes')

print("Done")
