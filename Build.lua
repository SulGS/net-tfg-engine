-- premake5.lua
workspace "TFG Project"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "GameClient"   -- default startup project

   -- Workspace-wide build options for MSVC
   filter "system:windows"
      buildoptions { "/EHsc", "/Zc:preprocessor", "/Zc:__cplusplus" }

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

-- Include Engine project
group "NetTFGEngine"
	include "NetTFGEngine/Build-NetTFGEngine.lua"
group ""

-- Include Game projects
group "Game"
	include "Game/Build-GameClient.lua"
	include "Game/Build-GameServer.lua"
group ""
