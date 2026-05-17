#!/usr/bin/env python3
"""Fix pyc_reader.cpp API mismatches."""
import re
from pathlib import Path

R = Path(__file__).resolve().parent.parent
path = R / "src/pyc_parser/pyc_reader.cpp"
with open(path) as f:
    c = f.read()
orig = c

# Fix: operand = int64_t → operand = BcIntOperand{...}
c = re.sub(r'operand = static_cast<int64_t>\(([^)]+)\)', r'operand = BcIntOperand{static_cast<int64_t>(\1)}', c)
c = re.sub(r'operand = \(int64_t\)\(([^)]+)\)', r'operand = BcIntOperand{static_cast<int64_t>(\1)}', c)

# Fix: operand = c.sval (string) → operand = BcStringOperand{c.sval}
# Fix: operand = c.fval (float) → operand = BcFloatOperand{c.fval}
c = c.replace('operand = c.sval;', 'operand = BcStringOperand{c.sval};')
c = c.replace('operand = c.fval;', 'operand = BcFloatOperand{c.fval};')
# Also fix string assignments from vector lookup
c = re.sub(r'operand = (code\.co_(?:names|varnames|cellvars|freevars))\[(\w+)\];',
           r'operand = BcStringOperand{\1[\2]};', c)

# Fix: out.operand = ... → out.operands = {...} or push_back
c = c.replace('out.operand = std::move(operand);', 
              'out.operands.clear();\n    out.operands.push_back(std::move(operand));')

# Fix: holds_alternative<int64_t>(last.operand) → !last.operands.empty() && holds_alternative<BcIntOperand>(last.operands[0])
c = c.replace('std::holds_alternative<int64_t>(last.operand)',
              '!last.operands.empty() && std::holds_alternative<BcIntOperand>(last.operands[0])')
# Fix: std::get<int64_t>(last.operand) → std::get<BcIntOperand>(last.operands[0]).value
c = c.replace('std::get<int64_t>(last.operand)',
              'std::get<BcIntOperand>(last.operands[0]).value')

if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed pyc_reader.cpp')
else:
    print('No changes')

print("Done")
