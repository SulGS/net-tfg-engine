#ifndef ICOLLIDER2D_HPP
#define ICOLLIDER2D_HPP

#include "ICollider.hpp"

// Base 2D collider interface
class ICollider2D : public ICollider {
public:
    virtual ~ICollider2D() = default;

    // Generic collision check - override in concrete classes
    virtual bool CollidesWith(const ICollider2D* other, CollisionInfo& info) const = 0;
    
    // Get 2D bounds (for broad phase)
    virtual glm::vec2 GetMin() const = 0;
    virtual glm::vec2 GetMax() const = 0;
    
    // Get center in world space
    virtual glm::vec2 GetCenter() const = 0;
    
    // Offset from entity position
    glm::vec2 offset = glm::vec2(0.0f);
};

#endif // ICOLLIDER2D_HPP