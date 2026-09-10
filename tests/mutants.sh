#!/usr/bin/env bash
# On-demand negative controls. The Python runner owns every child PID and temporary worktree.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 "$ROOT/tests/mutants.py" "$@"
