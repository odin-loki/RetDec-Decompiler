#!/usr/bin/env python3
"""Generate minimal but structurally valid .NET assemblies for the `cil` fuzz target.

There is no .NET assembly anywhere in this tree. scripts/standalone_fuzz.sh's
seed glob for the `cil` target -- tests/managed_integration/fixtures/dotnet/*.dll
-- matched nothing, and the shared malformed-fixture pool is copied into every
target's corpus directory, so the run still printed a green line:

    ok  cil replayed 59 input(s)

for a run in which not one input was a .NET assembly. The 4,400-line CIL parser
was effectively unfuzzed while the gate reported otherwise. The seed-counting
fix in scripts/standalone_fuzz.sh makes that visible; these files make it go
away.

Assembled here rather than committed as opaque blobs, for the same reason as
the sibling script in ../pe/make_pe_corpus.py: a reader can see exactly what
each one is, and a new variant is a few lines rather than a hex editor. They are
seeds, not tests -- the point is to start the mutator inside the parser rather
than outside it.

    python3 tests/managed_integration/fixtures/dotnet/make_dotnet_corpus.py

Copyright (c) 2026 Odin Loch trading as Imortek
"""

import os
import struct
import sys

FILE_ALIGN = 0x200
SECT_ALIGN = 0x1000
TEXT_RVA = 0x2000
HEADERS = 0x200


def dos_header(pe_offset: int) -> bytes:
    hdr = bytearray(0x40)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, pe_offset)
    return bytes(hdr)


def metadata_root(version: bytes, streams: "list[tuple[bytes, bytes]]") -> bytes:
    """A #~-style metadata root: BSJB, a version string, then the stream headers."""
    padded = version + b"\0" * ((4 - len(version) % 4) % 4)
    header = bytearray()
    header += struct.pack("<I", 0x424A5342)      # "BSJB"
    header += struct.pack("<HH", 1, 1)           # major, minor
    header += struct.pack("<I", 0)               # reserved
    header += struct.pack("<I", len(padded))     # VersionLength
    header += padded
    header += struct.pack("<H", 0)               # flags
    header += struct.pack("<H", len(streams))    # NumberOfStreams

    # Stream headers are (offset, size, name) with the name padded to 4 bytes;
    # the offsets are relative to the start of the root, so the header size has
    # to be known before they can be filled in.
    names = [n + b"\0" * ((4 - len(n) % 4) % 4) for n, _ in streams]
    stream_header_size = sum(8 + len(n) for n in names)
    cursor = len(header) + stream_header_size

    body = bytearray()
    for (name, payload), padded_name in zip(streams, names):
        header += struct.pack("<II", cursor, len(payload))
        header += padded_name
        body += payload
        cursor += len(payload)

    return bytes(header) + bytes(body)


