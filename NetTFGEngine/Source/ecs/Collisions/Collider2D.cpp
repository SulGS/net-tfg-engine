#include "CircleCollider2D.hpp"
#include "BoxCollider2D.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

// ==================== CircleCollider2D Implementation ====================

bool CircleCollider2D::CollidesWith(const ICollider2D* other, CollisionInfo& info) const {
    // Check if other is a CircleCollider2D
    if (const CircleCollider2D* otherCircle = dynamic_cast<const CircleCollider2D*>(other)) {
        // Circle vs Circle
        glm::vec2 center1 = GetCenter();
        glm::vec2 center2 = otherCircle->GetCenter();
        
        glm::vec2 delta = center2 - center1;
        float distanceSquared = glm::dot(delta, delta);
        float radiusSum = radius + otherCircle->radius;
        float radiusSumSquared = radiusSum * radiusSum;
        
        if (distanceSquared >= radiusSumSquared) {
            return false; // No collision
        }
        
        // Calculate collision info
        float distance = std::sqrt(distanceSquared);
        
        if (distance > 0.0001f) {
            info.normal = glm::vec3(delta / distance, 0.0f);
            info.penetration = radiusSum - distance;
            info.contactPoint = glm::vec3(center1 + delta * (radius / distance), 0.0f);
        } else {
            // Circles are exactly on top of each other
            info.normal = glm::vec3(1.0f, 0.0f, 0.0f);
            info.penetration = radius;
            info.contactPoint = glm::vec3(center1, 0.0f);
        }
        
        return true;
    }
    
    // Check if other is a BoxCollider2D
    if (const BoxCollider2D* otherBox = dynamic_cast<const BoxCollider2D*>(other)) {
        // Circle vs Box
        glm::vec2 circleCenter = GetCenter();
        glm::vec2 boxCenter = otherBox->GetCenter();
        
        // Find closest point on box to circle
        glm::vec2 boxMin = otherBox->GetMin();
        glm::vec2 boxMax = otherBox->GetMax();
        
        // Clamp circle center to box bounds
        glm::vec2 closestPoint;
        closestPoint.x = std::max(boxMin.x, std::min(circleCenter.x, boxMax.x));
        closestPoint.y = std::max(boxMin.y, std::min(circleCenter.y, boxMax.y));
        
        // Check distance from closest point to circle center
        glm::vec2 delta = circleCenter - closestPoint;
        float distanceSquared = glm::dot(delta, delta);
        
        if (distanceSquared >= radius * radius) {
            return false; // No collision
        }
        
        // Calculate collision info
        float distance = std::sqrt(distanceSquared);
        
        if (distance > 0.0001f) {
            info.normal = glm::vec3(delta / distance, 0.0f);
            info.penetration = radius - distance;
            info.contactPoint = glm::vec3(closestPoint, 0.0f);
        } else {
            // Circle center is inside box - find closest edge
            glm::vec2 distToEdges = glm::min(
                circleCenter - boxMin,
                boxMax - circleCenter
            );
            
            if (distToEdges.x < distToEdges.y) {
                // Closest to left or right edge
                float sign = (circleCenter.x < boxCenter.x) ? -1.0f : 1.0f;
                info.normal = glm::vec3(sign, 0.0f, 0.0f);
                info.penetration = radius + distToEdges.x;
            } else {
                // Closest to top or bottom edge
                float sign = (circleCenter.y < boxCenter.y) ? -1.0f : 1.0f;
                info.normal = glm::vec3(0.0f, sign, 0.0f);
                info.penetration = radius + distToEdges.y;
            }
            
            info.contactPoint = glm::vec3(closestPoint, 0.0f);
        }
        
        return true;
    }
    
    // Unknown collider type
    return false;
}

// ==================== BoxCollider2D Implementation ====================

bool BoxCollider2D::CollidesWith(const ICollider2D* other, CollisionInfo& info) const {
    // Check if other is a CircleCollider2D
    if (const CircleCollider2D* otherCircle = dynamic_cast<const CircleCollider2D*>(other)) {
        // Box vs Circle (use circle's implementation and flip normal)
        bool result = otherCircle->CollidesWith(this, info);
        if (result) {
            info.normal = -info.normal;
        }
        return result;
    }
    
    // Check if other is a BoxCollider2D
    if (const BoxCollider2D* otherBox = dynamic_cast<const BoxCollider2D*>(other)) {
        // Box vs Box (AABB)
        glm::vec2 min1 = GetMin();
        glm::vec2 max1 = GetMax();
        glm::vec2 min2 = otherBox->GetMin();
        glm::vec2 max2 = otherBox->GetMax();
        
        // Check for separation
        if (max1.x < min2.x || min1.x > max2.x ||
            max1.y < min2.y || min1.y > max2.y) {
            return false; // No collision
        }
        
        // Calculate overlap on each axis
        float overlapX = std::min(max1.x - min2.x, max2.x - min1.x);
        float overlapY = std::min(max1.y - min2.y, max2.y - min1.y);
        
        // Find axis of least penetration
        if (overlapX < overlapY) {
            // Separate on X axis
            glm::vec2 center1 = GetCenter();
            glm::vec2 center2 = otherBox->GetCenter();
            float direction = (center2.x > center1.x) ? 1.0f : -1.0f;
            
            info.normal = glm::vec3(direction, 0.0f, 0.0f);
            info.penetration = overlapX;
            info.contactPoint = glm::vec3(
                (center2.x > center1.x) ? min2.x : max2.x,
                (center1.y + center2.y) * 0.5f,
                0.0f
            );
        } else {
            // Separate on Y axis
            glm::vec2 center1 = GetCenter();
            glm::vec2 center2 = otherBox->GetCenter();
            float direction = (center2.y > center1.y) ? 1.0f : -1.0f;
            
            info.normal = glm::vec3(0.0f, direction, 0.0f);
            info.penetration = overlapY;
            info.contactPoint = glm::vec3(
                (center1.x + center2.x) * 0.5f,
                (center2.y > center1.y) ? min2.y : max2.y,
                0.0f
            );
        }
        
        return true;
    }
    
    // Unknown collider type
    return false;
}