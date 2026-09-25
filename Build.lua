-- premake5.lua
workspace "TFG Project"
   architecture "x64"
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


OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

-- Define directory structure
Directories = {}
Directories.OutputDir        = "%{wks.location}/Binaries/" .. OutputDir
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

-- Linux build stub (Windows-only: triggers WSL2 build from Visual Studio)
-- No path argument is passed to avoid MSBuild quote/backslash issues with spaces in paths.
-- The shell scripts resolve the project root from their own location instead.
filter "system:windows"
   group "Linux"
      project "LinuxBuild"
         kind "Makefile"
         location "%{wks.location}"

         buildcommands  { "wsl.exe bash Scripts/wsl-build.sh" }
         rebuildcommands{ "wsl.exe bash Scripts/wsl-build.sh" }
         cleancommands  { "wsl.exe bash Scripts/wsl-clean.sh" }
   group ""
filter {}