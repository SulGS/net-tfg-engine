#ifndef ICOLLIDER_HPP
#define ICOLLIDER_HPP

#include "ecs/ecs.hpp"
#include <glm/glm.hpp>
#include <functional>

enum class CollisionLayer : uint32_t {
    DEFAULT = 1 << 0,
    PLAYER = 1 << 1,
    ENEMY = 1 << 2,
    BULLET = 1 << 3,
    WALL = 1 << 4,
    PICKUP = 1 << 5,
    ALL = 0xFFFFFFFFu
};

inline CollisionLayer operator|(CollisionLayer a, CollisionLayer b)
{
    return static_cast<CollisionLayer>(
        static_cast<uint32_t>(a) |
        static_cast<uint32_t>(b));
}

inline CollisionLayer operator&(CollisionLayer a, CollisionLayer b)
{
    return static_cast<CollisionLayer>(
        static_cast<uint32_t>(a) &
        static_cast<uint32_t>(b));
}

struct CollisionInfo {
    Entity otherEntity;
    glm::vec3 normal;
    float penetration;
    glm::vec3 contactPoint;
};

using OnCollisionEnterCallback = std::function<void(Entity self, Entity other, const CollisionInfo& info)>;
using OnCollisionStayCallback = std::function<void(Entity self, Entity other, const CollisionInfo& info)>;
using OnCollisionExitCallback = std::function<void(Entity self, Entity other)>;
using OnTriggerEnterCallback = std::function<void(Entity self, Entity other)>;
using OnTriggerStayCallback = std::function<void(Entity self, Entity other)>;
using OnTriggerExitCallback = std::function<void(Entity self, Entity other)>;

class ICollider : public IComponent {
public:
    virtual ~ICollider() = default;

    virtual bool CheckCollision(const ICollider* other, CollisionInfo& info) const = 0;

    // For double dispatch.
    virtual int GetColliderType() const = 0;

    CollisionLayer layer = CollisionLayer::DEFAULT;
    CollisionLayer collidesWith = CollisionLayer::ALL;

    bool isTrigger = false;  // If true, detects but doesn't block

    bool isEnabled = true;

    OnCollisionEnterCallback onCollisionEnter = nullptr;
    OnCollisionStayCallback onCollisionStay = nullptr;
    OnCollisionExitCallback onCollisionExit = nullptr;

    OnTriggerEnterCallback onTriggerEnter = nullptr;
    OnTriggerStayCallback onTriggerStay = nullptr;
    OnTriggerExitCallback onTriggerExit = nullptr;

    bool CanCollideWith(CollisionLayer otherLayer) const {
        return (static_cast<uint32_t>(collidesWith) & static_cast<uint32_t>(otherLayer)) != 0;
    }

    // Setters return `this` so calls can be chained.
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

// IDs for double dispatch
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