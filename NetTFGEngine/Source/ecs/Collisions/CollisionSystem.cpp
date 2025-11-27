#include "CollisionSystem.hpp"
#include "CircleCollider2D.hpp"
#include "BoxCollider2D.hpp"
#include "SphereCollider3D.hpp"
#include "BoxCollider3D.hpp"
#include "CollisionHelpers.hpp"
#include <iostream>

void CollisionSystem::Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) {
    // Clear current frame collisions
    currentCollisions.clear();
    currentTriggers.clear();
    
    // Get all entities with colliders and transforms
    std::vector<std::pair<Entity, ICollider*>> colliders2D;
    std::vector<std::pair<Entity, ICollider*>> colliders3D;
    
    // Collect 2D colliders
    auto query2D = entityManager.CreateQuery<CircleCollider2D, Transform>();
    for (auto [entity, collider, transform] : query2D) {
        if (!collider->isEnabled) continue;

        collider->transform = transform;
        
        colliders2D.push_back({entity, collider});
    }

    auto query2DBox = entityManager.CreateQuery<BoxCollider2D, Transform>();
    for (auto [entity, collider, transform] : query2DBox) {
        if (!collider->isEnabled) continue;

        collider->transform = transform;
        
        colliders2D.push_back({entity, collider});
    }
    
    auto query3D = entityManager.CreateQuery<SphereCollider3D, Transform>();
    for (auto [entity, collider, transform] : query3D) {
        if (!collider->isEnabled) continue;

        collider->transform = transform;
        
        colliders3D.push_back({entity, collider});
    }

    auto query3DBox = entityManager.CreateQuery<BoxCollider3D, Transform>();
    for (auto [entity, collider, transform] : query3DBox) {
        if (!collider->isEnabled) continue;

        collider->transform = transform;
        
        colliders3D.push_back({entity, collider});
    }
    
    // Check 2D collisions (broadphase + narrowphase)
    for (size_t i = 0; i < colliders2D.size(); i++) {
        auto [entityA, colliderA] = colliders2D[i];
        
        for (size_t j = i + 1; j < colliders2D.size(); j++) {
            auto [entityB, colliderB] = colliders2D[j];
            
            // Skip if layers don't match
            if (!colliderA->CanCollideWith(colliderB->layer) ||
                !colliderB->CanCollideWith(colliderA->layer)) {
                continue;
            }
            
            // Broadphase: AABB check
            ICollider2D* col2DA = dynamic_cast<ICollider2D*>(colliderA);
            ICollider2D* col2DB = dynamic_cast<ICollider2D*>(colliderB);
            
            // Narrowphase: Detailed collision check
            Transform* transformA = entityManager.GetComponent<Transform>(entityA);
            Transform* transformB = entityManager.GetComponent<Transform>(entityB);
            CheckCollision(entityA, colliderA, transformA, 
                          entityB, colliderB, transformB);
        }
    }
    
    // Check 3D collisions (broadphase + narrowphase)
    for (size_t i = 0; i < colliders3D.size(); i++) {
        auto [entityA, colliderA] = colliders3D[i];
        
        for (size_t j = i + 1; j < colliders3D.size(); j++) {
            auto [entityB, colliderB] = colliders3D[j];
            
            // Skip if layers don't match
            if (!colliderA->CanCollideWith(colliderB->layer) ||
                !colliderB->CanCollideWith(colliderA->layer)) {
                continue;
            }
            
            // Broadphase: AABB check
            ICollider3D* col3DA = dynamic_cast<ICollider3D*>(colliderA);
            ICollider3D* col3DB = dynamic_cast<ICollider3D*>(colliderB);
            
            // Narrowphase: Detailed collision check
            Transform* transformA = entityManager.GetComponent<Transform>(entityA);
            Transform* transformB = entityManager.GetComponent<Transform>(entityB);
            CheckCollision(entityA, colliderA, transformA,
                          entityB, colliderB, transformB);
        }
    }
    
    // Process collision exits
    for (const auto& pair : previousCollisions) {
        if (currentCollisions.find(pair) == currentCollisions.end()) {
            // Collision ended
            ICollider* colliderA = entityManager.GetComponent<ICollider>(pair.first);
            ICollider* colliderB = entityManager.GetComponent<ICollider>(pair.second);
            
            if (colliderA && colliderB) {
                InvokeCollisionExit(pair.first, pair.second, colliderA, colliderB);
            }
        }
    }
    
    // Process trigger exits
    for (const auto& pair : previousTriggers) {
        if (currentTriggers.find(pair) == currentTriggers.end()) {
            // Trigger ended
            ICollider* colliderA = entityManager.GetComponent<ICollider>(pair.first);
            ICollider* colliderB = entityManager.GetComponent<ICollider>(pair.second);
            
            if (colliderA && colliderB) {
                InvokeTriggerExit(pair.first, pair.second, colliderA, colliderB);
            }
        }
    }
    
    // Update previous frame data
    previousCollisions = currentCollisions;
    previousTriggers = currentTriggers;
}

