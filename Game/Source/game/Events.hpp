#pragma once

#include "netcode/netcode_common.hpp"

enum AsteroidEventMask : uint8_t {
	SPAWN_BULLET = 0,
	BULLET_COLLIDES = 1,
	DEATH = 2,
	RESPAWN = 3
};

struct SpawnBulletEventData {
	int bulletId;
	int ownerId;
	float posX;
	float posY;
	float velX;
	float velY;
};


struct BulletCollidesEventData {
	int bulletId;
	int playerId;
};

struct DeathEventData {
	int playerId;
};

struct RespawnEventData {
	int playerId;
};