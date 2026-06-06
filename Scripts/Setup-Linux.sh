#!/bin/bash
set -e

pushd ..

# System dependencies
echo ">>> Installing build tools..."
sudo apt update
sudo apt install -y build-essential clang

# Install vcpkg dependencies (x64-linux)
echo ">>> Installing dependencies via vcpkg..."
~/vcpkg/vcpkg install --triplet x64-linux

# Generate makefiles
echo ">>> Generating makefiles..."
Vendor/Binaries/Premake/Linux/premake5 --cc=clang --file=Build.lua gmake2

# Compile
echo ">>> Compiling..."
make -j$(nproc) config=release

popd

echo ">>> Done!"