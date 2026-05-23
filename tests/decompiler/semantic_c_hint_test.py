#!/usr/bin/env python3
"""Validate cHint field in semanticDetections when outputLang is C."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

# Reuse base schema checks from sibling test module.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from semantic_detection_json_test import (  # noqa: E402
    REQUIRED_DETECTION_KEYS,
    validate_config,
    validate_function_semantic_detections,
)

KNOWN_C_HINTS = {
    "vector_like_3ptr",
    "list_like_dllist",
    "map_like_rbtree",
    "set_like_rbtree",
    "unordered_map_like_hash",
    "unordered_set_like_hash",
    "string_like_sso",
    "shared_ptr_like_2ptr",
    "unique_ptr_like_1ptr",
    "weak_ptr_like_2ptr",
    "deque_like_chunked",
    "array_like_fixed",
}


def validate_c_hint_detections(config: dict) -> list[str]:
    errors: list[str] = []
    output_lang = (
        config.get("decompParams", {}).get("outputLang")
        or config.get("parameters", {}).get("outputLang")
        or "c"
    )
    if output_lang not in ("c", ""):
        return errors

    functions = config.get("functions", [])
    if not isinstance(functions, list):
        return errors

    for fn in functions:
        if not isinstance(fn, dict):
            continue
        name = fn.get("name", "<unnamed>")
        dets = fn.get("semanticDetections")
        if not isinstance(dets, list):
            continue
        for i, item in enumerate(dets):
            if not isinstance(item, dict):
                continue
            if item.get("kind") != "container":
                continue
            c_hint = item.get("cHint")
            if c_hint is None:
                errors.append(
                    f"{name}: semanticDetections[{i}] container missing cHint "
                    f"(outputLang={output_lang!r})"
                )
                continue
            if not isinstance(c_hint, str) or not c_hint:
                errors.append(
                    f"{name}: semanticDetections[{i}].cHint must be a non-empty string"
                )
                continue
            if c_hint not in KNOWN_C_HINTS:
                errors.append(
                    f"{name}: semanticDetections[{i}].cHint unknown value {c_hint!r}"
                )
    return errors


def validate_config_with_c_hints(config: dict) -> list[str]:
    errors = validate_config(config)
    errors.extend(validate_c_hint_detections(config))
    return errors


MOCK_C_CONFIG = {
    "date": "2026-05-23",
    "time": "12:00:00",
    "decompParams": {"outputLang": "c"},
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
                    "cHint": "vector_like_3ptr",
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

MOCK_CPP_CONFIG = {
    **MOCK_C_CONFIG,
    "decompParams": {"outputLang": "cpp"},
    "functions": [
        {
            **MOCK_C_CONFIG["functions"][0],
            "semanticDetections": [
                {
                    "kind": "container",
                    "label": "std::vector<int32_t>",
                    "confidence": 0.87,
                    "detail": "three-pointer layout; growth x2",
                }
            ],
        }
    ],
}


class SemanticCHintTest(unittest.TestCase):
    def test_c_output_requires_c_hint_on_containers(self):
        errors = validate_config_with_c_hints(MOCK_C_CONFIG)
        self.assertEqual(errors, [])

    def test_cpp_output_does_not_require_c_hint(self):
        errors = validate_c_hint_detections(MOCK_CPP_CONFIG)
        self.assertEqual(errors, [])

    def test_rejects_unknown_c_hint(self):
        bad = json.loads(json.dumps(MOCK_C_CONFIG))
        bad["functions"][0]["semanticDetections"][0]["cHint"] = "not_a_real_hint"
        errors = validate_c_hint_detections(bad)
        self.assertTrue(any("unknown value" in e for e in errors))

    def test_rejects_empty_c_hint(self):
        bad = json.loads(json.dumps(MOCK_C_CONFIG))
        bad["functions"][0]["semanticDetections"][0]["cHint"] = ""
        errors = validate_c_hint_detections(bad)
        self.assertTrue(any("non-empty string" in e for e in errors))

    def test_c_hint_roundtrip_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.config.json"
            path.write_text(json.dumps(MOCK_C_CONFIG, indent=2), encoding="utf-8")
            loaded = json.loads(path.read_text(encoding="utf-8"))
            det = loaded["functions"][0]["semanticDetections"][0]
            self.assertEqual(det["cHint"], "vector_like_3ptr")
            self.assertNotIn("cHint", loaded["functions"][0]["semanticDetections"][1])
            errors = validate_config_with_c_hints(loaded)
            self.assertEqual(errors, [])

    def test_required_keys_unchanged_with_c_hint(self):
        det = MOCK_C_CONFIG["functions"][0]["semanticDetections"][0]
        missing = REQUIRED_DETECTION_KEYS - det.keys()
        self.assertEqual(missing, set())


if __name__ == "__main__":
    unittest.main(verbosity=2)
