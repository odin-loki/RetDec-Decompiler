#!/usr/bin/env python3
"""Fix CIL parser and reconstruct compilation errors."""
from pathlib import Path

R = Path(__file__).resolve().parent.parent

# Fix 1: Add DOTNET_CONV_OVF_I and DOTNET_CONV_OVF_U to BcOpcode enum
path = R / "include/retdec/bc_module/bc_instr.h"
with open(path) as f:
    c = f.read()
orig = c
if 'DOTNET_CONV_OVF_I,' not in c:
    c = c.replace(
        '    DOTNET_CONV_OVF_I1, DOTNET_CONV_OVF_U1, DOTNET_CONV_OVF_I2, DOTNET_CONV_OVF_U2,',
        '    DOTNET_CONV_OVF_I, DOTNET_CONV_OVF_U,\n    DOTNET_CONV_OVF_I1, DOTNET_CONV_OVF_U1, DOTNET_CONV_OVF_I2, DOTNET_CONV_OVF_U2,'
    )
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed bc_instr.h: added DOTNET_CONV_OVF_I/U')

# Fix 2: cil_lifter.cpp - fix BcMethodRef/BcFieldRef member names and tryBlock
path = R / "src/cli_parser/cil_lifter.cpp"
with open(path) as f:
    c = f.read()
orig = c
# BcMethodRef::className -> owner, methodName -> name
c = c.replace('ref.className  = "<class>";\n        ref.methodName = tokenToString(token);',
              'ref.owner = "<class>";\n        ref.name  = tokenToString(token);')
# BcFieldRef::className -> owner, fieldName -> name
c = c.replace('ref.className = "<class>";\n        ref.fieldName = tokenToString(token);',
              'ref.owner = "<class>";\n        ref.name  = tokenToString(token);')
# eh.tryBlock -> eh.startOffset/endOffset
c = c.replace(
    '        BcExceptionHandler eh;\n        eh.tryBlock     = tryIt->second;\n        eh.handlerBlock = hndIt->second;',
    '        BcExceptionHandler eh;\n        eh.startOffset  = clause.tryOffset;\n        eh.endOffset    = clause.tryOffset + clause.tryLength;\n        eh.handlerBlock = hndIt->second;'
)
# Fix duplicate case kFEPrefix | 0x16 - remove the second block (lines ~554-559)
# The first occurrence is in the large InlineMethod/InlineField block (line 536-538)
# The second is a standalone duplicate case at line 555
old_dup = '''    // constrained, readonly take a type token
    case kFEPrefix | 0x16: {
        uint32_t tok = r32(code, pos); pos += 4;
        addTok(tok);
        break;
    }'''
if old_dup in c:
    c = c.replace(old_dup, '    // kFEPrefix | 0x16 already handled above in InlineMethod block')
    print('  Removed duplicate case kFEPrefix|0x16')

# Fix duplicate case 0xB9 - check what's on line 534
# The error says lines 534 and 555. Let me check for other duplicates
# Actually let me check the specific duplicate
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed cil_lifter.cpp')

# Fix 3: cil_stack_sim.h - add operator== to StackSlot
path = R / "include/retdec/cil_reconstruct/cil_stack_sim.h"
with open(path) as f:
    c = f.read()
orig = c
if 'operator==(const StackSlot&' not in c:
    c = c.replace(
        '''struct StackSlot {
    BcType      type;   ///< Type of this stack slot
    CilExprPtr  expr;   ///< Expression that produced this value (may be null)
};''',
        '''struct StackSlot {
    BcType      type;   ///< Type of this stack slot
    CilExprPtr  expr;   ///< Expression that produced this value (may be null)

    bool operator==(const StackSlot& o) const noexcept { return type == o.type; }
    bool operator!=(const StackSlot& o) const noexcept { return !(*this == o); }
};'''
    )
if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed cil_stack_sim.h: added StackSlot::operator==')

print("Done")
