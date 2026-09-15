#ifndef COLLISION_SYSTEM_HPP
#define COLLISION_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ICollider.hpp"
#include "ICollider2D.hpp"
#include "ICollider3D.hpp"
#include "ecs/ecs_common.hpp"
#include <set>
#include <map>
#include <utility>

class CollisionSystem : public ISystem {
public:
    CollisionSystem() = default;
    ~CollisionSystem() = default;

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override;

    std::vector<Entity> QueryPoint2D(EntityManager& entityManager, const glm::vec2& point);
    std::vector<Entity> QueryPoint3D(EntityManager& entityManager, const glm::vec3& point);

    std::vector<Entity> QueryRegion2D(EntityManager& entityManager,
                                      const glm::vec2& min, const glm::vec2& max);
    std::vector<Entity> QueryRegion3D(EntityManager& entityManager,
                                      const glm::vec3& min, const glm::vec3& max);

    bool Raycast2D(EntityManager& entityManager, const glm::vec2& origin, 
                   const glm::vec2& direction, float maxDistance,
                   Entity& hitEntity, glm::vec2& hitPoint);
    bool Raycast3D(EntityManager& entityManager, const glm::vec3& origin,
                   const glm::vec3& direction, float maxDistance,
                   Entity& hitEntity, glm::vec3& hitPoint);

private:
    std::set<std::pair<Entity, Entity>> previousCollisions;
    std::set<std::pair<Entity, Entity>> previousTriggers;

    std::set<std::pair<Entity, Entity>> currentCollisions;
    std::set<std::pair<Entity, Entity>> currentTriggers;

    // Sorted so (a,b) and (b,a) map to the same pair.
    std::pair<Entity, Entity> MakePair(Entity a, Entity b) const {
        return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
    }

    void CheckCollision(Entity entityA, ICollider* colliderA, Transform* transformA,
                       Entity entityB, ICollider* colliderB, Transform* transformB);

    void InvokeCollisionEnter(Entity a, Entity b, ICollider* colliderA,
                             ICollider* colliderB, const CollisionInfo& info);
    void InvokeCollisionStay(Entity a, Entity b, ICollider* colliderA,
                            ICollider* colliderB, const CollisionInfo& info);
    void InvokeCollisionExit(Entity a, Entity b, ICollider* colliderA,
                            ICollider* colliderB);
    
    void InvokeTriggerEnter(Entity a, Entity b, ICollider* colliderA,
                           ICollider* colliderB);
    void InvokeTriggerStay(Entity a, Entity b, ICollider* colliderA,
                          ICollider* colliderB);
    void InvokeTriggerExit(Entity a, Entity b, ICollider* colliderA,
                          ICollider* colliderB);
};

#endif // COLLISION_SYSTEM_HPP