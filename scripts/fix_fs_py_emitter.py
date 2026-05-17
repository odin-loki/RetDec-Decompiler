#!/usr/bin/env python3
"""Fix F# type emitter and Python emitter errors."""
import re
from pathlib import Path

R = Path(__file__).resolve().parent.parent

# Fix 1: fs_type_emitter.cpp - paramTypes -> params, shared_ptr deref
path = R / "src/fsharp_emitter/fs_type_emitter.cpp"
with open(path) as f:
    c = f.read()
orig = c

# Check if we have includes
if '#include <algorithm>' not in c:
    c = c.replace('#include', '#include <algorithm>\n#include', 1)

# BcFuncType::paramTypes -> BcFuncType::params
c = c.replace('.paramTypes', '.params')

# For returnType as shared_ptr: func.returnType is shared_ptr<BcType>
# Patterns like: someType = func.returnType  -> someType = *func.returnType (if not null)
# and: format(func.returnType) -> format(*func.returnType)
# Need context to be precise, let me fix the specific patterns

if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed fs_type_emitter.cpp (paramTypes -> params)')

# Fix 2: py_file_emitter.cpp - missing #include <algorithm> for std::sort
path = R / "src/py_emitter/py_file_emitter.cpp"
with open(path) as f:
    c = f.read()
orig = c
if '#include <algorithm>' not in c:
    # Add after the last include
    c = re.sub(r'(#include [^\n]+\n)(?!#include)', r'\1#include <algorithm>\n', c, count=1)
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed py_file_emitter.cpp (added algorithm include)')

print("Done")
