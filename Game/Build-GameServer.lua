project "GameServer"
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
      "Source/ServerMain.cpp"
   }

   includedirs
   {
      "Source",
      "../NetTFGEngine/Source"
   }

   libdirs { Directories.EngineDir }
   links { "NetTFGEngine" }

   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }
      
      postbuildcommands 
      {
         -- Copy server-specific content only
         'if exist "%{prj.location}\\Content\\Server" (xcopy /Y /E /I /Q "%{prj.location}\\Content\\Server\\*" "%{Directories.ContentDir}\\Server\\")',
         'if exist "%{prj.location}\\Content\\Config" (xcopy /Y /E /I /Q "%{prj.location}\\Content\\Config\\*" "%{Directories.ContentDir}\\Config\\")',
      }

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