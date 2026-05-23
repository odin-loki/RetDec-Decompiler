#!/usr/bin/env python3
"""Validate semanticDetections JSON schema in decompiler config sidecars."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


REQUIRED_DETECTION_KEYS = {"kind", "label", "confidence", "detail"}


def validate_function_semantic_detections(fn_obj: dict) -> list[str]:
    errors: list[str] = []
    name = fn_obj.get("name", "<unnamed>")
    dets = fn_obj.get("semanticDetections")
    if dets is None:
        return errors
    if not isinstance(dets, list):
        errors.append(f"{name}: semanticDetections must be an array")
        return errors
    for i, item in enumerate(dets):
        if not isinstance(item, dict):
            errors.append(f"{name}: semanticDetections[{i}] must be an object")
            continue
        missing = REQUIRED_DETECTION_KEYS - item.keys()
        if missing:
            errors.append(
                f"{name}: semanticDetections[{i}] missing keys {sorted(missing)}"
            )
        conf = item.get("confidence")
        if conf is not None and not isinstance(conf, (int, float)):
            errors.append(f"{name}: semanticDetections[{i}].confidence must be numeric")
    return errors


def validate_config(config: dict) -> list[str]:
    errors: list[str] = []
    functions = config.get("functions")
    if functions is None:
        errors.append("config missing functions array")
        return errors
    if not isinstance(functions, list):
        errors.append("functions must be an array")
        return errors
    for fn in functions:
        if isinstance(fn, dict):
            errors.extend(validate_function_semantic_detections(fn))
    return errors


MOCK_CONFIG = {
    "date": "2026-05-23",
    "time": "12:00:00",
    "decompParams": {},
    "functions": [
        {
            "name": "function_401000",
            "fncType": "decompilerDefined",
            "callingConvention": "cdecl",
            "startLine": "42",
            "semanticDetections": [
                {
                    "kind": "container",
                    "label": "std::vector<int32_t>",
                    "confidence": 0.87,
                    "detail": "three-pointer layout; growth x2",
                },
                {
                    "kind": "sort",
                    "label": "introsort",
                    "confidence": 0.91,
                    "detail": "partition + insertion tail",
                },
            ],
        }
    ],
}


class SemanticDetectionJsonTest(unittest.TestCase):
    def test_mock_schema_valid(self):
        errors = validate_config(MOCK_CONFIG)
        self.assertEqual(errors, [])

    def test_mock_file_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.config.json"
            path.write_text(json.dumps(MOCK_CONFIG, indent=2), encoding="utf-8")
            loaded = json.loads(path.read_text(encoding="utf-8"))
            fn = loaded["functions"][0]
            self.assertIn("semanticDetections", fn)
            self.assertEqual(fn["semanticDetections"][0]["kind"], "container")
            errors = validate_config(loaded)
            self.assertEqual(errors, [])

    def test_rejects_invalid_confidence_type(self):
        bad = json.loads(json.dumps(MOCK_CONFIG))
        bad["functions"][0]["semanticDetections"][0]["confidence"] = "high"
        errors = validate_config(bad)
        self.assertTrue(any("confidence must be numeric" in e for e in errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
