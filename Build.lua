-- premake5.lua

-- Target CPU (Linux builds): --arch=arm64 builds for AArch64 against vcpkg's arm64-linux triplet
newoption
{
   trigger     = "arch",
   value       = "ARCH",
   description = "Target CPU architecture",
   default     = "x64",
   allowed     = { { "x64", "x86_64" }, { "arm64", "AArch64" } }
}

IsArm64      = (_OPTIONS["arch"] == "arm64")
VcpkgTriplet = IsArm64 and "arm64-linux" or "x64-linux"
VcpkgDir     = "%{wks.location}/vcpkg_installed/" .. VcpkgTriplet

workspace "TFG Project"
   architecture (IsArm64 and "ARM64" or "x64")
   configurations { "Debug", "Release", "Dist" }
   startproject "GameClient"

   -- Workspace-wide build options for MSVC
   filter "toolset:msc*"
      buildoptions {
         "/EHsc",
         "/Zc:preprocessor",
         "/Zc:__cplusplus",
         "/bigobj",
         "/utf-8"  -- sources are UTF-8: keeps literals like "¿Estás?" intact (the UI decodes UTF-8)
      }
      -- C4828 (invalid UTF-8 byte): only third-party headers trigger it (GameNetworkingSockets' steamtypes.h is Latin-1)
      disablewarnings { "4828" }
   filter {}

   -- No implicit FMA fusion: AArch64 always has FMA and clang would contract a*b+c there but not on x86_64,
   -- so the same simulation would round differently on each CPU (MSVC's default /fp:precise doesn't contract)
   filter "system:linux"
      buildoptions { "-ffp-contract=off" }
   filter {}


OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

-- Define directory structure
Directories = {}
-- One folder per executable (Binaries/<os>/<cfg>/<project>) holding only what that program needs, so each
-- folder is deployable as-is. On Windows, vcpkg's applocal step copies next to each .exe only the DLLs it imports.
Directories.OutputDir        = "%{wks.location}/Binaries/" .. OutputDir .. "/%{prj.name}"
Directories.IntermediateDir  = "%{wks.location}/Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}"
Directories.EngineDir        = "%{wks.location}/Binaries/" .. OutputDir .. "/Engine"
Directories.ThirdPartyDir    = "%{wks.location}/Binaries/" .. OutputDir .. "/ThirdParty"
Directories.ContentDir       = "%{wks.location}/Binaries/" .. OutputDir .. "/Content"

-- Include Engine project
group "NetTFGEngine"
   include "NetTFGEngine/Build-NetTFGEngine.lua"
group ""

-- Include Game projects
group "Game"
   include "Game/Build-GameClient.lua"
   include "Game/Build-GameServer.lua"
group ""

-- Include Matchmaking server (hands out GameServer instances to clients)
group "Matchmaking"
   include "Matchmaking/Build-Matchmaking.lua"
group ""

-- Linux build stubs (Windows-only: trigger the WSL2 builds from Visual Studio)
-- No path argument is passed to avoid MSBuild quote/backslash issues with spaces in paths.
-- The shell scripts resolve the project root from their own location instead.
-- ExcludeFromBuild keeps them out of "Build Solution": they only run when built explicitly.
filter "system:windows"
   group "Linux"
      project "LinuxBuildx86"
         kind "Makefile"
         location "%{wks.location}"
         flags { "ExcludeFromBuild" }

         buildcommands  { "wsl.exe bash Scripts/wsl-build.sh" }
         rebuildcommands{ "wsl.exe bash Scripts/wsl-build.sh" }
         cleancommands  { "wsl.exe bash Scripts/wsl-clean.sh" }

      -- ARM64 through an emulated Docker container: ~35 min with cached dependencies, hours when they change
      project "LinuxBuildARM64"
         kind "Makefile"
         location "%{wks.location}"
         flags { "ExcludeFromBuild" }

         buildcommands  { "wsl.exe bash Scripts/Build-Linux-ARM64.sh" }
         rebuildcommands{ "wsl.exe bash Scripts/Build-Linux-ARM64.sh" }
         cleancommands  { "wsl.exe bash Scripts/wsl-clean.sh arm64" }
   group ""
filter {}