#pragma once

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"

class BulletSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        const float WORLD_SIZE = 400.0f;
        auto query = entityManager.CreateQuery<Transform, ECSBullet>();
        for (auto [entity, transform, ecsb] : query) {
            transform->translate(glm::vec3(ecsb->velX, ecsb->velY, 0.0f));
            ecsb->lifetime--;
            if (ecsb->lifetime <= 0 ||
                transform->getPosition().x < -WORLD_SIZE || transform->getPosition().x > WORLD_SIZE ||
                transform->getPosition().y < -WORLD_SIZE || transform->getPosition().y > WORLD_SIZE) {
                entityManager.DestroyEntity(entity);
            }
        }
    }
};
