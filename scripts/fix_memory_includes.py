#!/usr/bin/env python3
"""Add missing #include <memory> to files using unique_ptr or shared_ptr."""
import os
import re
from pathlib import Path

base = str(Path(__file__).resolve().parent.parent)
fixed = 0

for root, dirs, files in os.walk(base):
    dirs[:] = [d for d in dirs if d not in ['build', '.git', 'deps', 'external']]
    for fn in files:
        if not (fn.endswith('.h') or fn.endswith('.cpp')):
            continue
        path = os.path.join(root, fn)
        try:
            with open(path) as f:
                c = f.read()
        except Exception:
            continue
        needs_memory = ('unique_ptr' in c or 'make_unique' in c or 
                        'shared_ptr' in c or 'make_shared' in c)
        if needs_memory and '#include <memory>' not in c:
            m = re.search(r'(#include\s+[<"][^\n]+\n)', c)
            if m:
                c2 = c[:m.start()] + '#include <memory>\n' + c[m.start():]
                with open(path, 'w') as f:
                    f.write(c2)
                fixed += 1
                print(f'Fixed: {fn}')

print(f'Fixed {fixed} files total')
