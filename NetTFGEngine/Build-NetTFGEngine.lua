project "NetTFGEngine"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   files { "Source/**.hpp", "Source/**.cpp" }

   includedirs
   {
      "Source"
   }

   -- Engine outputs to Engine subfolder
   targetdir (Directories.EngineDir)
   objdir    (Directories.IntermediateDir)

   -- Windows: vcpkg integration handled automatically by Visual Studio
   filter "system:windows"
      systemversion "latest"

   -- Linux (Clang + vcpkg installed to space-free WSL path)
   filter "system:linux"
      includedirs { "%{wks.location}/vcpkg_installed/x64-linux/include" }
      libdirs     { "%{wks.location}/vcpkg_installed/x64-linux/lib" }
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