void CollisionSystem::CheckCollision(Entity entityA, ICollider* colliderA, Transform* transformA,
                                     Entity entityB, ICollider* colliderB, Transform* transformB) {
    CollisionInfo info;
    info.otherEntity = entityB;
    
    // Perform collision test
    if (!colliderA->CheckCollision(colliderB, info)) {
        return; // No collision
    }
    
    auto pair = MakePair(entityA, entityB);
    bool isNewCollision = (previousCollisions.find(pair) == previousCollisions.end() &&
                           previousTriggers.find(pair) == previousTriggers.end());
    
    // Determine if this is a trigger or solid collision
    if (colliderA->isTrigger || colliderB->isTrigger) {
        // Trigger collision
        currentTriggers.insert(pair);
        
        if (isNewCollision) {
            InvokeTriggerEnter(entityA, entityB, colliderA, colliderB);
        } else {
            InvokeTriggerStay(entityA, entityB, colliderA, colliderB);
        }
    } else {
        // Solid collision
        currentCollisions.insert(pair);
        
        if (isNewCollision) {
            InvokeCollisionEnter(entityA, entityB, colliderA, colliderB, info);
        } else {
            InvokeCollisionStay(entityA, entityB, colliderA, colliderB, info);
        }
    }
}

void CollisionSystem::InvokeCollisionEnter(Entity a, Entity b, ICollider* colliderA,
                                           ICollider* colliderB, const CollisionInfo& info) {
    if (colliderA->onCollisionEnter) {
        CollisionInfo infoA = info;
        infoA.otherEntity = b;
        colliderA->onCollisionEnter(a, b, infoA);
    }
    
    if (colliderB->onCollisionEnter) {
        CollisionInfo infoB = info;
        infoB.otherEntity = a;
        infoB.normal = -infoB.normal; // Flip normal for other collider
        colliderB->onCollisionEnter(b, a, infoB);
    }
}

void CollisionSystem::InvokeCollisionStay(Entity a, Entity b, ICollider* colliderA,
                                          ICollider* colliderB, const CollisionInfo& info) {
    if (colliderA->onCollisionStay) {
        CollisionInfo infoA = info;
        infoA.otherEntity = b;
        colliderA->onCollisionStay(a, b, infoA);
    }
    
    if (colliderB->onCollisionStay) {
        CollisionInfo infoB = info;
        infoB.otherEntity = a;
        infoB.normal = -infoB.normal;
        colliderB->onCollisionStay(b, a, infoB);
    }
}

void CollisionSystem::InvokeCollisionExit(Entity a, Entity b, ICollider* colliderA,
                                          ICollider* colliderB) {
    if (colliderA->onCollisionExit) {
        colliderA->onCollisionExit(a, b);
    }
    
    if (colliderB->onCollisionExit) {
        colliderB->onCollisionExit(b, a);
    }
}

