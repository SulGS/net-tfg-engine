-- premake5.lua
workspace "TFG Project"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "GameClient"

   -- Workspace-wide build options for MSVC
   filter "system:windows"
      buildoptions { "/EHsc", "/Zc:preprocessor", "/Zc:__cplusplus" }

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

-- Define directory structure
Directories = {}
Directories.OutputDir = "%{wks.location}/Binaries/" .. OutputDir
Directories.IntermediateDir = "%{wks.location}/Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}"
Directories.EngineDir = "%{wks.location}/Binaries/" .. OutputDir .. "/Engine"
Directories.ThirdPartyDir = "%{wks.location}/Binaries/" .. OutputDir .. "/ThirdParty"
Directories.ContentDir = "%{wks.location}/Binaries/" .. OutputDir .. "/Content"

-- Include Engine project
group "NetTFGEngine"
   include "NetTFGEngine/Build-NetTFGEngine.lua"
group ""

-- Include Game projects
group "Game"
   include "Game/Build-GameClient.lua"
   include "Game/Build-GameServer.lua"
group ""