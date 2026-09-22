#!/bin/bash
set -e

# Resolve project root from script location (Scripts/../)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo ">>> Project root: $PROJECT_ROOT"
cd "$PROJECT_ROOT"

VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"

# Install vcpkg packages to a space-free path inside WSL's native filesystem
VCPKG_SAFE_ROOT="$HOME/.vcpkg-installed/net-tfg-engine"
mkdir -p "$VCPKG_SAFE_ROOT"

echo ">>> Installing dependencies via vcpkg..."
"$VCPKG_ROOT/vcpkg" install \
    --triplet x64-linux \
    --x-install-root="$VCPKG_SAFE_ROOT"

# Recreate symlink from project vcpkg_installed to space-free path
mkdir -p "$PROJECT_ROOT/vcpkg_installed"
rm -rf "$PROJECT_ROOT/vcpkg_installed/x64-linux"
ln -s "$VCPKG_SAFE_ROOT/x64-linux" "$PROJECT_ROOT/vcpkg_installed/x64-linux"
echo ">>> Symlink: vcpkg_installed/x64-linux -> $VCPKG_SAFE_ROOT/x64-linux"

# AssetsPackager.py (post-build step) compresses assets with Zstd and needs this Python module
if ! python3 -c "import zstandard" 2>/dev/null; then
    echo ">>> Installing Python 'zstandard' module for the asset packager..."
    python3 -m pip install --user zstandard || {
        echo "!!! Could not install 'zstandard'. Install it manually (e.g. 'sudo apt install python3-zstandard') and rerun."
        exit 1
    }
fi

# Generate makefiles
echo ">>> Generating makefiles..."
Vendor/Binaries/Premake/Linux/premake5 --cc=clang --file=Build.lua gmake2

# Clean entire linux build output to avoid mkdir failures on existing dirs
echo ">>> Cleaning stale build output..."
rm -rf "$PROJECT_ROOT/Binaries/linux-x86_64"
rm -rf "$PROJECT_ROOT/Binaries/Intermediates/linux-x86_64"

# Compile only the real projects, not LinuxBuild (which would cause recursion)
echo ">>> Compiling..."
make -j$(nproc) config=release NetTFGEngine GameClient GameServer

# Copy shared libraries (.so) to output folder so executables can find them
echo ">>> Copying shared libraries..."
OUTPUT_DIR="$PROJECT_ROOT/Binaries/linux-x86_64/Release"
find "$VCPKG_SAFE_ROOT/x64-linux/lib" -name "*.so*" -exec cp -P {} "$OUTPUT_DIR/" \;

echo ">>> Done!"