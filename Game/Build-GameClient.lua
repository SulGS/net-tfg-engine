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

   filter "system:windows"
      systemversion "latest"
      defines { "WINDOWS" }
      
      postbuildcommands 
      {
         -- Copy game content only
         'if exist "%{prj.location}\\Content" (xcopy /Y /E /I /Q "%{prj.location}\\Content\\*" "%{Directories.ContentDir}\\")',
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
	  linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup" }

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"
	  linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup" }