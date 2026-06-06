#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[wsl-clean] Project root: $PROJECT_ROOT"
cd "$PROJECT_ROOT"
make clean 2>/dev/null || true