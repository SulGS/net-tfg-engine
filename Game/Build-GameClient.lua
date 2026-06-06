project "GameClient"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   targetdir (Directories.OutputDir)
   objdir (Directories.IntermediateDir)

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
   links { "NetTFGEngine" }

   -- Windows
   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }

      includedirs { "%{wks.location}/vcpkg_installed/x64-windows/include" }

      postbuildcommands {
         'if exist "%{prj.location}/Assets" (python "%{prj.location}/../AssetsPackager.py" "%{prj.location}/Assets" "%{cfg.targetdir}")'
      }

   -- Linux
   filter "system:linux"
      includedirs { "%{wks.location}/vcpkg_installed/x64-linux/include" }

   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "On"

   filter "configurations:Release"
      defines { "RELEASE" }
      runtime "Release"
      optimize "On"
      symbols "On"
      linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup" }

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"
      linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup" }