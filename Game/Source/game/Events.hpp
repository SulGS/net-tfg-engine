#pragma once

#include "netcode/netcode_common.hpp"

enum AsteroidEventMask : uint8_t {
	PLAYER_POSITION = 0,
	SPAWN_BULLET = 1,
	BULLET_COLLIDES = 2
};


struct PlayerPositionEventData {
	int playerId;
	float x;
	float y;
	float rotation;
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