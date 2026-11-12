project "GameClient"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   targetdir (Directories.OutputDir)
   objdir    (Directories.IntermediateDir)

   files
   {
      "Source/game/**.hpp",
      "Source/game/**.cpp",
      "Source/ClientMain.cpp"
   }

   includedirs
   {
      "Source",
      "../NetTFGEngine/Source"
   }

   libdirs { Directories.EngineDir }
   -- Order matters for GNU ld: the client library uses symbols from Core
   links   { "NetTFGEngine", "NetTFGEngineCore" }

   -- Windows: vcpkg integration handled automatically by Visual Studio
   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }
	  files { "Resources/GameClient.rc" }

      postbuildcommands {
         'if exist "%{prj.location}/Assets" (python "%{prj.location}/../AssetsPackager.py" "%{prj.location}/Assets" "%{cfg.targetdir}")'
      }

   -- Linux
   filter "system:linux"
      -- -isystem: third-party headers must not raise warnings in our build
      externalincludedirs { VcpkgDir .. "/include" }
      libdirs     { VcpkgDir .. "/lib" }
      linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
      links
      {
         "freetype",
         "png16",
         "brotlidec",
         "brotlicommon",
         "bz2",
         "z",
         "zstd",
         "GameNetworkingSockets",
         "GLEW",
         "glfw3",
         "openal",
         "GL",
         "soil2",
         "ssl",
         "crypto",
         "pthread",
         "dl",
      }

      postbuildcommands {
         '[ -d "Assets" ] && python3 "../AssetsPackager.py" "Assets" "%{cfg.targetdir}" || true'
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

   -- Release Windows: hide console window
   filter { "configurations:Release", "system:windows" }
      linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup" }

   -- Dist
   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "Speed"
      symbols "Off"

   -- Dist Windows: hide console window
   filter { "configurations:Dist", "system:windows" }
      linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup" }