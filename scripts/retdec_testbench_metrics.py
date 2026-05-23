#!/usr/bin/env python3

"""Metrics helpers for RetDec full testbench."""

from __future__ import annotations

import re
from typing import Iterable, Set


_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_FUNC_RE = re.compile(
    r"^\s*(?:[A-Za-z_][\w\s\*\(\)]*?)\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*\{",
    re.MULTILINE,
)
_COMMENT_LINE_RE = re.compile(r"//.*?$", re.MULTILINE)
_COMMENT_BLOCK_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(code: str) -> str:
    """Remove C/C++ line and block comments."""
    no_block = _COMMENT_BLOCK_RE.sub("", code)
    return _COMMENT_LINE_RE.sub("", no_block)


def count_functions_c_like(code: str) -> int:
    """Roughly count top-level C-like function definitions."""
    cleaned = strip_comments(code)
    return len(_FUNC_RE.findall(cleaned))


def function_names(code: str) -> Set[str]:
    """Extract function names from decompiled C-like source."""
    cleaned = strip_comments(code)
    return {m.group(1).lower() for m in _FUNC_RE.finditer(cleaned)}


def identifiers(code: str) -> Set[str]:
    """Extract identifier set for lexical similarity metrics."""
    cleaned = strip_comments(code)
    return {m.group(0).lower() for m in _IDENT_RE.finditer(cleaned)}


def jaccard(a: Iterable[str], b: Iterable[str]) -> float:
    """Compute Jaccard similarity over two token collections."""
    sa = set(a)
    sb = set(b)
    if not sa and not sb:
        return 1.0
    union = sa | sb
    if not union:
        return 0.0
    return len(sa & sb) / len(union)

