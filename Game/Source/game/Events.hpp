#pragma once

#include "netcode/netcode_common.hpp"

enum AsteroidEventMask : uint8_t {
	SPAWN_BULLET = 0,
	BULLET_COLLIDES = 1,
	DEATH = 2,
	ENTER_SPECTATOR = 3,
	DESTROY_TILE = 4,
	WARN_TILE = 6,
	BULLET_HIT_WALL = 7,
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

// Server-side wall hit: the server destroys the bullet on the spot; this tells clients, whose bullets aren't in the
// deltas and have no colliders, so theirs don't keep flying through the wall until their lifetime runs out.
struct BulletHitWallEventData {
	int bulletId;
};

struct DeathEventData {
	int playerId;
};