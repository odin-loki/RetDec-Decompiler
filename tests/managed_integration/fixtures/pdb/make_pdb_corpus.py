#!/usr/bin/env python3
"""Generates the PDB corpus used by scripts/standalone_fuzz.sh for the pdb target.

    make_pdb_corpus.py tests/managed_integration/fixtures/pdb tests/crash_corpus/pdb

The first directory gets minimal.pdb and minimal-nonlinear.pdb, well formed PDB
7.00 files that cover the whole load path: streams stored in consecutive pages
and streams whose pages are scattered, a module list, a section header stream
and the DBI debug header.

The second gets one malformed variant per field that the parser used to take on
faith. Most of them made it read outside the file image, or size a container
from a number the file made up, before the bounds checks in
src/pdbparser/pdb_file.cpp were added; all of them stay checked in as regression
inputs for "standalone_fuzz.sh --replay pdb".
"""
import os, struct, sys

PAGE = 512
MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
assert len(MAGIC) == 32

def u32(x): return struct.pack("<I", x)
def u16(x): return struct.pack("<H", x)

def pdb_info_stream():
    v = u32(20000404) + u32(0) + u32(1)          # version, sig, age
    v += bytes(range(16))                         # guid
    v += u32(0) + u32(0)                          # empty name table
    return v

