#!/usr/bin/env python3
"""
Generate malformed/edge-case fixture variants for parser robustness testing.

Run this script from the fixtures/ directory after compiling all sources:

    cd tests/managed_integration/fixtures
    python3 malformed/generate_malformed.py

Requires compiled binaries to already exist (run compile_fixtures.sh first).
"""
import os
import sys
import shutil
import struct
import random

FIXTURES_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(FIXTURES_DIR, "malformed")

def ensure_dir(path: str):
    os.makedirs(path, exist_ok=True)

def truncate(src: bytes, factor: float) -> bytes:
    """Keep only the first `factor` fraction of the file."""
    return src[:max(4, int(len(src) * factor))]

def corrupt_magic(src: bytes, offset: int, bad_byte: int) -> bytes:
    """Flip one byte of the magic number."""
    data = bytearray(src)
    if offset < len(data):
        data[offset] = bad_byte
    return bytes(data)

def flip_random_bytes(src: bytes, n: int, seed: int = 42) -> bytes:
    """Randomly flip n bytes in the file body (skip first 16 bytes = magic/header)."""
    rng = random.Random(seed)
    data = bytearray(src)
    for _ in range(n):
        idx = rng.randint(16, max(16, len(data) - 1))
        data[idx] ^= 0xFF
    return bytes(data)

def null_section(src: bytes, start: int, length: int) -> bytes:
    """Zero out a section of the file."""
    data = bytearray(src)
    for i in range(start, min(start + length, len(data))):
        data[i] = 0
    return bytes(data)

def write_variant(name: str, data: bytes):
    path = os.path.join(OUT_DIR, name)
    ensure_dir(os.path.dirname(path))
    with open(path, "wb") as f:
        f.write(data)
    print(f"  wrote {name} ({len(data)} bytes)")

def make_tiny_valid_class() -> bytes:
    """
    Minimal valid Java .class file:
    magic=CAFEBABE, minor=0, major=55 (Java 11),
    const_pool_count=1, access=ACC_PUBLIC, this_class=0, super_class=0,
    0 interfaces, 0 fields, 0 methods, 0 attributes.
    (Not loadable by JVM, but exercises the parser path.)
    """
    return (
        b"\xca\xfe\xba\xbe"  # magic
        b"\x00\x00"          # minor version
        b"\x00\x37"          # major version = 55 (Java 11)
        b"\x00\x01"          # constant_pool_count = 1 (empty pool)
        b"\x00\x21"          # access_flags = ACC_PUBLIC | ACC_SUPER
        b"\x00\x00"          # this_class = 0 (invalid but parseable)
        b"\x00\x00"          # super_class = 0
        b"\x00\x00"          # interfaces_count = 0
        b"\x00\x00"          # fields_count = 0
        b"\x00\x00"          # methods_count = 0
        b"\x00\x00"          # attributes_count = 0
    )

