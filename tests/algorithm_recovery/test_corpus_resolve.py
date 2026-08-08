#!/usr/bin/env python3
"""Tests for corpus binary path resolution (Windows .exe)."""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from extract_decompiler_predictions import resolve_corpus_binary  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        corpus = Path(td)
        (corpus / "sample-gcc-O0.exe").write_bytes(b"\x7fELF")
        assert resolve_corpus_binary(corpus, "sample-gcc-O0") is not None
        (corpus / "plain").write_bytes(b"\x7fELF")
        assert resolve_corpus_binary(corpus, "plain") is not None
        assert resolve_corpus_binary(corpus, "missing") is None
    print("corpus binary resolution tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
