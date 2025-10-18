#ifndef SPHERECOLLIDER3D_HPP
#define SPHERECOLLIDER3D_HPP

#include "ICollider3D.hpp"
#include "ecs/ecs_common.hpp"

class SphereCollider3D : public ICollider3D {
public:
    SphereCollider3D(float radius = 1.0f, const glm::vec3& offset = glm::vec3(0.0f))
        : radius(radius)
        , transform(nullptr)
    {
        this->offset = offset;
    }

    // Radius
    float radius;
    
    // Reference to entity's transform
    Transform* transform;

    // ICollider interface
    int GetColliderType() const override { return COLLIDER_SPHERE_3D; }
    
    bool CheckCollision(const ICollider* other, CollisionInfo& info) const override {
        const ICollider3D* other3D = dynamic_cast<const ICollider3D*>(other);
        if (!other3D) return false;
        return other3D->CollidesWith(this, info);
    }

    // ICollider3D generic collision
    bool CollidesWith(const ICollider3D* other, CollisionInfo& info) const override;

    // Bounds
    glm::vec3 GetMin() const override {
        glm::vec3 center = GetCenter();
        return center - glm::vec3(radius);
    }

    glm::vec3 GetMax() const override {
        glm::vec3 center = GetCenter();
        return center + glm::vec3(radius);
    }

    glm::vec3 GetCenter() const override {
        if (!transform) return offset;
        return transform->getPosition() + offset;
    }
};

#endif // SPHERECOLLIDER3D_HPP