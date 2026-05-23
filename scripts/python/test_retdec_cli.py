#!/usr/bin/env python3
"""Unit tests for retdec_cli helpers (no decompiler required)."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import retdec_cli  # noqa: E402
from retdec_testbench_metrics import function_names, jaccard  # noqa: E402


class MetricsTests(unittest.TestCase):
    def test_function_names_and_jaccard(self) -> None:
        left = "int foo(void) { return 0; }\nint bar(void) { return 1; }"
        right = "int foo(void) { return 0; }\nint baz(void) { return 2; }"
        la = function_names(left)
        rb = function_names(right)
        self.assertEqual(la, {"foo", "bar"})
        self.assertAlmostEqual(jaccard(la, rb), 1 / 3)


class CliDiffTests(unittest.TestCase):
    def test_diff_c_files_report(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            left = tmp_path / "a.c"
            right = tmp_path / "b.c"
            left.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            right.write_text(
                "int main(void) { return 0; }\nint helper(void) { return 1; }\n",
                encoding="utf-8",
            )
            report = retdec_cli._diff_c_files(left, right)
            self.assertEqual(report["mode"], "c_files")
            self.assertIn("helper", report["added_function_names"])
            self.assertGreater(report["function_name_jaccard"], 0.0)


class ManifestTests(unittest.TestCase):
    def test_load_json_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "m.json"
            manifest.write_text(
                json.dumps({"inputs": [{"path": "/x", "output": "/y.c"}]}),
                encoding="utf-8",
            )
            items = retdec_cli._load_manifest(manifest)
            self.assertEqual(len(items), 1)
            self.assertEqual(items[0]["path"], "/x")


class HelpSmokeTests(unittest.TestCase):
    def test_help_exits_zero(self) -> None:
        with self.assertRaises(SystemExit) as ctx:
            retdec_cli.main(["--help"])
        self.assertEqual(ctx.exception.code, 0)

    def test_subcommand_help(self) -> None:
        for sub in ("batch", "diff", "emit-json", "watch"):
            with self.assertRaises(SystemExit) as ctx:
                retdec_cli.main([sub, "--help"])
            self.assertEqual(ctx.exception.code, 0)


if __name__ == "__main__":
    unittest.main()