void CollisionSystem::InvokeTriggerEnter(Entity a, Entity b, ICollider* colliderA,
                                         ICollider* colliderB) {
    if (colliderA->onTriggerEnter) {
        colliderA->onTriggerEnter(a, b);
    }
    
    if (colliderB->onTriggerEnter) {
        colliderB->onTriggerEnter(b, a);
    }
}

void CollisionSystem::InvokeTriggerStay(Entity a, Entity b, ICollider* colliderA,
                                        ICollider* colliderB) {
    if (colliderA->onTriggerStay) {
        colliderA->onTriggerStay(a, b);
    }
    
    if (colliderB->onTriggerStay) {
        colliderB->onTriggerStay(b, a);
    }
}

void CollisionSystem::InvokeTriggerExit(Entity a, Entity b, ICollider* colliderA,
                                        ICollider* colliderB) {
    if (colliderA->onTriggerExit) {
        colliderA->onTriggerExit(a, b);
    }
    
    if (colliderB->onTriggerExit) {
        colliderB->onTriggerExit(b, a);
    }
}

// Query implementations (simplified - can be optimized with spatial partitioning)
std::vector<Entity> CollisionSystem::QueryPoint2D(EntityManager& entityManager, const glm::vec2& point) {
    std::vector<Entity> result;
    
    auto query = entityManager.CreateQuery<ICollider2D, Transform>();
    for (auto [entity, collider, transform] : query) {
        if (!collider->isEnabled) continue;
        
        if (CollisionHelpers::PointInAABB2D(point, collider->GetMin(), collider->GetMax())) {
            // TODO: More precise check for non-box colliders
            result.push_back(entity);
        }
    }
    
    return result;
}

std::vector<Entity> CollisionSystem::QueryPoint3D(EntityManager& entityManager, const glm::vec3& point) {
    std::vector<Entity> result;
    
    auto query = entityManager.CreateQuery<ICollider3D, Transform>();
    for (auto [entity, collider, transform] : query) {
        if (!collider->isEnabled) continue;
        
        if (CollisionHelpers::PointInAABB3D(point, collider->GetMin(), collider->GetMax())) {
            // TODO: More precise check for non-box colliders
            result.push_back(entity);
        }
    }
    
    return result;
}

std::vector<Entity> CollisionSystem::QueryRegion2D(EntityManager& entityManager,
                                                    const glm::vec2& min, const glm::vec2& max) {
    std::vector<Entity> result;
    
    auto query = entityManager.CreateQuery<ICollider2D, Transform>();
    for (auto [entity, collider, transform] : query) {
        if (!collider->isEnabled) continue;
        
        if (CollisionHelpers::AABBOverlap2D(min, max, collider->GetMin(), collider->GetMax())) {
            result.push_back(entity);
        }
    }
    
    return result;
}

std::vector<Entity> CollisionSystem::QueryRegion3D(EntityManager& entityManager,
                                                    const glm::vec3& min, const glm::vec3& max) {
    std::vector<Entity> result;
    
    auto query = entityManager.CreateQuery<ICollider3D, Transform>();
    for (auto [entity, collider, transform] : query) {
        if (!collider->isEnabled) continue;
        
        if (CollisionHelpers::AABBOverlap3D(min, max, collider->GetMin(), collider->GetMax())) {
            result.push_back(entity);
        }
    }
    
    return result;
}

bool CollisionSystem::Raycast2D(EntityManager& entityManager, const glm::vec2& origin,
                                const glm::vec2& direction, float maxDistance,
                                Entity& hitEntity, glm::vec2& hitPoint) {
    // TODO: Implement proper 2D raycast
    return false;
}

bool CollisionSystem::Raycast3D(EntityManager& entityManager, const glm::vec3& origin,
                                const glm::vec3& direction, float maxDistance,
                                Entity& hitEntity, glm::vec3& hitPoint) {
    // TODO: Implement proper 3D raycast using helpers
    return false;
}