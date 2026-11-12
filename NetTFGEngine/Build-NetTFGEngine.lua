-- The engine is split in two static libraries:
--   NetTFGEngineCore : headless part (ECS, collisions, netcode, server, matchmaking client, logging, localization).
--                      No OpenGL/GLFW/OpenAL/FreeType/SOIL: GameServer links only this one.
--   NetTFGEngine     : everything client-side (rendering, audio, window/input, UI, fonts, client netcode) on top of Core.
-- Core is compiled with NETTFG_HEADLESS, which makes every client-only header fail with #error if it gets included,
-- so a graphics dependency can't sneak back into the server build unnoticed.

-- Files of the headless core. Anything not listed here belongs to the client library.
EngineCoreFiles =
{
   "Source/Client-Server/Server.hpp",
   "Source/netcode/netcode_common.hpp",
   "Source/netcode/server_netcode.hpp",
   "Source/netcode/valve_sockets_session.hpp",
   "Source/ecs/ecs.hpp",
   "Source/ecs/ecs_common.hpp",
   "Source/ecs/ecs_gamelogic.hpp",
   "Source/ecs/Collisions/**.hpp",
   "Source/ecs/Collisions/**.cpp",
   "Source/ecs/Events/**.hpp",
   "Source/ecs/Deltas/**.hpp",
   "Source/Matchmaking/**.hpp",
   "Source/Matchmaking/**.cpp",
   "Source/Utils/Debug/**.hpp",
   "Source/Utils/Debug/**.cpp",
   "Source/Utils/PlayerKey.hpp",
   "Source/Utils/PlayerKey.cpp",
   "Source/Utils/Localization.hpp",
   "Source/Utils/Localization.cpp",
   "Source/Utils/UserDataPath.hpp",
   "Source/Utils/Utf8.hpp",
   -- Pure data (the AL handles are plain integers), used by the shared game components
   "Source/OpenAL/AudioComponents.hpp",
   "Source/OpenAL/AudioChannels.hpp",
}

-- Settings shared by both engine libraries
local function EngineCommon()
   language "C++"
   cppdialect "C++20"
   staticruntime "off"

   includedirs { "Source" }

   -- Engine outputs to Engine subfolder
   targetdir (Directories.EngineDir)
   objdir    (Directories.IntermediateDir)

   -- Windows: vcpkg integration handled automatically by Visual Studio
   filter "system:windows"
      systemversion "latest"

   -- Linux (Clang + vcpkg installed to space-free WSL path)
   filter "system:linux"
      -- -isystem: third-party headers must not raise warnings in our build
      externalincludedirs { VcpkgDir .. "/include" }
      libdirs     { VcpkgDir .. "/lib" }

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

   filter {}
end

project "NetTFGEngineCore"
   kind "StaticLib"
   EngineCommon()

   files   (EngineCoreFiles)
   defines { "NETTFG_HEADLESS" }

project "NetTFGEngine"
   kind "StaticLib"
   EngineCommon()

   files       { "Source/**.hpp", "Source/**.cpp" }
   removefiles (EngineCoreFiles)

   -- Built after Core; executables link both (NetTFGEngine first, then NetTFGEngineCore)
   dependson { "NetTFGEngineCore" }
