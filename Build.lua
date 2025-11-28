-- premake5.lua
workspace "TFG Project"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "Game"

   -- Workspace-wide build options for MSVC
   filter "system:windows"
      buildoptions { "/EHsc", "/Zc:preprocessor", "/Zc:__cplusplus" }

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

group "NetTFGEngine"
	include "NetTFGEngine/Build-NetTFGEngine.lua"
group ""

include "Game/Build-Game.lua"