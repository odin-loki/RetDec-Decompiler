#!/usr/bin/env python3
"""Tests for the one-line summary of a failing decompiler run.

The gate reported `extract: <name> rc=-6: <last three lines>`, and for an LLVM
assertion the last three lines are "Stack dump:", the program arguments, and
the pass name. The sentence naming the file, the line and the failed condition
is the FIRST line, and it was the one thrown away -- so the only red step in
`ctest-linux` said a binary aborted and never said why.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from extract_decompiler_predictions import summarise_failure_output  # noqa: E402

# What the decompiler actually printed for generated_quicksort-gcc-O0, with the
# frames elided.
LLVM_ASSERT_CRASH = """\
retdec-decompiler: src/bin2llvmir/analyses/reaching_definitions.cpp:248: void \
retdec::bin2llvmir::ReachingDefinitionsAnalysis::initializeBasicBlocksPrev(): \
Assertion `p != pair1.second.end() && "we should have all BBs stored in bbMap"' failed.
PLEASE submit a bug report and include the crash backtrace.
Stack dump:
0.\tProgram arguments: retdec-decompiler corpus/generated_quicksort-gcc-O0 --output out.c
1.\tRunning pass 'LLVM instruction optimization using RDA' on module ''.
 #0 0x00 llvm::sys::PrintStackTrace(llvm::raw_ostream&, int)
 #1 0x00 llvm::sys::RunSignalHandlers()
 #2 0x00 SignalHandler(int)
 #3 0x00 __restore_rt
 #4 0x00 pthread_kill
 #5 0x00 raise
 #6 0x00 abort
"""


def main() -> int:
    summary = summarise_failure_output(LLVM_ASSERT_CRASH)

    # The half that says what went wrong.
    assert "reaching_definitions.cpp:248" in summary, summary
    assert "we should have all BBs stored in bbMap" in summary, summary

    # And the half that says where in the pipeline.
    assert "LLVM instruction optimization using RDA" in summary, summary

    # One line, whatever the input was.
    assert "\n" not in summary, summary

    # The middle is elided rather than dropped silently.
    assert "more line(s)" in summary, summary

    # Short output is kept whole.
    assert summarise_failure_output("boom") == "boom"
    assert summarise_failure_output("a\nb\nc") == "a | b | c"

    # Blank lines do not eat the budget, and no output says so.
    assert summarise_failure_output("\n\n  first  \n\n\nsecond\n\n") == "first | second"
    assert summarise_failure_output("") == "no output"
    assert summarise_failure_output("   \n  \n") == "no output"

    # A crash whose message is only at the tail still survives.
    tail_only = "\n".join([f"line {i}" for i in range(40)] + ["Segmentation fault"])
    assert "Segmentation fault" in summarise_failure_output(tail_only)

    print("failure summary tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
