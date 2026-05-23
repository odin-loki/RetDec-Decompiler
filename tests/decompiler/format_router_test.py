#!/usr/bin/env python3
"""
Unit tests for managed format detection (detectManagedFormatFromBytes).

Runs against a pure-Python reference implementation of the detection rules.
When format_router_probe is available (built target or FORMAT_ROUTER_PROBE env),
each case is cross-checked against the C++ probe.

Usage:
  python3 format_router_test.py [path/to/format_router_probe]
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import unittest


# ManagedFormat enum order must match managed_decompiler.h
FORMAT_NAMES = {
    0: "Unknown",
    1: "JavaClass",
    2: "JavaJar",
    3: "Dex",
    4: "Apk",
    5: "PythonPyc",
    6: "LuaBytecode",
    7: "Wasm",
    8: "CliAssembly",
}


def _le16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def _le32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def _is_zip_local_header(data: bytes) -> bool:
    return (
        len(data) >= 4
        and data[0] == 0x50
        and data[1] == 0x4B
        and data[2] in (0x03, 0x05, 0x07)
        and data[3] in (0x04, 0x06, 0x08)
    )


def _ends_with(name: str, suffix: str) -> bool:
    return len(name) >= len(suffix) and name.endswith(suffix)


def _is_classes_dex_entry(name: str) -> bool:
    if name == "classes.dex":
        return True
    if len(name) > 10 and name.startswith("classes") and name.endswith(".dex"):
        mid = name[8:-4]
        return mid.isdigit()
    return False


def _probe_zip_managed_kind(data: bytes) -> str | None:
    if len(data) < 22:
        return None
    eocd_offset = len(data) - 22
    while eocd_offset > 0:
        if _le32(data, eocd_offset) == 0x06054B50:
            break
        eocd_offset -= 1
    if _le32(data, eocd_offset) != 0x06054B50:
        return None

    cd_offset = _le32(data, eocd_offset + 16)
    cd_count = _le16(data, eocd_offset + 10)
    has_class = has_dex = False
    pos = cd_offset
    for _ in range(cd_count):
        if pos + 46 > len(data) or _le32(data, pos) != 0x02014B50:
            break
        fn_len = _le16(data, pos + 28)
        extra_len = _le16(data, pos + 30)
        comment_len = _le16(data, pos + 32)
        if pos + 46 + fn_len <= len(data):
            name = data[pos + 46 : pos + 46 + fn_len].decode("utf-8", errors="replace")
            if _ends_with(name, ".class"):
                has_class = True
            if _is_classes_dex_entry(name):
                has_dex = True
        pos += 46 + fn_len + extra_len + comment_len

    if has_dex:
        return "Apk"
    if has_class:
        return "JavaJar"
    return None


def _probe_cli_assembly(data: bytes) -> bool:
    if len(data) < 0x40 or data[0:2] != b"MZ":
        return False
    pe_off = _le32(data, 0x3C)
    if pe_off + 24 > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        return False
    opt_size = _le16(data, pe_off + 20)
    opt = pe_off + 24
    if opt + 2 > len(data):
        return False
    magic = _le16(data, opt)
    if magic not in (0x010B, 0x020B):
        return False
    is64 = magic == 0x020B
    dd_start = opt + (112 if is64 else 96)
    com_off = dd_start + 14 * 8
    if com_off + 8 > len(data):
        return False
    rva = _le32(data, com_off)
    size = _le32(data, com_off + 4)
    return rva != 0 and size >= 72


def _is_java_class_magic(data: bytes) -> bool:
    if len(data) < 8 or data[0:4] != b"\xCA\xFE\xBA\xBE":
        return False
    major = (data[6] << 8) | data[7]
    return 44 <= major <= 70


def detect_managed_format_from_bytes_ref(data: bytes) -> int:
    """Python mirror of detectManagedFormatFromBytes (managed_decompiler.cpp)."""
    if len(data) < 4:
        return 0
    if _is_java_class_magic(data):
        return 1
    if data[0:4] == b"dex\n":
        return 3
    if data[0:4] == b"\x00asm":
        return 7
    if data[0:4] == b"\x1bLua":
        return 6
    if data[2:4] == b"\x0d\x0a":
        magic_word = data[0] | (data[1] << 8)
        if magic_word != 0:
            return 5
    if _is_zip_local_header(data):
        kind = _probe_zip_managed_kind(data)
        if kind == "Apk":
            return 4
        if kind == "JavaJar":
            return 2
    if len(data) >= 2 and data[0:2] == b"MZ" and _probe_cli_assembly(data):
        return 8
    return 0


OUTPUT_LANG = {
    1: "java", 2: "java", 3: "java", 4: "java",
    5: "python", 6: "lua", 7: "wat", 8: "csharp",
}


def _build_minimal_zip_central(*entry_names: str) -> bytes:
    """Build a minimal ZIP with central-directory entries only (STORED, zero payload)."""
    local_parts = bytearray()
    central = bytearray()
    offset = 0
    for name in entry_names:
        name_b = name.encode("utf-8")
        # local file header + empty payload
        local = bytearray()
        local += (0x04034B50).to_bytes(4, "little")
        local += (20).to_bytes(2, "little")  # version needed
        local += (0).to_bytes(2, "little")   # flags
        local += (0).to_bytes(2, "little")   # method stored
        local += (0).to_bytes(2, "little")   # mod time
        local += (0).to_bytes(2, "little")   # mod date
        local += (0).to_bytes(4, "little")   # crc
        local += (0).to_bytes(4, "little")   # comp size
        local += (0).to_bytes(4, "little")   # uncomp size
        local += len(name_b).to_bytes(2, "little")
        local += (0).to_bytes(2, "little")   # extra len
        local += name_b
        local_parts += local
        local_offset = offset
        offset += len(local)

        cd = bytearray()
        cd += (0x02014B50).to_bytes(4, "little")
        cd += (20).to_bytes(2, "little")
        cd += (20).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(4, "little")
        cd += (0).to_bytes(4, "little")
        cd += (0).to_bytes(4, "little")
        cd += len(name_b).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(2, "little")
        cd += (0).to_bytes(4, "little")
        cd += local_offset.to_bytes(4, "little")
        cd += name_b
        central += cd

    eocd = bytearray()
    eocd += (0x06054B50).to_bytes(4, "little")
    eocd += (0).to_bytes(2, "little")
    eocd += (0).to_bytes(2, "little")
    eocd += len(entry_names).to_bytes(2, "little")
    eocd += len(entry_names).to_bytes(2, "little")
    eocd += len(central).to_bytes(4, "little")
    eocd += (len(local_parts)).to_bytes(4, "little")
    eocd += (0).to_bytes(2, "little")

    return bytes(local_parts) + bytes(central) + bytes(eocd)


def _build_pe_with_cli_dir(has_cli: bool) -> bytes:
    """Minimal PE32 with optional COM descriptor directory entry."""
    buf = bytearray(512)
    buf[0:2] = b"MZ"
    pe_off = 0x80
    buf[0x3C:0x40] = pe_off.to_bytes(4, "little")
    buf[pe_off:pe_off + 4] = b"PE\x00\x00"
    buf[pe_off + 4:pe_off + 6] = (0x014C).to_bytes(2, "little")  # i386
    buf[pe_off + 6:pe_off + 8] = (0).to_bytes(2, "little")       # sections
    opt_size = 0xE0
    buf[pe_off + 20:pe_off + 22] = opt_size.to_bytes(2, "little")
    opt = pe_off + 24
    buf[opt:opt + 2] = (0x010B).to_bytes(2, "little")  # PE32 magic
    dd_start = opt + 96
    com_off = dd_start + 14 * 8
    if has_cli:
        buf[com_off:com_off + 4] = (0x2000).to_bytes(4, "little")  # rva
        buf[com_off + 4:com_off + 8] = (72).to_bytes(4, "little")   # size
    return bytes(buf)


class FormatRouterProbe:
    def __init__(self, probe_path: str | None):
        self.probe_path = probe_path

    def detect(self, data: bytes) -> tuple[int, str, str]:
        fmt_id = detect_managed_format_from_bytes_ref(data)
        lang = OUTPUT_LANG.get(fmt_id, "")
        name = FORMAT_NAMES.get(fmt_id, "Unknown")
        if self.probe_path:
            hex_str = data.hex()
            result = subprocess.run(
                [self.probe_path, hex_str],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            if result.returncode == 0:
                parts = result.stdout.strip().split("\t")
                cpp_id = int(parts[0])
                if cpp_id != fmt_id:
                    raise AssertionError(
                        f"C++ probe mismatch: py={fmt_id} cpp={cpp_id} ({parts[1] if len(parts)>1 else '?'})"
                    )
        return fmt_id, name, lang


def resolve_probe_path() -> str | None:
    if len(sys.argv) > 1:
        return sys.argv[1]
    env = os.environ.get("FORMAT_ROUTER_PROBE")
    if env and os.path.isfile(env):
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("format_router_probe", "format_router_probe.exe"):
        candidate = os.path.join(here, name)
        if os.path.isfile(candidate):
            return candidate
    return None


class FormatRouterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = FormatRouterProbe(resolve_probe_path())

    def assertFormat(self, data: bytes, expected_id: int, expected_lang: str = ""):
        fmt_id, name, lang = self.probe.detect(data)
        self.assertEqual(fmt_id, expected_id, msg=f"got {name}")
        if expected_lang:
            self.assertEqual(lang, expected_lang)

    def test_java_class_magic(self):
        # CAFEBABE + minor 0 + major 52 (Java 8)
        data = bytes([0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34])
        self.assertFormat(data, 1, "java")

    def test_dex_magic(self):
        self.assertFormat(b"dex\n", 3, "java")

    def test_wasm_magic(self):
        self.assertFormat(b"\x00asm", 7, "wat")

    def test_lua_magic(self):
        self.assertFormat(b"\x1bLua", 6, "lua")

    def test_python_pyc_magic(self):
        self.assertFormat(bytes([0x55, 0x0D, 0x0D, 0x0A]), 5, "python")

    def test_jar_zip_with_class(self):
        data = _build_minimal_zip_central("com/example/Foo.class")
        self.assertFormat(data, 2, "java")

    def test_apk_zip_with_dex(self):
        data = _build_minimal_zip_central("classes.dex")
        self.assertFormat(data, 4, "java")

    def test_zip_without_managed_entries(self):
        data = _build_minimal_zip_central("README.txt")
        self.assertFormat(data, 0)

    def test_pe_without_cli(self):
        data = _build_pe_with_cli_dir(has_cli=False)
        self.assertFormat(data, 0)

    def test_pe_with_cli_directory(self):
        data = _build_pe_with_cli_dir(has_cli=True)
        self.assertFormat(data, 8, "csharp")

    def test_empty_buffer(self):
        self.assertFormat(b"", 0)

    def test_too_small(self):
        self.assertFormat(b"\x00\x01", 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
