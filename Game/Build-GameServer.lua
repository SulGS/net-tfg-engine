project "GameServer"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   targetdir (Directories.OutputDir)
   objdir    (Directories.IntermediateDir)

   -- Headless: only the shared game logic, never the renderer side (RenderSystems, menus, asteroids.hpp...)
   files
   {
      "Source/game/**.hpp",
      "Source/game/**.cpp",
      "Source/ServerMain.cpp"
   }
   removefiles
   {
      "Source/game/RenderSystems/**",
      "Source/game/LogicSystems/ExitCheckerSystem.hpp",
      "Source/game/asteroids.hpp",
      "Source/game/menu.hpp",
      "Source/game/settings_menu.hpp",
      "Source/game/PauseMenu.hpp",
      "Source/game/Explosion.hpp",
      "Source/game/GameActions.hpp",
   }

   -- Client-only engine headers (OpenGL, GLFW, OpenAL, FreeType, SOIL) #error out if included
   defines { "NETTFG_HEADLESS" }

   includedirs
   {
      "Source",
      "../NetTFGEngine/Source"
   }

   libdirs { Directories.EngineDir }
   links   { "NetTFGEngineCore" }

   -- Windows: vcpkg integration handled automatically by Visual Studio
   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }

   -- Linux
   filter "system:linux"
      -- -isystem: third-party headers must not raise warnings in our build
      externalincludedirs { VcpkgDir .. "/include" }
      libdirs     { VcpkgDir .. "/lib" }
      linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
      links
      {
         "GameNetworkingSockets",
         "ssl",
         "crypto",
         "pthread",
         "dl",
      }

   -- Debug
   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "On"

   filter { "configurations:Debug", "system:linux" }
      libdirs { VcpkgDir .. "/debug/lib" }

   -- Release
   filter "configurations:Release"
      defines { "RELEASE" }
      runtime "Release"
      optimize "Speed"
      symbols "On"

   -- Dist
   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "Speed"
      symbols "Off"