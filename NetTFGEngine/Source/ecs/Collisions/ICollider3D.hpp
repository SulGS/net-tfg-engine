#ifndef ICOLLIDER3D_HPP
#define ICOLLIDER3D_HPP

#include "ICollider.hpp"

// Base 3D collider interface
class ICollider3D : public ICollider {
public:
    virtual ~ICollider3D() = default;

    // Generic collision check - override in concrete classes
    virtual bool CollidesWith(const ICollider3D* other, CollisionInfo& info) const = 0;
    
    // Get 3D bounds (for broad phase)
    virtual glm::vec3 GetMin() const = 0;
    virtual glm::vec3 GetMax() const = 0;
    
    // Get center in world space
    virtual glm::vec3 GetCenter() const = 0;
    
    // Offset from entity position
    glm::vec3 offset = glm::vec3(0.0f);
};

#endif // ICOLLIDER3D_HPP