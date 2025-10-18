#ifndef BOXCOLLIDER3D_HPP
#define BOXCOLLIDER3D_HPP

#include "ICollider3D.hpp"
#include "ecs/ecs_common.hpp"
#include <glm/gtc/matrix_transform.hpp>

class BoxCollider3D : public ICollider3D {
public:
    BoxCollider3D(const glm::vec3& size = glm::vec3(1.0f), 
                  const glm::vec3& offset = glm::vec3(0.0f))
        : size(size)
        , transform(nullptr)
    {
        this->offset = offset;
    }

    // Half extents (width/2, height/2, depth/2)
    glm::vec3 size;
    
    // Reference to entity's transform
    Transform* transform;

    // ICollider interface
    int GetColliderType() const override { return COLLIDER_BOX_3D; }
    
    bool CheckCollision(const ICollider* other, CollisionInfo& info) const override {
        const ICollider3D* other3D = dynamic_cast<const ICollider3D*>(other);
        if (!other3D) return false;
        return other3D->CollidesWith(this, info);
    }

    // ICollider3D generic collision
    bool CollidesWith(const ICollider3D* other, CollisionInfo& info) const override;

    // Bounds (AABB)
    glm::vec3 GetMin() const override {
        glm::vec3 center = GetCenter();
        return center - size;
    }

    glm::vec3 GetMax() const override {
        glm::vec3 center = GetCenter();
        return center + size;
    }

    glm::vec3 GetCenter() const override {
        if (!transform) return offset;
        return transform->getPosition() + offset;
    }
    
    // Get 8 corners of the box (for OBB if rotated)
    std::vector<glm::vec3> GetCorners() const {
        glm::vec3 center = GetCenter();
        
        // If no rotation, return AABB corners
        if (!transform) {
            return GetAABBCorners(center);
        }
        
        // Apply rotation for OBB
        glm::vec3 rot = transform->getRotation();
        if (rot.x == 0.0f && rot.y == 0.0f && rot.z == 0.0f) {
            return GetAABBCorners(center);
        }
        
        // Create rotation matrix
        glm::mat4 rotMat = glm::mat4(1.0f);
        rotMat = glm::rotate(rotMat, glm::radians(rot.x), glm::vec3(1, 0, 0));
        rotMat = glm::rotate(rotMat, glm::radians(rot.y), glm::vec3(0, 1, 0));
        rotMat = glm::rotate(rotMat, glm::radians(rot.z), glm::vec3(0, 0, 1));
        
        std::vector<glm::vec3> corners;
        glm::vec3 localCorners[] = {
            {-size.x, -size.y, -size.z},
            {size.x, -size.y, -size.z},
            {size.x, size.y, -size.z},
            {-size.x, size.y, -size.z},
            {-size.x, -size.y, size.z},
            {size.x, -size.y, size.z},
            {size.x, size.y, size.z},
            {-size.x, size.y, size.z}
        };
        
        for (const auto& local : localCorners) {
            glm::vec4 rotated = rotMat * glm::vec4(local, 1.0f);
            corners.push_back(center + glm::vec3(rotated));
        }
        
        return corners;
    }

private:
    std::vector<glm::vec3> GetAABBCorners(const glm::vec3& center) const {
        return {
            center + glm::vec3(-size.x, -size.y, -size.z),
            center + glm::vec3(size.x, -size.y, -size.z),
            center + glm::vec3(size.x, size.y, -size.z),
            center + glm::vec3(-size.x, size.y, -size.z),
            center + glm::vec3(-size.x, -size.y, size.z),
            center + glm::vec3(size.x, -size.y, size.z),
            center + glm::vec3(size.x, size.y, size.z),
            center + glm::vec3(-size.x, size.y, size.z)
        };
    }
};

#endif // BOXCOLLIDER3D_HPP