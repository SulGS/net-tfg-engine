-- premake5.lua
workspace "TFG Project"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "Game"

   -- Enable Visual Studio integrated vcpkg
   filter "system:windows"
      buildoptions { "/EHsc", "/Zc:preprocessor", "/Zc:__cplusplus" }
      -- Enable vcpkg manifest mode
      defines { "VCPKG_MANIFEST_MODE=ON" }
      
   -- Set vcpkg configuration at workspace level
   vcpkgenabled "On"

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

group "NetTFGEngine"
	include "NetTFGEngine/Build-NetTFGEngine.lua"
group ""

include "Game/Build-Game.lua"