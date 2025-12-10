#pragma once
#include "OpenGL/OpenGLIncludes.hpp"
#include "ecs/Events/ecs_iecs_event_handler.hpp"
#include "ecs/ecs_common.hpp"
#include "ecs/Collisions/BoxCollider2D.hpp"
#include "Events.hpp"
#include "Components.hpp"

#include "OpenAL/AudioManager.hpp"
#include "OpenAL/AudioComponents.hpp"

class SpawnBulletHandler : public IEventHandler {
public:
    void Handle(const GameEventBlob& event, ECSWorld& world, bool isServer) override
    {
        //std::cout << "Processing SPAWN_BULLET event\n";

        const int BULLET_LIFETIME = 30;
        auto spawn_ev = *reinterpret_cast<const SpawnBulletEventData*>(event.data);

        Entity bulletEntity = world.GetEntityManager().CreateEntity();
        Transform* t = world.GetEntityManager().AddComponent<Transform>(bulletEntity, Transform{});
        t->setPosition(glm::vec3(spawn_ev.posX, spawn_ev.posY, 0.0f));
        world.GetEntityManager().AddComponent<ECSBullet>(bulletEntity,
            ECSBullet{ spawn_ev.bulletId, spawn_ev.velX, spawn_ev.velY, spawn_ev.ownerId, BULLET_LIFETIME });

        if (isServer) {
            BoxCollider2D* collider = world.GetEntityManager().AddComponent<BoxCollider2D>(
                bulletEntity, BoxCollider2D{ glm::vec2(1.0f, 1.0f) });
            collider->layer = CollisionLayer::BULLET;
            collider->collidesWith = CollisionLayer::PLAYER;
            collider->SetOnCollisionEnter([&world](Entity self, Entity other, const CollisionInfo& info) {
                Playable* p = world.GetEntityManager().GetComponent<Playable>(other);

                if (!p || p->playerId == world.GetEntityManager().GetComponent<ECSBullet>(self)->ownerId) {
                    return;
                }

                EntityManager& em = world.GetEntityManager();
                EventEntry eventEntry;
                eventEntry.event.type = AsteroidEventMask::BULLET_COLLIDES;
                BulletCollidesEventData data;
                ECSBullet* ecsb = em.GetComponent<ECSBullet>(self);
                Playable* play = em.GetComponent<Playable>(other);
                data.bulletId = ecsb->id;
                data.playerId = play->playerId;
                std::memcpy(eventEntry.event.data, &data, sizeof(BulletCollidesEventData));
                eventEntry.event.len = sizeof(BulletCollidesEventData);

                world.GetEvents().push_back(eventEntry);
                });
        }
    }
};

class BulletCollidesHandler : public IEventHandler {
public:
    void Handle(const GameEventBlob& event, ECSWorld& world, bool isServer) override
    {

        auto coll_ev = *reinterpret_cast<const BulletCollidesEventData*>(event.data);

        auto query2 = world.GetEntityManager().CreateQuery<Playable, SpaceShip>();

        for (auto [entity, play, ship] : query2) {
			
            if (play->playerId == coll_ev.playerId && ship->isAlive) {
                ship->health -= 5;



                if (ship->health <= 0) {

                    if (isServer) 
                    {

                        EventEntry deathEvent;
                        deathEvent.event.type = AsteroidEventMask::DEATH;
                        DeathEventData deathData;
                        deathData.playerId = play->playerId;
                        std::memcpy(deathEvent.event.data, &deathData, sizeof(DeathEventData));
                        deathEvent.event.len = sizeof(DeathEventData);
                        world.GetEvents().push_back(deathEvent);
                    }
					

                }
            }
        }

        auto query = world.GetEntityManager().CreateQuery<Transform, ECSBullet>();
        for (auto [entity, transform, ecsb] : query) {
            if (ecsb->id == coll_ev.bulletId) {
                world.GetEntityManager().DestroyEntity(entity);
            }
        }
    }
};

class DeathHandler : public IEventHandler {
public:
	void Handle(const GameEventBlob& event, ECSWorld& world, bool isServer) override
	{

		auto death_ev = *reinterpret_cast<const DeathEventData*>(event.data);
		auto query = world.GetEntityManager().CreateQuery<Playable, SpaceShip>();
		for (auto [entity, play, ship] : query) {
			if (play->playerId == death_ev.playerId) {
				ship->health = 0;
				ship->deathCooldown = 10 * TICKS_PER_SECOND;
				ship->isAlive = false;
				if (isServer) {
					auto col = world.GetEntityManager().GetComponent<BoxCollider2D>(entity);
					if (col) {
						col->isEnabled = false;
					}
				}
			}
		}
	}
};

class RespawnHandler : public IEventHandler {
public:
	void Handle(const GameEventBlob& event, ECSWorld& world, bool isServer) override
	{

		auto respawn_ev = *reinterpret_cast<const RespawnEventData*>(event.data);
		auto query = world.GetEntityManager().CreateQuery<Playable, Transform, SpaceShip>();
		for (auto [entity, play, transform, ship] : query) {
			if (play->playerId == respawn_ev.playerId) {
				ship->health = 100;
				ship->deathCooldown = 0;
				ship->isAlive = true;
				transform->setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
				transform->setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
				if (isServer) {
					auto col = world.GetEntityManager().GetComponent<BoxCollider2D>(entity);
					if (col) {
						col->isEnabled = true;
					}
				}
			}
		}
	}
};