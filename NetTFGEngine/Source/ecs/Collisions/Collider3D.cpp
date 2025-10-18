#include "SphereCollider3D.hpp"
#include "BoxCollider3D.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

// ==================== SphereCollider3D Implementation ====================

bool SphereCollider3D::CollidesWith(const ICollider3D* other, CollisionInfo& info) const {
    // Check if other is a SphereCollider3D
    if (const SphereCollider3D* otherSphere = dynamic_cast<const SphereCollider3D*>(other)) {
        // Sphere vs Sphere
        glm::vec3 center1 = GetCenter();
        glm::vec3 center2 = otherSphere->GetCenter();
        
        glm::vec3 delta = center2 - center1;
        float distanceSquared = glm::dot(delta, delta);
        float radiusSum = radius + otherSphere->radius;
        float radiusSumSquared = radiusSum * radiusSum;
        
        if (distanceSquared >= radiusSumSquared) {
            return false; // No collision
        }
        
        // Calculate collision info
        float distance = std::sqrt(distanceSquared);
        
        if (distance > 0.0001f) {
            info.normal = delta / distance;
            info.penetration = radiusSum - distance;
            info.contactPoint = center1 + delta * (radius / distance);
        } else {
            // Spheres are exactly on top of each other
            info.normal = glm::vec3(1.0f, 0.0f, 0.0f);
            info.penetration = radius;
            info.contactPoint = center1;
        }
        
        return true;
    }
    
    // Check if other is a BoxCollider3D
    if (const BoxCollider3D* otherBox = dynamic_cast<const BoxCollider3D*>(other)) {
        // Sphere vs Box
        glm::vec3 sphereCenter = GetCenter();
        glm::vec3 boxCenter = otherBox->GetCenter();
        
        // Find closest point on box to sphere
        glm::vec3 boxMin = otherBox->GetMin();
        glm::vec3 boxMax = otherBox->GetMax();
        
        // Clamp sphere center to box bounds
        glm::vec3 closestPoint;
        closestPoint.x = std::max(boxMin.x, std::min(sphereCenter.x, boxMax.x));
        closestPoint.y = std::max(boxMin.y, std::min(sphereCenter.y, boxMax.y));
        closestPoint.z = std::max(boxMin.z, std::min(sphereCenter.z, boxMax.z));
        
        // Check distance from closest point to sphere center
        glm::vec3 delta = sphereCenter - closestPoint;
        float distanceSquared = glm::dot(delta, delta);
        
        if (distanceSquared >= radius * radius) {
            return false; // No collision
        }
        
        // Calculate collision info
        float distance = std::sqrt(distanceSquared);
        
        if (distance > 0.0001f) {
            info.normal = delta / distance;
            info.penetration = radius - distance;
            info.contactPoint = closestPoint;
        } else {
            // Sphere center is inside box - find closest face
            glm::vec3 distToFaces = glm::min(
                sphereCenter - boxMin,
                boxMax - sphereCenter
            );
            
            // Find axis with minimum distance
            float minDist = distToFaces.x;
            int axis = 0;
            
            if (distToFaces.y < minDist) {
                minDist = distToFaces.y;
                axis = 1;
            }
            if (distToFaces.z < minDist) {
                minDist = distToFaces.z;
                axis = 2;
            }
            
            // Set normal based on closest axis
            info.normal = glm::vec3(0.0f);
            if (axis == 0) {
                info.normal.x = (sphereCenter.x < boxCenter.x) ? -1.0f : 1.0f;
            } else if (axis == 1) {
                info.normal.y = (sphereCenter.y < boxCenter.y) ? -1.0f : 1.0f;
            } else {
                info.normal.z = (sphereCenter.z < boxCenter.z) ? -1.0f : 1.0f;
            }
            
            info.penetration = radius + minDist;
            info.contactPoint = closestPoint;
        }
        
        return true;
    }
    
    // Unknown collider type
    return false;
}

// ==================== BoxCollider3D Implementation ====================

bool BoxCollider3D::CollidesWith(const ICollider3D* other, CollisionInfo& info) const {
    // Check if other is a SphereCollider3D
    if (const SphereCollider3D* otherSphere = dynamic_cast<const SphereCollider3D*>(other)) {
        // Box vs Sphere (use sphere's implementation and flip normal)
        bool result = otherSphere->CollidesWith(this, info);
        if (result) {
            info.normal = -info.normal;
        }
        return result;
    }
    
    // Check if other is a BoxCollider3D
    if (const BoxCollider3D* otherBox = dynamic_cast<const BoxCollider3D*>(other)) {
        // Box vs Box (AABB)
        glm::vec3 min1 = GetMin();
        glm::vec3 max1 = GetMax();
        glm::vec3 min2 = otherBox->GetMin();
        glm::vec3 max2 = otherBox->GetMax();
        
        // Check for separation
        if (max1.x < min2.x || min1.x > max2.x ||
            max1.y < min2.y || min1.y > max2.y ||
            max1.z < min2.z || min1.z > max2.z) {
            return false; // No collision
        }
        
        // Calculate overlap on each axis
        float overlapX = std::min(max1.x - min2.x, max2.x - min1.x);
        float overlapY = std::min(max1.y - min2.y, max2.y - min1.y);
        float overlapZ = std::min(max1.z - min2.z, max2.z - min1.z);
        
        // Find axis of least penetration
        float minOverlap = overlapX;
        int axis = 0;
        
        if (overlapY < minOverlap) {
            minOverlap = overlapY;
            axis = 1;
        }
        if (overlapZ < minOverlap) {
            minOverlap = overlapZ;
            axis = 2;
        }
        
        // Set collision info based on separation axis
        glm::vec3 center1 = GetCenter();
        glm::vec3 center2 = otherBox->GetCenter();
        
        info.normal = glm::vec3(0.0f);
        info.penetration = minOverlap;
        
        if (axis == 0) {
            // Separate on X axis
            float direction = (center2.x > center1.x) ? 1.0f : -1.0f;
            info.normal.x = direction;
            info.contactPoint = glm::vec3(
                (center2.x > center1.x) ? min2.x : max2.x,
                (center1.y + center2.y) * 0.5f,
                (center1.z + center2.z) * 0.5f
            );
        } else if (axis == 1) {
            // Separate on Y axis
            float direction = (center2.y > center1.y) ? 1.0f : -1.0f;
            info.normal.y = direction;
            info.contactPoint = glm::vec3(
                (center1.x + center2.x) * 0.5f,
                (center2.y > center1.y) ? min2.y : max2.y,
                (center1.z + center2.z) * 0.5f
            );
        } else {
            // Separate on Z axis
            float direction = (center2.z > center1.z) ? 1.0f : -1.0f;
            info.normal.z = direction;
            info.contactPoint = glm::vec3(
                (center1.x + center2.x) * 0.5f,
                (center1.y + center2.y) * 0.5f,
                (center2.z > center1.z) ? min2.z : max2.z
            );
        }
        
        return true;
    }
    
    // Unknown collider type
    return false;
}