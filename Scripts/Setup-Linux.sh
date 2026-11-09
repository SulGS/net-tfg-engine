#!/bin/bash
set -e

# Resolve project root from script location (Scripts/../)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo ">>> Project root: $PROJECT_ROOT"
cd "$PROJECT_ROOT"

VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"

# Target CPU: the host's by default, overridable with ARCH=x64|arm64
if [ -z "$ARCH" ]; then
    case "$(uname -m)" in
        aarch64|arm64) ARCH=arm64 ;;
        *)             ARCH=x64 ;;
    esac
fi
case "$ARCH" in
    x64)   TRIPLET=x64-linux;   BIN_ARCH=x86_64 ;;
    arm64) TRIPLET=arm64-linux; BIN_ARCH=ARM64 ;;
    *)     echo "!!! Unsupported ARCH '$ARCH' (use x64 or arm64)"; exit 1 ;;
esac
echo ">>> Target architecture: $ARCH (vcpkg triplet $TRIPLET)"

# SERVER_ONLY=1: headless machine. Builds only GameServer and Matchmaking (on NetTFGEngineCore) and installs the vcpkg
# packages without the "client" feature (no OpenGL/GLFW/OpenAL/FreeType/SOIL). This also removes the client packages from
# the shared install root, so a later full build reinstalls them.
if [ "$SERVER_ONLY" = "1" ]; then
    VCPKG_FEATURE_ARGS="--x-no-default-features"
    MAKE_PROJECTS="NetTFGEngineCore GameServer Matchmaking"
    APPS="GameServer Matchmaking"
    echo ">>> Server-only build (no graphics/audio dependencies)"
else
    VCPKG_FEATURE_ARGS=""
    MAKE_PROJECTS="NetTFGEngineCore NetTFGEngine GameClient GameServer Matchmaking"
    APPS="GameClient GameServer Matchmaking"
fi

# Parallel compile jobs for vcpkg and make: all cores by default, BUILD_JOBS=N to cap memory use
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
export VCPKG_MAX_CONCURRENCY="$BUILD_JOBS"

# vcpkg has no prebuilt cmake/ninja for ARM: it must use the system ones
if [ "$(uname -m)" != "x86_64" ]; then
    export VCPKG_FORCE_SYSTEM_BINARIES=1
fi

# The bundled premake5 is an x86_64 binary; on other hosts use the one in PATH (or PREMAKE=/path/to/premake5)
if [ -z "$PREMAKE" ]; then
    if [ "$(uname -m)" = "x86_64" ]; then
        PREMAKE="Vendor/Binaries/Premake/Linux/premake5"
    else
        PREMAKE="$(command -v premake5 || true)"
    fi
fi
if [ -z "$PREMAKE" ]; then
    echo "!!! premake5 not found: build it for this CPU and put it in PATH (or set PREMAKE)"
    exit 1
fi

# Install vcpkg packages to a space-free path inside WSL's native filesystem
VCPKG_SAFE_ROOT="$HOME/.vcpkg-installed/net-tfg-engine"
mkdir -p "$VCPKG_SAFE_ROOT"

echo ">>> Installing dependencies via vcpkg..."
"$VCPKG_ROOT/vcpkg" install \
    --triplet "$TRIPLET" \
    --x-install-root="$VCPKG_SAFE_ROOT"     $VCPKG_FEATURE_ARGS

# Recreate symlink from project vcpkg_installed to space-free path
mkdir -p "$PROJECT_ROOT/vcpkg_installed"
rm -rf "$PROJECT_ROOT/vcpkg_installed/$TRIPLET"
ln -s "$VCPKG_SAFE_ROOT/$TRIPLET" "$PROJECT_ROOT/vcpkg_installed/$TRIPLET"
echo ">>> Symlink: vcpkg_installed/$TRIPLET -> $VCPKG_SAFE_ROOT/$TRIPLET"

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
"$PREMAKE" --cc=clang --arch="$ARCH" --file=Build.lua gmake2

# Clean entire linux build output to avoid mkdir failures on existing dirs
echo ">>> Cleaning stale build output..."
rm -rf "$PROJECT_ROOT/Binaries/linux-$BIN_ARCH"
rm -rf "$PROJECT_ROOT/Binaries/Intermediates/linux-$BIN_ARCH"

# Compile only the real projects, not the LinuxBuild* stubs (which would cause recursion)
echo ">>> Compiling..."
make -j"$BUILD_JOBS" config=release $MAKE_PROJECTS

# Copy into each executable's folder only the vcpkg shared libraries (.so) it actually loads, transitively.
# The executables use rpath $ORIGIN, so each folder is self-contained and deployable on its own.
echo ">>> Copying shared libraries..."
OUTPUT_DIR="$PROJECT_ROOT/Binaries/linux-$BIN_ARCH/Release"
VCPKG_LIB="$VCPKG_SAFE_ROOT/$TRIPLET/lib"
for APP in $APPS; do
    APP_DIR="$OUTPUT_DIR/$APP"
    LD_LIBRARY_PATH="$VCPKG_LIB" ldd "$APP_DIR/$APP" | awk -v lib="$VCPKG_LIB/" 'index($3, lib) == 1 { print $1, $3 }' |
    while read -r SONAME LIBPATH; do
        cp -L "$LIBPATH" "$APP_DIR/$SONAME"
        echo "    $APP <- $SONAME"
    done
done

echo ">>> Done!"