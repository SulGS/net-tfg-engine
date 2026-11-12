#pragma once

// Current game version, shared by GameClient and GameServer: bump it on every release. Clients and servers with
// different versions refuse each other (CLIENT_HELLO check), and the matchmaker can require it (game-version setting).
// Format: GameVersion::IsValid (1-31 printable characters without spaces).
inline constexpr const char* ASTEROIDS_GAME_VERSION = "0.2.0";
