#!/usr/bin/env python3
"""Sample RetDec post-decompile plugin — append banner to .c output.

Usage:
    python3 post_process_output.py FILE.c [--marker TEXT]
"""

from __future__ import annotations

import argparse
import os
import sys
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="RetDec sample post-process plugin")
    parser.add_argument("c_file", type=Path, help="Decompiled C output to modify in place")
    parser.add_argument(
        "--marker",
        default="retdec-plugin-sample",
        help="Marker string embedded in the banner comment",
    )
    args = parser.parse_args()

    path = args.c_file.resolve()
    if not path.is_file():
        print(f"Error: file not found: {path}", file=sys.stderr)
        return 1

    text = path.read_text(encoding="utf-8", errors="replace")
    banner = (
        f"/* RetDec post-process plugin ({args.marker})\n"
        f" * processed: {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}\n"
        f" */\n"
    )

    if args.marker in text.splitlines()[0:5]:
        if os.environ.get("RETDEC_PLUGIN_POST"):
            print(f"Skip: banner already present in {path}", file=sys.stderr)
        return 0

    path.write_text(banner + text, encoding="utf-8")

    if os.environ.get("RETDEC_PLUGIN_POST"):
        print(f"[retdec-plugin-sample] post-processed {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
