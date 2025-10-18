#ifndef ICOLLIDER_HPP
#define ICOLLIDER_HPP

#include "ecs/ecs.hpp"
#include <glm/glm.hpp>
#include <functional>

// Collision layer for filtering
enum class CollisionLayer : uint32_t {
    DEFAULT = 1 << 0,
    PLAYER = 1 << 1,
    ENEMY = 1 << 2,
    BULLET = 1 << 3,
    WALL = 1 << 4,
    PICKUP = 1 << 5,
    ALL = 0xFFFFFFFFu
};

// Collision callback data
struct CollisionInfo {
    Entity otherEntity;
    glm::vec3 normal;        // Collision normal
    float penetration;       // Penetration depth
    glm::vec3 contactPoint;  // Point of contact
};

// Collision callback function types
using OnCollisionEnterCallback = std::function<void(Entity self, Entity other, const CollisionInfo& info)>;
using OnCollisionStayCallback = std::function<void(Entity self, Entity other, const CollisionInfo& info)>;
using OnCollisionExitCallback = std::function<void(Entity self, Entity other)>;
using OnTriggerEnterCallback = std::function<void(Entity self, Entity other)>;
using OnTriggerStayCallback = std::function<void(Entity self, Entity other)>;
using OnTriggerExitCallback = std::function<void(Entity self, Entity other)>;

// Base collider interface
class ICollider : public IComponent {
public:
    virtual ~ICollider() = default;

    // Collision detection (pure virtual)
    virtual bool CheckCollision(const ICollider* other, CollisionInfo& info) const = 0;
    
    // Get collider type for double dispatch
    virtual int GetColliderType() const = 0;
    
    // Layer filtering
    CollisionLayer layer = CollisionLayer::DEFAULT;
    CollisionLayer collidesWith = CollisionLayer::ALL;
    
    // Trigger vs solid collider
    bool isTrigger = false;  // If true, detects but doesn't block
    
    // Enabled state
    bool isEnabled = true;
    
    // Collision callbacks (for solid colliders)
    OnCollisionEnterCallback onCollisionEnter = nullptr;
    OnCollisionStayCallback onCollisionStay = nullptr;
    OnCollisionExitCallback onCollisionExit = nullptr;
    
    // Trigger callbacks (for trigger colliders)
    OnTriggerEnterCallback onTriggerEnter = nullptr;
    OnTriggerStayCallback onTriggerStay = nullptr;
    OnTriggerExitCallback onTriggerExit = nullptr;
    
    // Helper to check if should collide with layer
    bool CanCollideWith(CollisionLayer otherLayer) const {
        return (static_cast<uint32_t>(collidesWith) & static_cast<uint32_t>(otherLayer)) != 0;
    }
    
    // Set collision callbacks (chainable)
    ICollider* SetOnCollisionEnter(OnCollisionEnterCallback callback) {
        onCollisionEnter = callback;
        return this;
    }
    
    ICollider* SetOnCollisionStay(OnCollisionStayCallback callback) {
        onCollisionStay = callback;
        return this;
    }
    
    ICollider* SetOnCollisionExit(OnCollisionExitCallback callback) {
        onCollisionExit = callback;
        return this;
    }
    
    // Set trigger callbacks (chainable)
    ICollider* SetOnTriggerEnter(OnTriggerEnterCallback callback) {
        onTriggerEnter = callback;
        return this;
    }
    
    ICollider* SetOnTriggerStay(OnTriggerStayCallback callback) {
        onTriggerStay = callback;
        return this;
    }
    
    ICollider* SetOnTriggerExit(OnTriggerExitCallback callback) {
        onTriggerExit = callback;
        return this;
    }
};

// Collider type IDs for double dispatch
enum ColliderType {
    COLLIDER_CIRCLE_2D = 0,
    COLLIDER_BOX_2D = 1,
    COLLIDER_POLYGON_2D = 2,
    COLLIDER_SPHERE_3D = 3,
    COLLIDER_BOX_3D = 4,
    COLLIDER_CAPSULE_3D = 5,
    COLLIDER_MESH_3D = 6
};

#endif // ICOLLIDER_HPP