#!/usr/bin/env bash
# Placeholder for a real coding agent. Replace with your tool, for example:
#   - a wrapper around Cursor / Claude Code / Aider / custom LLM script
#   - a script that runs targeted tests and edits files until green
#
# Contract: argument $1 = Taskmaster task id; exit 0 only when the task is finished.

set -euo pipefail

TASK_ID="${1:-}"
echo "example-coding-agent.sh: task $TASK_ID — replace this script with your real agent." >&2
echo "It must exit 0 only after the task is implemented and verified." >&2
exit 1
