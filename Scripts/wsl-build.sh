#!/bin/bash
# Called by Visual Studio via: wsl.exe bash Scripts/wsl-build.sh
# Instead of receiving the path as argument (which breaks with spaces and trailing backslash),
# we resolve the project root from the script's own location inside WSL.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[wsl-build] Project root: $PROJECT_ROOT"
cd "$PROJECT_ROOT"
bash Scripts/Setup-Linux.sh