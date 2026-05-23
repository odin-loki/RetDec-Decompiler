#!/usr/bin/env python3
"""Unit tests for retdec_export_intel.py using mock config artifacts."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from retdec_export_intel import build_intel_bundle  # noqa: E402


MOCK_CONFIG = {
    "functions": [
        {
            "name": "main",
            "startAddr": "0x401000",
            "endAddr": "0x401050",
            "startLine": "10",
            "endLine": "25",
        },
        {
            "name": "helper",
            "startAddr": "0x401060",
            "endAddr": "0x4010a0",
        },
    ],
    "globals": [
        {
            "name": "g_banner",
            "realName": "Hello, RetDec!",
            "type": {"llvmIr": "i8*"},
        },
        {
            "name": "g_flag",
            "realName": "DEBUG",
            "type": {"llvmIr": "i8*"},
        },
    ],
    "patterns": [
        {
            "name": "crypto_aes",
            "description": "AES S-box pattern",
            "confidence": "high",
        }
    ],
    "tools": [{"name": "retdec-decompiler", "version": "test"}],
}


class IntelExportTest(unittest.TestCase):
    def test_build_bundle_from_mock_config(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config_path = root / "sample.config.json"
            c_path = root / "sample.c"
            binary_path = root / "sample.bin"

            config_path.write_text(json.dumps(MOCK_CONFIG), encoding="utf-8")
            c_path.write_text("int main(){ return 0; }\n", encoding="utf-8")
            binary_path.write_bytes(b"mock-binary-payload")

            bundle = build_intel_bundle(
                c_path=c_path,
                config_path=config_path,
                binary_path=binary_path,
                strings_sample_limit=8,
            )

        self.assertEqual(bundle["type"], "bundle")
        self.assertEqual(bundle["x_retdec_export_version"], "1.0")
        self.assertIn("x_retdec_generated", bundle)
        self.assertEqual(len(bundle["objects"]), 2)

        file_obj = bundle["objects"][0]
        self.assertEqual(file_obj["type"], "file")
        self.assertEqual(file_obj["x_retdec_function_count"], 2)
        self.assertIn("SHA-256", file_obj["hashes"])
        self.assertGreaterEqual(len(file_obj["x_retdec_strings_sample"]), 1)
        self.assertTrue(any("Hello" in s for s in file_obj["x_retdec_strings_sample"]))

        detections = file_obj["x_retdec_semantic_detections"]
        self.assertTrue(any(d.get("name") == "crypto_aes" for d in detections))
        self.assertTrue(any(d.get("kind") == "tool" for d in detections))

        indicator = bundle["objects"][1]
        self.assertEqual(indicator["type"], "indicator")
        self.assertEqual(indicator["x_retdec_function_count"], 2)
        self.assertEqual(indicator["x_retdec_string_count"], 2)

    def test_fileinfo_semantic_merge(self) -> None:
        fileinfo = {
            "compiler": {"tool": "gcc", "version": "11"},
            "languages": [{"name": "C"}],
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fi_path = root / "info.json"
            fi_path.write_text(json.dumps(fileinfo), encoding="utf-8")
            bundle = build_intel_bundle(fileinfo_path=fi_path)

        file_obj = bundle["objects"][0]
        kinds = {d.get("category") for d in file_obj["x_retdec_semantic_detections"]}
        self.assertIn("compiler", kinds)


if __name__ == "__main__":
    unittest.main()
