# Overrides vcpkg's stock arm64-linux triplet (same settings) to build only the release variant of each
# dependency: under QEMU emulation the debug build roughly doubles the time and memory.
# Setup-Linux.sh only compiles config=release, so the missing debug/lib is never used here.
# Only applied inside the ARM64 Docker builder (VCPKG_OVERLAY_TRIPLETS in Dockerfile.arm64).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_BUILD_TYPE release)