def build_assembly(*, with_metadata: bool, pe32_plus: bool = False) -> bytes:
    """A PE32(+) whose data directory 14 points at a COR20 header."""
    magic = 0x020B if pe32_plus else 0x010B
    opt_size = 240 if pe32_plus else 224
    dd_start = 112 if pe32_plus else 96

    # The #~ header is Reserved(4) MajorVersion(1) MinorVersion(1)
    # HeapSizes(1) Reserved(1) Valid(8) Sorted(8), then one uint32 row count per
    # bit set in Valid, then the rows themselves. Valid = 1 is the Module table,
    # whose single row with 2-byte heap indexes is Generation(2) Name(2)
    # Mvid(2) EncId(2) EncBaseId(2).
    tilde = bytearray()
    tilde += struct.pack("<I", 0)                 # Reserved
    tilde += struct.pack("<BB", 2, 0)             # MajorVersion, MinorVersion
    tilde += struct.pack("<BB", 0, 1)             # HeapSizes (all 2-byte), Reserved
    tilde += struct.pack("<Q", 1 << 0)            # Valid: Module only
    tilde += struct.pack("<Q", 0)                 # Sorted
    tilde += struct.pack("<I", 1)                 # Module row count
    tilde += struct.pack("<HHHHH", 0, 1, 0, 0, 0) # the Module row

    streams = [
        (b"#~", bytes(tilde)),
        (b"#Strings", b"\0" + b"minimal\0"),
        (b"#US", b"\0"),
        (b"#GUID", b"\0" * 16),
        (b"#Blob", b"\0"),
    ]
    md = metadata_root(b"v4.0.30319\0", streams) if with_metadata else b""

    cor20_rva = TEXT_RVA
    md_rva = TEXT_RVA + 72
    text = bytearray()
    text += struct.pack("<I", 72)                 # cb
    text += struct.pack("<HH", 2, 5)              # runtime version 2.5
    text += struct.pack("<II", md_rva if md else 0, len(md))
    text += struct.pack("<I", 0x00000001)         # COMIMAGE_FLAGS_ILONLY
    text += struct.pack("<I", 0)                  # EntryPointToken
    text += struct.pack("<II", 0, 0)              # Resources
    text += struct.pack("<II", 0, 0)              # StrongNameSignature
    text += struct.pack("<II", 0, 0)              # CodeManagerTable
    text += struct.pack("<II", 0, 0)              # VTableFixups
    text += struct.pack("<II", 0, 0)              # ExportAddressTableJumps
    text += struct.pack("<II", 0, 0)              # ManagedNativeHeader
    assert len(text) == 72, len(text)
    text += md

    raw_size = (len(text) + FILE_ALIGN - 1) // FILE_ALIGN * FILE_ALIGN
    size_of_image = TEXT_RVA + (len(text) + SECT_ALIGN - 1) // SECT_ALIGN * SECT_ALIGN

    pe_off = 0x80
    img = bytearray(dos_header(pe_off))
    img += b"\0" * (pe_off - len(img))

    coff = bytearray()
    coff += b"PE\0\0"
    coff += struct.pack("<H", 0x014C)             # Machine i386
    coff += struct.pack("<H", 1)                  # NumberOfSections
    coff += struct.pack("<I", 0)                  # TimeDateStamp
    coff += struct.pack("<I", 0)                  # PointerToSymbolTable
    coff += struct.pack("<I", 0)                  # NumberOfSymbols
    coff += struct.pack("<H", opt_size)
    coff += struct.pack("<H", 0x2102)             # DLL | 32BIT_MACHINE | EXECUTABLE
    img += coff

    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, magic)
    struct.pack_into("<I", opt, 16, 0)            # AddressOfEntryPoint
    struct.pack_into("<I", opt, 20, TEXT_RVA)     # BaseOfCode
    if pe32_plus:
        struct.pack_into("<Q", opt, 24, 0x400000)     # ImageBase
        struct.pack_into("<I", opt, 32, SECT_ALIGN)
        struct.pack_into("<I", opt, 36, FILE_ALIGN)
        struct.pack_into("<I", opt, 56, size_of_image)
        struct.pack_into("<I", opt, 60, HEADERS)
        struct.pack_into("<I", opt, 108, 16)          # NumberOfRvaAndSizes
    else:
        struct.pack_into("<I", opt, 24, 0)            # BaseOfData
        struct.pack_into("<I", opt, 28, 0x400000)     # ImageBase
        struct.pack_into("<I", opt, 32, SECT_ALIGN)
        struct.pack_into("<I", opt, 36, FILE_ALIGN)
        struct.pack_into("<I", opt, 56, size_of_image)
        struct.pack_into("<I", opt, 60, HEADERS)
        struct.pack_into("<I", opt, 92, 16)           # NumberOfRvaAndSizes
    # Data directory 14 is the COM descriptor -- what makes this a .NET assembly.
    struct.pack_into("<I", opt, dd_start + 14 * 8 + 0, cor20_rva)
    struct.pack_into("<I", opt, dd_start + 14 * 8 + 4, 72)
    img += opt

    sect = bytearray(40)
    sect[0:5] = b".text"
    struct.pack_into("<I", sect, 8, len(text))    # VirtualSize
    struct.pack_into("<I", sect, 12, TEXT_RVA)    # VirtualAddress
    struct.pack_into("<I", sect, 16, raw_size)    # SizeOfRawData
    struct.pack_into("<I", sect, 20, HEADERS)     # PointerToRawData
    struct.pack_into("<I", sect, 36, 0x60000020)  # CODE | EXECUTE | READ
    img += sect

    img += b"\0" * (HEADERS - len(img))
    img += text
    img += b"\0" * (raw_size - len(text))
    return bytes(img)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    files = {
        "minimal-cli.dll": build_assembly(with_metadata=True),
        "minimal-cli-pe32plus.dll": build_assembly(with_metadata=True, pe32_plus=True),
        # A COR20 header whose metadata RVA is 0: valid PE, valid CLI directory,
        # nothing behind it. The parser has to say so rather than assume.
        "cli-no-metadata.dll": build_assembly(with_metadata=False),
    }
    for name, payload in files.items():
        path = os.path.join(here, name)
        with open(path, "wb") as fh:
            fh.write(payload)
        print(f"wrote {path} ({len(payload)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