def make_max_pool_class() -> bytes:
    """
    .class with a large constant pool (many Utf8 entries) to stress pool iteration.
    """
    POOL_SIZE = 4000
    entries = []
    for i in range(POOL_SIZE - 1):
        word = f"entry_{i:04d}"
        encoded = word.encode("utf-8")
        entries.append(b"\x01" + struct.pack(">H", len(encoded)) + encoded)
    pool = b"".join(entries)
    return (
        b"\xca\xfe\xba\xbe"
        b"\x00\x00"
        b"\x00\x37"
        + struct.pack(">H", POOL_SIZE)
        + pool
        + b"\x00\x21\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    )

def make_truncated_wasm() -> bytes:
    """WASM magic + version + code section header that is cut off."""
    return b"\x00asm\x01\x00\x00\x00\x0a\x80\x80\x80"  # truncated LEB128

def make_bad_magic_wasm() -> bytes:
    """WASM with corrupted magic."""
    return b"\x00WOW\x01\x00\x00\x00"

def make_truncated_pyc_38() -> bytes:
    """
    Python 3.8 pyc header (16 bytes) with no code object body.
    Magic = 0x0D550D0A (3.8.x).
    """
    magic = b"\x55\x0d\x0d\x0a"  # Python 3.8 magic
    bitfield = struct.pack("<I", 0)
    mtime = struct.pack("<I", 0)
    size = struct.pack("<I", 0)
    return magic + bitfield + mtime + size  # header only, no marshal data

def process_file(src_path: str, lang: str, basename: str):
    if not os.path.exists(src_path):
        print(f"  [skip] {src_path} not found (compile first)")
        return
    with open(src_path, "rb") as f:
        data = f.read()

    print(f"Processing {lang}/{basename} ({len(data)} bytes):")

    write_variant(f"{lang}/{basename}.truncated_50pct", truncate(data, 0.50))
    write_variant(f"{lang}/{basename}.truncated_10pct", truncate(data, 0.10))
    write_variant(f"{lang}/{basename}.truncated_4bytes", data[:4])
    write_variant(f"{lang}/{basename}.random_flipped",  flip_random_bytes(data, 50))
    write_variant(f"{lang}/{basename}.null_body",       null_section(data, 16, len(data) - 16))

def main():
    ensure_dir(OUT_DIR)

    # Java .class files
    java_dir = os.path.join(FIXTURES_DIR, "java")
    for cls in ["Hello", "Loops", "Exceptions", "Generics", "Lambdas"]:
        src = os.path.join(java_dir, f"{cls}.class")
        process_file(src, "java", f"{cls}.class")

    # Synthetic minimal/maximal .class files (no compiled source needed)
    print("Generating synthetic .class variants:")
    write_variant("java/Tiny.class",    make_tiny_valid_class())
    write_variant("java/MaxPool.class", make_max_pool_class())
    write_variant("java/BadMagic.class", corrupt_magic(make_tiny_valid_class(), 0, 0xDE))
    write_variant("java/EmptyFile.class", b"")

    # DEX files
    dex_dir = os.path.join(FIXTURES_DIR, "dex")
    for dex in ["Hello.dex", "classes.dex"]:
        src = os.path.join(dex_dir, dex)
        process_file(src, "dex", dex)

    # WASM files
    wasm_dir = os.path.join(FIXTURES_DIR, "wasm")
    for wasm in ["add.wasm", "memory.wasm", "table.wasm", "exceptions.wasm"]:
        src = os.path.join(wasm_dir, wasm)
        process_file(src, "wasm", wasm)

    # Synthetic WASM
    print("Generating synthetic WASM variants:")
    write_variant("wasm/truncated.wasm",  make_truncated_wasm())
    write_variant("wasm/bad_magic.wasm",  make_bad_magic_wasm())
    write_variant("wasm/empty.wasm",      b"")
    # Minimal valid WASM (magic + version only, no sections)
    write_variant("wasm/minimal.wasm",    b"\x00asm\x01\x00\x00\x00")

    # Python .pyc
    pyc_dir = os.path.join(FIXTURES_DIR, "python")
    for py in ["hello", "classes", "comprehensions", "generators", "closures"]:
        for ver in ["38", "311", "312"]:
            src = os.path.join(pyc_dir, f"{py}.cpython-{ver}.pyc")
            if not os.path.exists(src):
                # Try the __pycache__ path
                src = os.path.join(pyc_dir, "__pycache__", f"{py}.cpython-{ver}.pyc")
            process_file(src, f"python/py{ver}", f"{py}.pyc")

    # Synthetic pyc header-only
    print("Generating synthetic pyc variants:")
    write_variant("python/truncated_header.pyc", make_truncated_pyc_38())
    write_variant("python/empty.pyc", b"")
    write_variant("python/bad_magic.pyc", b"\xDE\xAD\xBE\xEF" + b"\x00" * 12)

    # Lua .luac
    lua_dir = os.path.join(FIXTURES_DIR, "lua")
    for lua in ["hello", "tables", "closures", "coroutines"]:
        for ver in ["51", "54"]:
            src = os.path.join(lua_dir, f"{lua}.luac{ver}")
            process_file(src, f"lua/lua{ver}", f"{lua}.luac")

    print("\nMalformed fixtures generated in:", OUT_DIR)

if __name__ == "__main__":
    main()
