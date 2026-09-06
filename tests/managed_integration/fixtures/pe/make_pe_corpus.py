#!/usr/bin/env python3
"""Generate minimal but structurally valid PE seeds for the pelib fuzz target.

There is no PE binary anywhere in this tree, so scripts/standalone_fuzz.sh had
nothing to seed the `pelib` target with and the fuzzer had to discover "MZ" and
a plausible e_lfanew by chance. It spent 423,000 executions doing that and found
nothing, which says more about the seeds than about src/pelib.

These files are assembled here rather than committed as opaque blobs so a
reader can see exactly what each one is, and so a new variant is a few lines
rather than a hex editor. They are seeds, not tests: the point is to start the
mutator inside the parser rather than outside it.

    python3 tests/managed_integration/fixtures/pe/make_pe_corpus.py

Copyright (c) 2026 Odin Loch trading as Imortek
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ── DOS header ───────────────────────────────────────────────────────────────
# Only e_magic and e_lfanew matter to a loader; the rest is the historical stub.
DOS_STUB_LEN = 0x40


def dos_header(pe_offset: int) -> bytes:
    h = bytearray(DOS_STUB_LEN)
    h[0:2] = b"MZ"
    struct.pack_into("<I", h, 0x3C, pe_offset)
    return bytes(h)


def section_header(name: bytes, vsize: int, vaddr: int, rawsize: int,
                   rawptr: int, characteristics: int) -> bytes:
    return (
        name.ljust(8, b"\0")
        + struct.pack("<IIII", vsize, vaddr, rawsize, rawptr)
        + struct.pack("<IIHH", 0, 0, 0, 0)
        + struct.pack("<I", characteristics)
    )


def build_pe(pe32_plus: bool) -> bytes:
    """A one-section executable with no data directories populated."""
    machine = 0x8664 if pe32_plus else 0x014C
    magic = 0x20B if pe32_plus else 0x10B
    # 16 directory entries, whatever the bitness.
    opt_size = (0x70 if pe32_plus else 0x60) + 16 * 8

    pe_off = DOS_STUB_LEN
    headers_end = pe_off + 4 + 20 + opt_size + 40
    # Headers are padded to FileAlignment; sections start there.
    file_align, sect_align = 0x200, 0x1000
    raw_ptr = (headers_end + file_align - 1) // file_align * file_align

    coff = struct.pack(
        "<HHIIIHH",
        machine,
        1,          # NumberOfSections
        0,          # TimeDateStamp -- zero keeps the seed reproducible
        0, 0,       # symbol table (deprecated)
        opt_size,
        0x0002 | (0x0100 if not pe32_plus else 0x0020),  # EXECUTABLE_IMAGE
    )

    # Standard fields, then Windows-specific fields. ImageBase is the one field
    # whose width differs between PE32 and PE32+.
    opt = struct.pack("<HBBIIIII", magic, 14, 0, 0x200, 0, 0, 0x1000, 0x1000)
    if not pe32_plus:
        opt += struct.pack("<I", 0)          # BaseOfData
        opt += struct.pack("<I", 0x00400000)  # ImageBase
    else:
        opt += struct.pack("<Q", 0x0000000140000000)
    opt += struct.pack("<IIHHHHHH", sect_align, file_align, 6, 0, 0, 0, 6, 0)
    opt += struct.pack("<III", 0, sect_align + 0x1000, raw_ptr)
    opt += struct.pack("<IHH", 0, 3, 0x8160)  # Subsystem CONSOLE, DllCharacteristics
    if not pe32_plus:
        opt += struct.pack("<IIII", 0x100000, 0x1000, 0x100000, 0x1000)
    else:
        opt += struct.pack("<QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)
    opt += struct.pack("<II", 0, 16)          # LoaderFlags, NumberOfRvaAndSizes
    opt += b"\0" * (16 * 8)                   # empty data directories
    assert len(opt) == opt_size, (len(opt), opt_size)

    # A single executable section holding one RET, so there is real code to walk.
    body = b"\xC3" + b"\0" * (file_align - 1)
    sect = section_header(b".text", len(body), 0x1000, len(body), raw_ptr,
                          0x60000020)  # CODE | EXECUTE | READ

    out = bytearray()
    out += dos_header(pe_off)
    out += b"PE\0\0" + coff + opt + sect
    out += b"\0" * (raw_ptr - len(out))
    out += body
    return bytes(out)


def build_pe_with_export_dir() -> bytes:
    """PE32 whose export directory RVA points into the section.

    Reaches readExportDirectory, which is otherwise unreachable from a seed with
    no data directories at all.
    """
    base = bytearray(build_pe(pe32_plus=False))
    # Data directory 0 (export) sits at the end of the optional header.
    opt_start = DOS_STUB_LEN + 4 + 20
    dir_start = opt_start + 0x60
    struct.pack_into("<II", base, dir_start, 0x1000, 0x28)
    return bytes(base)


def main() -> int:
    seeds = {
        "minimal-pe32": build_pe(pe32_plus=False),
        "minimal-pe32plus": build_pe(pe32_plus=True),
        "pe32-export-dir": build_pe_with_export_dir(),
    }
    for name, data in seeds.items():
        path = os.path.join(HERE, name + ".exe")
        with open(path, "wb") as fh:
            fh.write(data)
        print(f"{name}.exe  {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
