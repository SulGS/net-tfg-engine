#!/bin/bash
# Builds net-tfg-engine for linux-ARM64 inside an ARM64 Docker container (emulated with QEMU on x86_64 hosts).
# Creates whatever is missing on first use: QEMU registration, the builder image and its cache volume.
# Run from WSL or any Linux with Docker:  bash Scripts/Build-Linux-ARM64.sh
# Output: Binaries/linux-ARM64/Release/{GameClient,GameServer,Matchmaking}
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE=net-tfg-arm64-builder
CACHE_VOLUME=net-tfg-arm64-cache   # /root: vcpkg binary cache, downloads and installed packages, survives between runs

echo ">>> Project root: $PROJECT_ROOT"

if ! command -v docker >/dev/null; then
    echo "!!! Docker is not installed: sudo apt install docker.io && sudo usermod -aG docker \$USER, then restart WSL"
    exit 1
fi

# Register QEMU for arm64 binaries on non-ARM hosts (needed again after each WSL restart)
if [ "$(uname -m)" != "aarch64" ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    echo ">>> Registering QEMU arm64 emulation..."
    docker run --privileged --rm tonistiigi/binfmt --install arm64
fi

# Builds the image the first time; later runs reuse every cached layer and take seconds.
# The Dockerfile needs no build context (the project is mounted at run time), so it is fed through stdin.
echo ">>> Building image (only slow the first time)..."
docker build --platform linux/arm64 -t "$IMAGE" - < "$SCRIPT_DIR/Dockerfile.arm64"

echo ">>> Compiling (the first run builds every vcpkg dependency under emulation: expect several hours)..."
# BUILD_JOBS=N bash Build-Linux-ARM64.sh  ->  overrides the image's default of 6 parallel jobs
docker run --rm --platform linux/arm64 \
    ${BUILD_JOBS:+-e BUILD_JOBS="$BUILD_JOBS"} \
    -v "$PROJECT_ROOT":/src \
    -v "$CACHE_VOLUME":/root \
    "$IMAGE"

echo ">>> Done: $PROJECT_ROOT/Binaries/linux-ARM64/Release"
