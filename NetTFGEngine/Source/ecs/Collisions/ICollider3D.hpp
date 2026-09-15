#ifndef ICOLLIDER3D_HPP
#define ICOLLIDER3D_HPP

#include "ICollider.hpp"

class ICollider3D : public ICollider {
public:
    virtual ~ICollider3D() = default;

    virtual bool CollidesWith(const ICollider3D* other, CollisionInfo& info) const = 0;

    // For broad-phase.
    virtual glm::vec3 GetMin() const = 0;
    virtual glm::vec3 GetMax() const = 0;

    // World space.
    virtual glm::vec3 GetCenter() const = 0;

    glm::vec3 offset = glm::vec3(0.0f);
};

#endif // ICOLLIDER3D_HPP