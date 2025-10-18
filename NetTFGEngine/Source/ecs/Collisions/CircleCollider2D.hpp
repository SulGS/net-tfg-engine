#ifndef CIRCLECOLLIDER2D_HPP
#define CIRCLECOLLIDER2D_HPP

#include "ICollider2D.hpp"
#include "ecs/ecs_common.hpp"

class CircleCollider2D : public ICollider2D {
public:
    CircleCollider2D(float radius = 1.0f, const glm::vec2& offset = glm::vec2(0.0f))
        : radius(radius)
        , transform(nullptr)
    {
        this->offset = offset;
    }

    // Radius
    float radius;
    
    // Reference to entity's transform (set by collision system)
    Transform* transform;

    // ICollider interface
    int GetColliderType() const override { return COLLIDER_CIRCLE_2D; }
    
    bool CheckCollision(const ICollider* other, CollisionInfo& info) const override {
        const ICollider2D* other2D = dynamic_cast<const ICollider2D*>(other);
        if (!other2D) return false;
        return other2D->CollidesWith(this, info);
    }

    // ICollider2D generic collision
    bool CollidesWith(const ICollider2D* other, CollisionInfo& info) const override;

    // Bounds
    glm::vec2 GetMin() const override {
        glm::vec2 center = GetCenter();
        return center - glm::vec2(radius);
    }

    glm::vec2 GetMax() const override {
        glm::vec2 center = GetCenter();
        return center + glm::vec2(radius);
    }

    glm::vec2 GetCenter() const override {
        if (!transform) return offset;
        glm::vec3 pos = transform->getPosition();
        return glm::vec2(pos.x, pos.y) + offset;
    }
};

#endif // CIRCLECOLLIDER2D_HPP