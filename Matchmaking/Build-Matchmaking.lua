project "Matchmaking"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   targetdir (Directories.OutputDir)
   objdir    (Directories.IntermediateDir)

   -- Headless: does not link NetTFGEngine (it would drag in GLFW/OpenGL/OpenAL).
   -- Only the shared protocol header and the Debug logger are taken from it.
   files
   {
      "Source/**.hpp",
      "Source/**.cpp",
      "../NetTFGEngine/Source/Matchmaking/MatchmakingProtocol.hpp",
      "../NetTFGEngine/Source/Utils/PlayerKey.hpp",
      "../NetTFGEngine/Source/Utils/Debug/**.hpp",
      "../NetTFGEngine/Source/Utils/Debug/**.cpp"
   }

   includedirs
   {
      "Source",
      "../NetTFGEngine/Source"
   }

   -- Windows: vcpkg integration handled automatically by Visual Studio
   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }

   -- Linux (cpp-httplib and nlohmann-json are header-only)
   filter "system:linux"
      -- -isystem: third-party headers must not raise warnings in our build
      externalincludedirs { "%{wks.location}/vcpkg_installed/x64-linux/include" }
      links { "pthread" }

   -- Debug
   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "On"

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
