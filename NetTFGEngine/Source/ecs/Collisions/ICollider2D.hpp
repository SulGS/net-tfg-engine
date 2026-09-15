#ifndef ICOLLIDER2D_HPP
#define ICOLLIDER2D_HPP

#include "ICollider.hpp"

class ICollider2D : public ICollider {
public:
    virtual ~ICollider2D() = default;

    virtual bool CollidesWith(const ICollider2D* other, CollisionInfo& info) const = 0;

    // For broad-phase.
    virtual glm::vec2 GetMin() const = 0;
    virtual glm::vec2 GetMax() const = 0;

    // World space.
    virtual glm::vec2 GetCenter() const = 0;

    glm::vec2 offset = glm::vec2(0.0f);
};

#endif // ICOLLIDER2D_HPP