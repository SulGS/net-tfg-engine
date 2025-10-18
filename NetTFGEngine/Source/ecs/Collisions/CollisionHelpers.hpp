#ifndef COLLISION_HELPERS_HPP
#define COLLISION_HELPERS_HPP

#include <glm/glm.hpp>
#include <vector>
#include <cmath>

namespace CollisionHelpers {

// ==================== Point Tests ====================

inline bool PointInCircle(const glm::vec2& point, const glm::vec2& center, float radius) {
    glm::vec2 delta = point - center;
    return glm::dot(delta, delta) <= radius * radius;
}

inline bool PointInSphere(const glm::vec3& point, const glm::vec3& center, float radius) {
    glm::vec3 delta = point - center;
    return glm::dot(delta, delta) <= radius * radius;
}

inline bool PointInAABB2D(const glm::vec2& point, const glm::vec2& min, const glm::vec2& max) {
    return point.x >= min.x && point.x <= max.x &&
           point.y >= min.y && point.y <= max.y;
}

inline bool PointInAABB3D(const glm::vec3& point, const glm::vec3& min, const glm::vec3& max) {
    return point.x >= min.x && point.x <= max.x &&
           point.y >= min.y && point.y <= max.y &&
           point.z >= min.z && point.z <= max.z;
}

// ==================== Line/Ray Tests ====================

// Ray-Sphere intersection
inline bool RaySphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                      const glm::vec3& sphereCenter, float radius,
                      float& t) {
    glm::vec3 oc = rayOrigin - sphereCenter;
    float a = glm::dot(rayDir, rayDir);
    float b = 2.0f * glm::dot(oc, rayDir);
    float c = glm::dot(oc, oc) - radius * radius;
    float discriminant = b * b - 4 * a * c;
    
    if (discriminant < 0) {
        return false;
    }
    
    t = (-b - std::sqrt(discriminant)) / (2.0f * a);
    return t >= 0.0f;
}

// Ray-AABB intersection (3D)
inline bool RayAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                    const glm::vec3& boxMin, const glm::vec3& boxMax,
                    float& tMin, float& tMax) {
    tMin = 0.0f;
    tMax = std::numeric_limits<float>::max();
    
    for (int i = 0; i < 3; i++) {
        if (std::abs(rayDir[i]) < 0.0001f) {
            // Ray is parallel to slab
            if (rayOrigin[i] < boxMin[i] || rayOrigin[i] > boxMax[i]) {
                return false;
            }
        } else {
            float ood = 1.0f / rayDir[i];
            float t1 = (boxMin[i] - rayOrigin[i]) * ood;
            float t2 = (boxMax[i] - rayOrigin[i]) * ood;
            
            if (t1 > t2) std::swap(t1, t2);
            
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            
            if (tMin > tMax) return false;
        }
    }
    
    return true;
}

// ==================== Closest Point Functions ====================

inline glm::vec2 ClosestPointOnLineSegment2D(const glm::vec2& point,
                                              const glm::vec2& lineStart,
                                              const glm::vec2& lineEnd) {
    glm::vec2 line = lineEnd - lineStart;
    float t = glm::dot(point - lineStart, line) / glm::dot(line, line);
    t = std::max(0.0f, std::min(1.0f, t));
    return lineStart + t * line;
}

inline glm::vec3 ClosestPointOnLineSegment3D(const glm::vec3& point,
                                              const glm::vec3& lineStart,
                                              const glm::vec3& lineEnd) {
    glm::vec3 line = lineEnd - lineStart;
    float t = glm::dot(point - lineStart, line) / glm::dot(line, line);
    t = std::max(0.0f, std::min(1.0f, t));
    return lineStart + t * line;
}

inline glm::vec3 ClosestPointOnAABB(const glm::vec3& point,
                                    const glm::vec3& boxMin,
                                    const glm::vec3& boxMax) {
    return glm::vec3(
        std::max(boxMin.x, std::min(point.x, boxMax.x)),
        std::max(boxMin.y, std::min(point.y, boxMax.y)),
        std::max(boxMin.z, std::min(point.z, boxMax.z))
    );
}

// ==================== SAT (Separating Axis Theorem) Helpers ====================

// Project polygon onto axis
inline void ProjectPolygon(const std::vector<glm::vec2>& vertices,
                          const glm::vec2& axis,
                          float& min, float& max) {
    min = max = glm::dot(vertices[0], axis);
    
    for (size_t i = 1; i < vertices.size(); i++) {
        float projection = glm::dot(vertices[i], axis);
        if (projection < min) min = projection;
        if (projection > max) max = projection;
    }
}

// Check if two ranges overlap
inline bool RangesOverlap(float min1, float max1, float min2, float max2) {
    return !(max1 < min2 || max2 < min1);
}

// Get overlap amount
inline float GetOverlap(float min1, float max1, float min2, float max2) {
    return std::min(max1, max2) - std::max(min1, min2);
}

// ==================== Distance Functions ====================

inline float DistanceSquared2D(const glm::vec2& a, const glm::vec2& b) {
    glm::vec2 delta = b - a;
    return glm::dot(delta, delta);
}

inline float Distance2D(const glm::vec2& a, const glm::vec2& b) {
    return std::sqrt(DistanceSquared2D(a, b));
}

inline float DistanceSquared3D(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 delta = b - a;
    return glm::dot(delta, delta);
}

inline float Distance3D(const glm::vec3& a, const glm::vec3& b) {
    return std::sqrt(DistanceSquared3D(a, b));
}

// ==================== AABB Overlap Tests ====================

inline bool AABBOverlap2D(const glm::vec2& min1, const glm::vec2& max1,
                          const glm::vec2& min2, const glm::vec2& max2) {
    return !(max1.x < min2.x || min1.x > max2.x ||
             max1.y < min2.y || min1.y > max2.y);
}

inline bool AABBOverlap3D(const glm::vec3& min1, const glm::vec3& max1,
                          const glm::vec3& min2, const glm::vec3& max2) {
    return !(max1.x < min2.x || min1.x > max2.x ||
             max1.y < min2.y || min1.y > max2.y ||
             max1.z < min2.z || min1.z > max2.z);
}

// Expand AABB by radius (useful for broadphase)
inline void ExpandAABB2D(glm::vec2& min, glm::vec2& max, float radius) {
    min -= glm::vec2(radius);
    max += glm::vec2(radius);
}

inline void ExpandAABB3D(glm::vec3& min, glm::vec3& max, float radius) {
    min -= glm::vec3(radius);
    max += glm::vec3(radius);
}

// ==================== Utility Functions ====================

// Safe normalize (returns zero vector if input is too small)
inline glm::vec2 SafeNormalize2D(const glm::vec2& v, const glm::vec2& fallback = glm::vec2(1, 0)) {
    float lengthSq = glm::dot(v, v);
    if (lengthSq < 0.0001f) return fallback;
    return v / std::sqrt(lengthSq);
}

inline glm::vec3 SafeNormalize3D(const glm::vec3& v, const glm::vec3& fallback = glm::vec3(1, 0, 0)) {
    float lengthSq = glm::dot(v, v);
    if (lengthSq < 0.0001f) return fallback;
    return v / std::sqrt(lengthSq);
}

// Clamp value between min and max
template<typename T>
inline T Clamp(T value, T min, T max) {
    return std::max(min, std::min(value, max));
}

} // namespace CollisionHelpers

#endif // COLLISION_HELPERS_HPP