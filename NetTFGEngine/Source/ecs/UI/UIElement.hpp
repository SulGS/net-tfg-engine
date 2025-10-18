#ifndef UIELEMENT_HPP
#define UIELEMENT_HPP

#include "ecs/ecs_common.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <string>
#include <functional>

// UI Anchor positions
enum class UIAnchor {
    TOP_LEFT,
    TOP_CENTER,
    TOP_RIGHT,
    CENTER_LEFT,
    CENTER,
    CENTER_RIGHT,
    BOTTOM_LEFT,
    BOTTOM_CENTER,
    BOTTOM_RIGHT
};

// Base UI element component
class UIElement : public IComponent {
public:
    UIElement() 
        : position(0.0f, 0.0f)
        , size(100.0f, 50.0f)
        , anchor(UIAnchor::TOP_LEFT)
        , pivot(0.0f, 0.0f)
        , isVisible(true)
        , layer(0)
        , opacity(1.0f)
    {}

    // Position in screen space (pixels from anchor)
    glm::vec2 position;
    
    // Size in pixels
    glm::vec2 size;
    
    // Anchor point on screen
    UIAnchor anchor;
    
    // Pivot point (0,0 = top-left, 0.5,0.5 = center, 1,1 = bottom-right)
    glm::vec2 pivot;
    
    // Visibility
    bool isVisible;
    
    // Render order (higher = rendered on top)
    int layer;
    
    // Opacity (0.0 = transparent, 1.0 = opaque)
    float opacity;
    
    // Get screen position based on anchor
    glm::vec2 GetScreenPosition(int screenWidth, int screenHeight) const {
        glm::vec2 anchorPos = GetAnchorPosition(screenWidth, screenHeight);
        glm::vec2 pivotOffset = glm::vec2(size.x * pivot.x, size.y * pivot.y);
        return anchorPos + position - pivotOffset;
    }
    
    // Get bounding box in screen space
    glm::vec4 GetBounds(int screenWidth, int screenHeight) const {
        glm::vec2 pos = GetScreenPosition(screenWidth, screenHeight);
        return glm::vec4(pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    }
    
    // Check if point is inside element
    bool Contains(const glm::vec2& point, int screenWidth, int screenHeight) const {
        glm::vec4 bounds = GetBounds(screenWidth, screenHeight);
        return point.x >= bounds.x && point.x <= bounds.z &&
               point.y >= bounds.y && point.y <= bounds.w;
    }

private:
    glm::vec2 GetAnchorPosition(int screenWidth, int screenHeight) const {
        switch (anchor) {
            case UIAnchor::TOP_LEFT:     return glm::vec2(0, 0);
            case UIAnchor::TOP_CENTER:   return glm::vec2(screenWidth / 2, 0);
            case UIAnchor::TOP_RIGHT:    return glm::vec2(screenWidth, 0);
            case UIAnchor::CENTER_LEFT:  return glm::vec2(0, screenHeight / 2);
            case UIAnchor::CENTER:       return glm::vec2(screenWidth / 2, screenHeight / 2);
            case UIAnchor::CENTER_RIGHT: return glm::vec2(screenWidth, screenHeight / 2);
            case UIAnchor::BOTTOM_LEFT:  return glm::vec2(0, screenHeight);
            case UIAnchor::BOTTOM_CENTER:return glm::vec2(screenWidth / 2, screenHeight);
            case UIAnchor::BOTTOM_RIGHT: return glm::vec2(screenWidth, screenHeight);
            default: return glm::vec2(0, 0);
        }
    }
};

#endif // UIELEMENT_HPP