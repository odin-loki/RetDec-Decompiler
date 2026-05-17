#!/usr/bin/env python3
import re
from pathlib import Path

R = Path(__file__).resolve().parent.parent

# Fix 1: java_class_emitter.cpp - opts_.classOpts.javaVersion -> opts_.javaVersion
path = R / "src/java_emitter/java_class_emitter.cpp"
with open(path) as f:
    c = f.read()
orig = c
c = c.replace('opts_.classOpts.javaVersion', 'opts_.javaVersion')
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed java_class_emitter.cpp')

# Fix 2: java_expr_emitter.cpp
path = R / "src/java_emitter/java_expr_emitter.cpp"
with open(path) as f:
    c = f.read()
orig = c
# Add missing includes
if '#include <algorithm>' not in c:
    c = '#include <algorithm>\n' + c
# Replace BcOpcode::Switch with TableSwitch/LookupSwitch
c = c.replace(
    'case BcOpcode::Switch:',
    'case BcOpcode::TableSwitch:\n        case BcOpcode::LookupSwitch:'
)
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed java_expr_emitter.cpp')

# Fix 3: java_stmt_emitter.cpp
path = R / "src/java_emitter/java_stmt_emitter.cpp"
with open(path) as f:
    c = f.read()
orig = c
c = c.replace(
    'insn.opcode == BcOpcode::Switch)',
    'insn.opcode == BcOpcode::TableSwitch ||\n            insn.opcode == BcOpcode::LookupSwitch)'
)
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed java_stmt_emitter.cpp')

# Fix 4: local_rebuild.cpp - params is vector<shared_ptr<BcType>>
path = R / "src/jvm_reconstruct/local_rebuild.cpp"
with open(path) as f:
    c = f.read()
orig = c
c = c.replace(
    'for (const auto& pt : method.descriptor.params)\n        paramTypes.push_back(pt);',
    'for (const auto& pt : method.descriptor.params)\n        if (pt) paramTypes.push_back(*pt);'
)
c = c.replace(
    'const BcType& ptype = method.descriptor.params[pi];',
    'const BcType& ptype = *method.descriptor.params[pi];'
)
c = c.replace(
    '        for (const auto& pt : method.descriptor.params) {\n'
    '            paramSlots.insert(ps);\n'
    '            bool wide = (pt.isPrim() &&\n'
    '                         (pt.prim() == BcPrimType{BcPrimKind::Long} ||\n'
    '                          pt.prim() == BcPrimType{BcPrimKind::Double}));',
    '        for (const auto& pt : method.descriptor.params) {\n'
    '            paramSlots.insert(ps);\n'
    '            bool wide = (pt && pt->isPrim() &&\n'
    '                         (pt->prim() == BcPrimType{BcPrimKind::Long} ||\n'
    '                          pt->prim() == BcPrimType{BcPrimKind::Double}));'
)
c = c.replace('t.name = "java.lang.Throwable"', 't.className = "java.lang.Throwable"')
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed local_rebuild.cpp')

# Fix 5: Add LCmp to BcOpcode enum in bc_instr.h
path = R / "include/retdec/bc_module/bc_instr.h"
with open(path) as f:
    c = f.read()
orig = c
if 'LCmp,' not in c:
    c = c.replace(
        'FCmpL, FCmpG,        ///< JVM fcmpl / fcmpg (NaN handling)',
        'FCmpL, FCmpG,        ///< JVM fcmpl / fcmpg (NaN handling)\n    LCmp,                ///< JVM lcmp -- push -1/0/1 for long compare'
    )
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed bc_instr.h (added LCmp)')

print("Done")
