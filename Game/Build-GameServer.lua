project "GameServer"
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
      "Source/ServerMain.cpp"
   }

   includedirs
   {
      "Source",
      "../NetTFGEngine/Source"
   }

   libdirs { Directories.EngineDir }
   links   { "NetTFGEngine" }

   -- Windows: vcpkg integration handled automatically by Visual Studio
   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }

   -- Linux
   filter "system:linux"
      includedirs { "%{wks.location}/vcpkg_installed/x64-linux/include" }
      libdirs     { "%{wks.location}/vcpkg_installed/x64-linux/lib" }
      linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
      links
      {
         "freetype",
         "png16",
         "brotlidec",
         "brotlicommon",
         "bz2",
         "z",
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

   -- Debug
   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "On"

   filter { "configurations:Debug", "system:linux" }
      libdirs { "%{wks.location}/vcpkg_installed/x64-linux/debug/lib" }

   -- Release
   filter "configurations:Release"
      defines { "RELEASE" }
      runtime "Release"
      optimize "On"
      symbols "On"

   -- Dist
   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"