def tpi_stream():
    # TPIHash-less minimal TPI header: version, header size, ti_min, ti_max, follow size
    hdr = u32(20040203) + u32(56) + u32(0x1000) + u32(0x1000) + u32(0)
    hdr += u32(0) * ((56 - len(hdr)) // 4)
    return hdr

def modi(stream_num, name, obj):
    m = bytearray(0x40)
    struct.pack_into("<H", m, 0x22, stream_num)
    struct.pack_into("<i", m, 0x24, 0)
    rgch = name.encode() + b"\0" + obj.encode() + b"\0"
    while len(rgch) % 4 != 0:
        rgch += b"\0"
    return bytes(m) + rgch

def dbi_stream(mods, dbg_numbers, gsi, psi, sym):
    body = b"".join(mods)
    hdr = bytearray(0x40)
    struct.pack_into("<I", hdr, 0x00, 0xFFFFFFFF)   # verSignature
    struct.pack_into("<I", hdr, 0x04, 19990903)     # verHdr
    struct.pack_into("<I", hdr, 0x08, 1)            # age
    struct.pack_into("<H", hdr, 0x0C, gsi)
    struct.pack_into("<H", hdr, 0x10, psi)
    struct.pack_into("<H", hdr, 0x14, sym)
    # Real DBI streams carry the section contribution substream between the
    # module list and the debug header; the module name scan relies on it.
    sc = b"\x00" * 32
    struct.pack_into("<i", hdr, 0x18, len(body))    # cbGpModi
    struct.pack_into("<i", hdr, 0x1C, len(sc))      # cbSC
    struct.pack_into("<i", hdr, 0x30, 2 * len(dbg_numbers))  # cbDbgHdr
    dbg = b"".join(u16(n & 0xFFFF) for n in dbg_numbers)
    return bytes(hdr) + body + sc + dbg

def section(name, va, raw):
    s = name.encode()[:8].ljust(8, b"\0")
    s += u32(0x1000) + u32(va) + u32(0x200) + u32(raw)
    s += u32(0) + u32(0) + u16(0) + u16(0) + u32(0x60000020)
    return s

def build(nonlinear=False, sym_records=None, scatter=()):
    """sym_records replaces the contents of the symbol record stream, and
    scatter names streams whose pages are stored out of order even when the
    file is otherwise linear -- those get copied into an allocation of their
    own, which is what makes a read past the end of one visible."""
    sym = b"\x04\x00\x00\x00"                        # CV signature
    sections = section(".text", 0x1000, 0x400) + section(".data", 0x2000, 0x600)
    streams = [
        b"\x00" * 8,          # 0 old directory
        pdb_info_stream(),    # 1 PDB info
        tpi_stream(),         # 2 TPI
        None,                 # 3 DBI, filled below
        sym,                  # 4 GSI
        sym,                  # 5 PSI
        sym if sym_records is None else sym_records,  # 6 symbol records
        sections * 20,         # 7 section headers (spans several pages)
        b"",                  # 8 module stream (empty)
    ]
    # dbg header: index 5 is the section header stream, 0 = FPO, 9 = new FPO
    dbg_numbers = [0xFFFF] * 11
    dbg_numbers[5] = 7
    mods = [modi(8, "mod.obj", "mod.lib"), modi(0xFFFF, "second.obj", "second.lib")]
    streams[3] = dbi_stream(mods, dbg_numbers, 4, 5, 6)

    blocks, next_block = [], 3           # 0 = superblock, 1/2 = free page maps
    for idx, s in enumerate(streams):
        n = (len(s) + PAGE - 1) // PAGE
        bl = list(range(next_block, next_block + n))
        if (nonlinear or idx in scatter) and n > 1:
            bl.reverse()
        blocks.append(bl)
        next_block += n

    root = u32(len(streams))
    root += b"".join(u32(len(s)) for s in streams)
    root += b"".join(u32(b) for bl in blocks for b in bl)

    root_pages = list(range(next_block, next_block + (len(root) + PAGE - 1) // PAGE))
    next_block += len(root_pages)
    root_index_page = next_block
    next_block += 1
    total = next_block

    f = bytearray(total * PAGE)
    f[0:32] = MAGIC
    struct.pack_into("<I", f, 32, PAGE)
    struct.pack_into("<I", f, 36, 1)
    struct.pack_into("<I", f, 40, total)
    struct.pack_into("<I", f, 44, len(root))
    struct.pack_into("<I", f, 48, 0)
    struct.pack_into("<I", f, 52, root_index_page)
    for i, p in enumerate(root_pages):
        struct.pack_into("<I", f, root_index_page * PAGE + 4 * i, p)
    for i, p in enumerate(root_pages):
        chunk = root[i * PAGE:(i + 1) * PAGE]
        f[p * PAGE:p * PAGE + len(chunk)] = chunk
    for s, bl in zip(streams, blocks):
        for i, p in enumerate(bl):
            chunk = s[i * PAGE:(i + 1) * PAGE]
            f[p * PAGE:p * PAGE + len(chunk)] = chunk
    return bytes(f)


def make_malformed(base, outdir):
    def w32(b, off, v): struct.pack_into("<I", b, off, v)
    def r32(b, off):    return struct.unpack_from("<I", b, off)[0]
    def emit(name, data): open(os.path.join(outdir, "malformed-" + name), "wb").write(bytes(data))

    # 1. signature prefix only, no terminator anywhere
    emit("sig-prefix-only", base[:20])
    # 2. whole signature but no header behind it
    emit("sig-without-header", base[:32])
    # 3. page count that wraps 32bit multiplication with the page size
    b = bytearray(base); w32(b, 40, (len(base) // PAGE) + (1 << 23)); emit("pagecount-wraps", b)
    # 4. root index page outside the file
    b = bytearray(base); w32(b, 52, 0x00FFFFFF); emit("rootindexpage-outside-file", b)
    # 5. root directory bigger than one page of page numbers can describe
    b = bytearray(base); w32(b, 44, 0xFFFF00); emit("rootsize-huge", b)
    # 6. root page number outside the file
    b = bytearray(base); w32(b, r32(base, 52) * PAGE, 0x00FFFFFF); emit("rootpage-outside-file", b)

    root_page = r32(base, r32(base, 52) * PAGE) * PAGE
    num_streams = r32(base, root_page)
    # 7. more streams than the root directory can describe
    b = bytearray(base); w32(b, root_page, 0x0FFFFFFF); emit("streamcount-huge", b)
    # 8. stream size larger than the file
    b = bytearray(base); w32(b, root_page + 4, 0x7FFFFF00); emit("streamsize-huge", b)
    # 9. stream page number outside the file
    b = bytearray(base); w32(b, root_page + 4 * (1 + num_streams), 0x00FFFFFF)
    emit("streampage-outside-file", b)

    # The DBI stream is stream 3; find its first page through the directory.
    sizes = [r32(base, root_page + 4 + 4 * i) for i in range(num_streams)]
    pages, idx = [], root_page + 4 + 4 * num_streams
    for size in sizes:
        n = (size + PAGE - 1) // PAGE
        pages.append([r32(base, idx + 4 * i) for i in range(n)])
        idx += 4 * n
    dbi = pages[3][0] * PAGE
    dbi_size = sizes[3]

    # 10. debug header longer than the DBI stream
    b = bytearray(base); w32(b, dbi + 0x30, 0x7FFFFFF0); emit("dbghdr-longer-than-stream", b)
    # 11. module list longer than the DBI stream
    b = bytearray(base); w32(b, dbi + 0x18, 0x7FFFFFF0); emit("modulelist-longer-than-stream", b)
    # 12. module names that are never terminated
    b = bytearray(base)
    for off in range(dbi + 0x40 + 0x40, dbi + dbi_size):
        b[off] = 0x41
    emit("module-names-unterminated", b)
    # 13. symbol stream numbers that name no stream
    b = bytearray(base); struct.pack_into("<H", b, dbi + 0x0C, 0xFFFF); emit("gsi-stream-missing", b)
    # 14. section header stream number that names no stream
    b = bytearray(base)
    struct.pack_into("<H", b, dbi + dbi_size - 22 + 10, 0x7FFF); emit("sections-stream-missing", b)
    # 15. module stream number that names no stream
    b = bytearray(base); struct.pack_into("<H", b, dbi + 0x40 + 0x22, 0x7FFE); emit("module-stream-missing", b)

    # The last two are whole files rather than mutations of the good one: they
    # need a symbol record stream of their own, which the good file does not
    # have room for. Both walk the record chain in the symbol stream, where the
    # record length is the file's word for how far the next record is.

    # 16. a record header that straddles the end of its stream. The chain is
    # laid out so the last record starts two bytes before the end, and the
    # stream is scattered over its pages so it is copied into an allocation
    # that ends exactly where the stream does -- reading the four byte header
    # there runs off it.
    recs = bytearray(2 * PAGE)
    recs[0:4] = u16(4) + u16(0)               # CV signature, read as a span of 6
    pos = 6
    while pos < 2 * PAGE - 2:
        struct.pack_into("<HH", recs, pos, 2, 0)
        pos += 4
    assert pos == 2 * PAGE - 2
    struct.pack_into("<H", recs, pos, 2)      # header runs two bytes past the end
    emit("symrec-past-stream-end", build(sym_records=bytes(recs), scatter={6}))

    # 17. an odd record length, which puts the next record on an odd offset of
    # the stream. The record structures are what the file says they are, so
    # they may start anywhere; reading one through a pointer that expects to be
    # aligned is undefined behaviour.
    recs = bytearray(PAGE)
    recs[0:4] = u16(3) + u16(0)               # span of 5, so the next record is at 5
    struct.pack_into("<HH", recs, 5, 2, 0)
    emit("symrec-odd-length", build(sym_records=bytes(recs)))


if __name__ == "__main__":
    fixtures, crashes = sys.argv[1], sys.argv[2]
    os.makedirs(fixtures, exist_ok=True)
    os.makedirs(crashes, exist_ok=True)
    good = build()
    open(os.path.join(fixtures, "minimal.pdb"), "wb").write(good)
    open(os.path.join(fixtures, "minimal-nonlinear.pdb"), "wb").write(build(nonlinear=True))
    make_malformed(bytearray(good), crashes)
