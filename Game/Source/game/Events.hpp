#pragma once

#include "netcode/netcode_common.hpp"

enum AsteroidEventMask : uint8_t {
	BULLET_COLLIDES = 0,
	DEATH = 1,
	RESPAWN = 2
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