#ifndef BOXCOLLIDER2D_HPP
#define BOXCOLLIDER2D_HPP

#include "ICollider2D.hpp"
#include "ecs/ecs_common.hpp"

class BoxCollider2D : public ICollider2D {
public:
    BoxCollider2D(const glm::vec2& size = glm::vec2(1.0f), 
                  const glm::vec2& offset = glm::vec2(0.0f))
        : size(size)
        , transform(nullptr)
    {
        this->offset = offset;
    }

    // Half extents (width/2, height/2)
    glm::vec2 size;
    
    // Reference to entity's transform
    Transform* transform;

    // ICollider interface
    int GetColliderType() const override { return COLLIDER_BOX_2D; }
    
    bool CheckCollision(const ICollider* other, CollisionInfo& info) const override {
        const ICollider2D* other2D = dynamic_cast<const ICollider2D*>(other);
        if (!other2D) return false;
        return other2D->CollidesWith(this, info);
    }

    // ICollider2D generic collision
    bool CollidesWith(const ICollider2D* other, CollisionInfo& info) const override;

    // Bounds (AABB)
    glm::vec2 GetMin() const override {
        glm::vec2 center = GetCenter();
        return center - size;
    }

    glm::vec2 GetMax() const override {
        glm::vec2 center = GetCenter();
        return center + size;
    }

    glm::vec2 GetCenter() const override {
        if (!transform) return offset;
        glm::vec3 pos = transform->getPosition();
        return glm::vec2(pos.x, pos.y) + offset;
    }
    
    // Get corners (useful for OBB if entity is rotated)
    std::vector<glm::vec2> GetCorners() const {
        glm::vec2 center = GetCenter();
        
        // If no rotation, return AABB corners
        if (!transform || transform->getRotation().z == 0.0f) {
            return {
                center + glm::vec2(-size.x, -size.y),
                center + glm::vec2(size.x, -size.y),
                center + glm::vec2(size.x, size.y),
                center + glm::vec2(-size.x, size.y)
            };
        }
        
        // Apply rotation for OBB
        float angle = glm::radians(transform->getRotation().z);
        float c = cos(angle);
        float s = sin(angle);
        
        std::vector<glm::vec2> corners;
        glm::vec2 localCorners[] = {
            {-size.x, -size.y},
            {size.x, -size.y},
            {size.x, size.y},
            {-size.x, size.y}
        };
        
        for (const auto& local : localCorners) {
            glm::vec2 rotated;
            rotated.x = local.x * c - local.y * s;
            rotated.y = local.x * s + local.y * c;
            corners.push_back(center + rotated);
        }
        
        return corners;
    }
};

#endif // BOXCOLLIDER2D_HPP