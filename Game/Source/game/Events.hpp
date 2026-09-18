#pragma once

#include "netcode/netcode_common.hpp"

enum AsteroidEventMask : uint8_t {
	SPAWN_BULLET = 0,
	BULLET_COLLIDES = 1,
	DEATH = 2,
	ENTER_SPECTATOR = 3,
	DESTROY_TILE = 4,
	WARN_TILE = 6,
};

struct SpawnBulletEventData {
	int bulletId;
	int ownerId;
	float posX;
	float posY;
	float velX;
	float velY;
};

struct WarnTileEventData {
	int tileId;
};

struct DestroyTileEventData {
	int tileId;
};

struct BulletCollidesEventData {
	int bulletId;
	int playerId;
};

struct DeathEventData {
	int playerId;
};