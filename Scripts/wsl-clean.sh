#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[wsl-clean] Project root: $PROJECT_ROOT"
cd "$PROJECT_ROOT"

# arm64: the makefiles on disk are the x86_64 ones, so remove the ARM64 output folders directly
if [ "$1" = "arm64" ]; then
    rm -rf "$PROJECT_ROOT/Binaries/linux-ARM64" "$PROJECT_ROOT/Binaries/Intermediates/linux-ARM64"
    exit 0
fi

make clean 2>/dev/null || true