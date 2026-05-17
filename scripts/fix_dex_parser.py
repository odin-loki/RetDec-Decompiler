#!/usr/bin/env python3
"""Fix dex_parser compilation errors."""
import re
from pathlib import Path

R = Path(__file__).resolve().parent.parent


def fix_file(path, fixes):
    with open(path) as f:
        c = f.read()
    orig = c
    for old, new in fixes:
        if callable(new):
            c = new(c)
        else:
            c = c.replace(old, new)
    if c != orig:
        with open(path, 'w') as f:
            f.write(c)
        print(f"Fixed {Path(path).name}")
    else:
        print(f"No changes in {Path(path).name}")

# Fix dex_lifter.cpp
path = R / "src/dex_parser/dex_lifter.cpp"
with open(path) as f:
    c = f.read()
orig = c

# BcBlockId -> uint32_t
c = c.replace('static BcOperand makeBlock(BcBlockId id)',
              'static BcOperand makeBlock(uint32_t id)')
c = c.replace('BcBlockId handlerBlock = 0;', 'uint32_t handlerBlock = 0;')
c = c.replace('BcBlockId catchAllBlock = 0;', 'uint32_t catchAllBlock = 0;')
c = re.sub(r'static_cast<BcBlockId>\(', 'static_cast<uint32_t>(', c)

# BcRefType::Class -> BcRefKind::Class
c = c.replace('BcRefType::Class', 'BcRefKind::Class')
# BcRefType::Array -> BcRefKind::Array
c = c.replace('BcRefType::Array', 'BcRefKind::Array')
# ref.name = -> ref.className =
c = c.replace('ref.name = ', 'ref.className = ')
# BcType(ref) -> BcType{ref}
c = re.sub(r'BcType\(ref\)', 'BcType{ref}', c)
# BcType(std::move(ref)) -> BcType{std::move(ref)}
c = re.sub(r'BcType\(std::move\(ref\)\)', r'BcType{std::move(ref)}', c)

# BcFuncType::ret -> returnType
c = c.replace('.ret =', '.returnType =')
c = c.replace('.ret;', '.returnType;')
c = c.replace('.ret)', '.returnType)')
c = c.replace('.ret ', '.returnType ')

# params.push_back(someType) -> params.push_back(std::make_shared<BcType>(someType))
c = re.sub(r'\.params\.push_back\((?!std::make_shared)([^)]+)\)',
           r'.params.push_back(std::make_shared<BcType>(\1))',
           c)

# catchAll.catchType = ""; -> catchAll.catchType = std::nullopt;
c = c.replace('catchAll.catchType    = "";', 'catchAll.catchType    = std::nullopt;')
c = c.replace('catchAll.catchType = "";', 'catchAll.catchType = std::nullopt;')

if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed dex_lifter.cpp')

# Fix dex_class_parser.cpp
path = R / "src/dex_parser/dex_class_parser.cpp"
with open(path) as f:
    c = f.read()
orig = c

# Remove BcAccess::Interface, Annotation, Enum flags (not in enum)
c = c.replace('    if (flags & ACC_INTERFACE)    acc |= static_cast<uint32_t>(BcAccess::Interface);\n', '')
c = c.replace('    if (flags & ACC_ANNOTATION)   acc |= static_cast<uint32_t>(BcAccess::Annotation);\n', '')
c = c.replace('    if (flags & ACC_ENUM)         acc |= static_cast<uint32_t>(BcAccess::Enum);\n', '')

# BcRefType::Class -> BcRefKind::Class
c = c.replace('BcRefType::Class', 'BcRefKind::Class')
# BcRefType::Array -> BcRefKind::Array
c = c.replace('BcRefType::Array', 'BcRefKind::Array')
# ref.name = -> ref.className =
c = c.replace('ref.name = ', 'ref.className = ')
# BcType(ref) -> BcType{ref}
c = re.sub(r'BcType\(ref\)', 'BcType{ref}', c)

# BcFuncType::ret -> returnType
c = c.replace('.ret =', '.returnType =')
c = c.replace('.ret;', '.returnType;')

# params.push_back(descriptorToType(...)) -> params.push_back(std::make_shared<BcType>(descriptorToType(...)))
c = re.sub(r'\.params\.push_back\((?!std::make_shared)([^)]+)\)',
           r'.params.push_back(std::make_shared<BcType>(\1))',
           c)

if c != orig:
    with open(path, 'w') as f:
        f.write(c)
    print('Fixed dex_class_parser.cpp')

print("Done")
