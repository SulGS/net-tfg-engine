# NetTFG Engine

NetTFG Engine is a netcode game engine built using Premake. It is designed to facilitate the development of networked games by providing a robust framework for handling networking, game logic, and rendering.

## Engine libraries

The engine is built as two static libraries:

- `NetTFGEngineCore`: headless part (ECS, collisions, netcode, server, matchmaking client, logging, localization).
  Depends only on GameNetworkingSockets, OpenSSL and glm.
- `NetTFGEngine`: client part (OpenGL renderer, OpenAL audio, GLFW window/input, UI, fonts, asset loading) on top of Core.

`GameServer` links only `NetTFGEngineCore`, so it has no OpenGL/GLFW/OpenAL/FreeType/SOIL dependency; `GameClient`
links both. The game simulation the server runs lives in `Game/Source/game/AsteroidsLogic.hpp`; the client side
(`AsteroidShooterClientGame`, the renderer) in `asteroids.hpp`. Core and GameServer are compiled with
`NETTFG_HEADLESS`, which turns any include of a client-only engine header into a compile error.

The graphics/audio vcpkg packages are in the manifest's `client` feature (on by default). A headless machine can
skip them: `SERVER_ONLY=1 bash Scripts/Setup-Linux.sh` installs without that feature and builds only `GameServer`
and `Matchmaking`.

## Building for Linux

Linux builds run through WSL2 (Ubuntu) with Clang and vcpkg. Each executable ends up in its own self-contained
folder under `Binaries/linux-<arch>/Release/` (`GameClient`, `GameServer`, `Matchmaking`), next to the vcpkg
shared libraries it loads.

### x86_64

Any of these:

- Visual Studio: build the `LinuxBuildx86` project (right-click > Build; it is left out of "Build Solution").
- Windows: `Scripts\Build-Linux.bat "<project path>"`
- WSL: `bash Scripts/Setup-Linux.sh`

Output: `Binaries/linux-x86_64/Release/`.

### ARM64 (AArch64)

`Setup-Linux.sh` builds for the CPU it runs on, so ARM64 binaries are compiled inside an ARM64 Docker container,
emulated with QEMU on an x86_64 machine (`Scripts/Build-Linux-ARM64.sh`, `Scripts/Dockerfile.arm64`,
`Scripts/ARM64-Triplets/`). On a machine where the builder doesn't exist yet, the script creates it: it registers
QEMU, builds the image and creates the cache volume.

1. Needs Docker inside WSL: `sudo apt install docker.io && sudo usermod -aG docker $USER`, then restart WSL.
2. Either build the `LinuxBuildARM64` project in Visual Studio (right-click > Build; it is left out of
   "Build Solution"), or run it from a WSL terminal that stays open until it finishes (if WSL shuts down, the build
   dies with it):
   ```bash
   bash Scripts/Build-Linux-ARM64.sh
   ```
   The first run compiles every vcpkg dependency under emulation and takes hours; later runs reuse them from the
   `net-tfg-arm64-cache` Docker volume and take about 35 minutes. `BUILD_JOBS=N` lowers the parallel jobs
   (default 6) if memory runs short.
3. Output: `Binaries/linux-ARM64/Release/`.
4. The build regenerates the tracked makefiles (`Game/*.make`) for ARM64. Regenerate them for x86_64 afterwards with
   `Vendor/Binaries/Premake/Linux/premake5 --cc=clang --file=Build.lua gmake2` (any x86_64 build also does it).
   Don't run both builds at the same time.

The targets need the system's OpenGL/Mesa, X11, ALSA/PulseAudio and libstdc++, and a glibc at least as new as
Ubuntu 24.04's.

Release-only dependencies: the builder overrides vcpkg's `arm64-linux` triplet so dependencies are built only in
release, which is all `Setup-Linux.sh` compiles. ARM64 Debug configurations have no vcpkg debug libraries.

### vcpkg overlay ports

`vcpkg-ports/` (registered in `vcpkg-configuration.json`, so it applies to every platform) replaces registry ports
that no longer install. `tinygltf` 2.9.3: GitHub now serves a different source tarball for that tag, which fails
the stock port's SHA512 check, so the overlay fetches the tag's commit through git instead.

### Floating point

Linux builds use `-ffp-contract=off`: AArch64 always has FMA and Clang would fuse `a*b+c` there but not on x86_64,
making the simulation round differently on each CPU.

## License
- UNLICENSE for this repository (see `UNLICENSE.txt` for more details)
- Premake is licensed under BSD 3-Clause (see included LICENSE.txt file for more details)