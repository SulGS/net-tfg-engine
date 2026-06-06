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
   objdir (Directories.IntermediateDir)

   -- Windows (MSVC + vcpkg x64-windows)
   filter "system:windows"
      systemversion "latest"
      defines { }

      includedirs { "%{wks.location}/vcpkg_installed/x64-windows/include" }
      libdirs     { "%{wks.location}/vcpkg_installed/x64-windows/lib" }
      links
      {
         "freetype",
         "GameNetworkingSockets",
         "glew32",
         "glfw3",
         "OpenAL32",
         "opengl32",
         "SOIL2",
         "libssl",
         "libcrypto",
      }

   -- Linux (Clang + vcpkg x64-linux)
   filter "system:linux"
      includedirs { "%{wks.location}/vcpkg_installed/x64-linux/include" }
      libdirs     { "%{wks.location}/vcpkg_installed/x64-linux/lib" }
      links
      {
         "freetype",
         "GameNetworkingSockets",
         "GLEW",
         "glfw",
         "openal",
         "GL",
         "SOIL2",
         "ssl",
         "crypto",
      }

   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "On"

   filter { "configurations:Debug", "system:windows" }
      libdirs { "%{wks.location}/vcpkg_installed/x64-windows/debug/lib" }

   filter { "configurations:Debug", "system:linux" }
      libdirs { "%{wks.location}/vcpkg_installed/x64-linux/debug/lib" }

   filter "configurations:Release"
      defines { "RELEASE" }
      runtime "Release"
      optimize "On"
      symbols "On"

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"