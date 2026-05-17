#!/usr/bin/env python3
"""
Fix GCC 13 CWG 1990 restriction: nested struct with default member initializers
used as default argument inside the same enclosing class.

Strategy: For each class with a nested struct (Config/Options/LiftOptions) that has
default member initializers (DMIs) and is used as a default argument inside the class:
  1. Add a static factory method after the struct: `static Struct defaultStruct() noexcept { return {}; }`
  2. Replace `= Struct{}` default args with `= defaultStruct()`

The factory method body `return {};` is NOT a default argument, so the CWG 1990
restriction does not apply there. The default argument `= defaultStruct()` is a
function call, not aggregate initialization, so GCC 13 accepts it.
"""

import re
import sys
import os
import glob

STRUCT_NAMES = ['Config', 'Options', 'LiftOptions']

FACTORY_METHODS = {
    'Config':      'defaultConfig',
    'Options':     'defaultOptions',
    'LiftOptions': 'defaultLiftOptions',
}

def find_matching_brace(text, start):
    """Find the index of the closing '}' matching the '{' at index start."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def has_dmi(struct_body):
    """Return True if struct body contains default member initializers (= value)."""
    # Look for member declarations with = (but not ==)
    return bool(re.search(r'[^=!<>]=\s*[^=]', struct_body))


def process_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    original = content
    changed = False

    for struct_name in STRUCT_NAMES:
        factory_name = FACTORY_METHODS[struct_name]

        # Check if this file has "= StructName{}" default args
        if not re.search(r'=\s*' + re.escape(struct_name) + r'\{\}', content):
            continue

        # Check if already fixed (factory method already exists)
        if factory_name + '(' in content:
            # Replace remaining = StructName{} with factory call
            new_content = re.sub(
                r'=\s*' + re.escape(struct_name) + r'\{\}',
                '= ' + factory_name + '()',
                content
            )
            if new_content != content:
                content = new_content
                changed = True
            continue

        # Find ALL "struct StructName {" occurrences with DMIs in the file
        # and add a factory method after each one (each may be in a different class).
        pattern = re.compile(r'(\bstruct\s+' + re.escape(struct_name) + r'\s*\{)')
        offset = 0  # track cumulative insertion offset
        for m in pattern.finditer(content):
            adj_start = m.start() + offset
            struct_open = content.index('{', adj_start)
            struct_close = find_matching_brace(content, struct_open)
            if struct_close == -1:
                continue

            struct_body = content[struct_open + 1:struct_close]
            if not has_dmi(struct_body):
                continue

            # Check if factory already added right after this struct
            after_struct = content[struct_close:struct_close + 200]
            if 'static ' + struct_name + ' ' + factory_name in after_struct:
                continue  # Already has factory

            # Check for semicolon after closing brace
            after_close = struct_close + 1
            while after_close < len(content) and content[after_close] in ' \t':
                after_close += 1
            if after_close < len(content) and content[after_close] == ';':
                insert_pos = after_close + 1
            else:
                insert_pos = struct_close + 1

            # Determine indentation by looking at the struct line
            line_start = content.rfind('\n', 0, adj_start) + 1
            indent = ''
            for ch in content[line_start:adj_start]:
                if ch in ' \t':
                    indent += ch
                else:
                    break

            factory_method = (
                '\n' + indent + 'static ' + struct_name + ' ' + factory_name +
                '() noexcept { return {}; }'
            )

            content = content[:insert_pos] + factory_method + content[insert_pos:]
            offset += len(factory_method)
            changed = True

        # Now replace all "= StructName{}" with factory calls
        new_content = re.sub(
            r'=\s*' + re.escape(struct_name) + r'\{\}',
            '= ' + factory_name + '()',
            content
        )
        if new_content != content:
            content = new_content
            changed = True

    if changed:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'Fixed: {filepath}')
    else:
        pass  # No changes needed


def main():
    workspace = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    include_dir = os.path.join(workspace, 'include')

    headers = glob.glob(os.path.join(include_dir, '**', '*.h'), recursive=True)
    headers.sort()

    for h in headers:
        process_file(h)

    print('Done.')


if __name__ == '__main__':
    main()
