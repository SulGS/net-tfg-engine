project "GameServer"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   targetdir ("../Binaries/" .. OutputDir .. "/%{prj.name}")
   objdir ("../Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")

   files 
   {
      "Source/game/**.hpp",
      "Source/game/**.cpp",
      "Source/ServerMain.cpp"  -- only server main
   }

   includedirs
   {
      "Source",
      "../NetTFGEngine/Source"
   }

   links { "NetTFGEngine" }

   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }

   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "On"

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
