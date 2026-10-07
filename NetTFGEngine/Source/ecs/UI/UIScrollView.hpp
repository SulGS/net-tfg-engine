#ifndef UISCROLLVIEW_HPP
#define UISCROLLVIEW_HPP

#include "ecs/ecs_common.hpp"
#include "UIElement.hpp"
#include <algorithm>
#include <cmath>

// Vertical scroll area. Goes on an entity whose UIElement is the visible window (the "viewport"); the scrolled widgets
// are ordinary UI entities that carry a UIScrollChild pointing at it (UIScroll::Attach). UIUpdateSystem moves them
// (UIElement::position = their base position minus the scroll), and UI rendering and hit testing clip them to the
// viewport, so a widget scrolled out of view can't be seen nor clicked, but stays reachable by keyboard/gamepad
// navigation: focusing it scrolls it into view.
//
// Input: mouse wheel over the viewport, dragging the thumb or clicking the track (one page), Page Up/Down, and the
// gamepad's right stick (UIAction::ScrollUp/ScrollDown). The bar is only drawn when the content doesn't fit.
//
// The content height is, by default, measured every frame from the VISIBLE children (bottom-most one + padding), so
// one viewport can serve several pages that are shown one at a time (e.g. tabs); call ScrollTo(0, true) when switching.
class UIScrollView : public IComponent {
public:
    float scroll = 0.0f;            // current offset in reference pixels (0 = top)
    float targetScroll = 0.0f;      // where it's easing to

    bool  autoContentHeight = true;
    float contentHeight = 0.0f;     // measured (autoContentHeight) or set by the owner
    float contentPadding = 8.0f;    // added below the last child, and kept around a focused one when scrolling to it

    float wheelStep = 60.0f;        // pixels per wheel notch
    float stickSpeed = 900.0f;      // pixels per second with the right stick fully pushed
    float smoothing = 18.0f;        // ease rate (1/s); 0 = jump straight to the target
    bool  isInteractable = true;

    // Bar look
    float barWidth = 8.0f;
    float barMargin = 4.0f;         // gap between the bar and the viewport's right edge
    float minThumbLength = 32.0f;
    glm::vec4 trackColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.08f);
    glm::vec4 thumbColor = glm::vec4(0.75f, 0.78f, 0.85f, 0.55f);
    glm::vec4 thumbHoverColor = glm::vec4(0.90f, 0.92f, 1.0f, 0.80f);
    glm::vec4 thumbDragColor = glm::vec4(1.0f, 0.78f, 0.2f, 0.90f);

    // Interaction state (UIUpdateSystem)
    bool thumbHovered = false;
    bool thumbDragging = false;
    float dragGrab = 0.0f;          // where on the thumb it was grabbed, from its top

    float MaxScroll(float viewHeight) const { return std::max(0.0f, contentHeight - viewHeight); }
    bool  CanScroll(float viewHeight) const { return contentHeight > viewHeight + 0.5f; }

    void ScrollTo(float offset, bool immediate = false) {
        targetScroll = offset;
        if (immediate) scroll = offset;
    }
    void ScrollBy(float delta) { targetScroll += delta; }

    // Track and thumb rectangles (x, y, w, h) in reference pixels, for a viewport at viewPos/viewSize.
    glm::vec4 TrackRect(const glm::vec2& viewPos, const glm::vec2& viewSize) const {
        return glm::vec4(viewPos.x + viewSize.x - barMargin - barWidth, viewPos.y + barMargin,
            barWidth, std::max(0.0f, viewSize.y - 2.0f * barMargin));
    }
    glm::vec4 ThumbRect(const glm::vec2& viewPos, const glm::vec2& viewSize) const {
        const glm::vec4 track = TrackRect(viewPos, viewSize);
        const float maxScroll = MaxScroll(viewSize.y);
        if (maxScroll <= 0.0f || contentHeight <= 0.0f) return track;
        const float length = std::clamp(track.w * viewSize.y / contentHeight, std::min(minThumbLength, track.w), track.w);
        const float t = std::clamp(scroll / maxScroll, 0.0f, 1.0f);
        return glm::vec4(track.x, track.y + (track.w - length) * t, track.z, length);
    }
};

// On each scrolled widget. basePosition is its UIElement::position with the view scrolled to the top.
class UIScrollChild : public IComponent {
public:
    Entity view = 0;
    glm::vec2 basePosition = glm::vec2(0.0f);

    UIScrollChild() {}
    UIScrollChild(Entity v, glm::vec2 base) : view(v), basePosition(base) {}
};

namespace UIScroll {
    inline void Register(EntityManager& em) {
        em.RegisterComponentType<UIScrollView>();
        em.RegisterComponentType<UIScrollChild>();
    }

    // Puts `child` in the scroll view `view`; its current UIElement::position becomes the top-scrolled position.
    inline UIScrollChild* Attach(EntityManager& em, Entity child, Entity view) {
        const UIElement* element = em.GetComponent<UIElement>(child);
        return em.AddComponent<UIScrollChild>(child,
            UIScrollChild{ view, element ? element->position : glm::vec2(0.0f) });
    }

    inline bool RectContains(const glm::vec4& r, const glm::vec2& p) {
        return p.x >= r.x && p.x <= r.x + r.z && p.y >= r.y && p.y <= r.y + r.w;
    }

    // Viewport rectangle (x, y, w, h) a widget is clipped to, if it's in a visible scroll view.
    inline bool GetClipRect(EntityManager& em, Entity entity, int refW, int refH, glm::vec4& rect) {
        const UIScrollChild* child = em.GetComponent<UIScrollChild>(entity);
        if (!child || child->view == 0) return false;
        const UIElement* view = em.GetComponent<UIElement>(child->view);
        if (!view || !em.GetComponent<UIScrollView>(child->view)) return false;
        const glm::vec2 pos = view->GetScreenPosition(refW, refH);
        rect = glm::vec4(pos, view->size);
        return true;
    }

    // False when `point` falls on a part of the widget that its scroll view clips away (always true outside of one).
    inline bool IsPointVisible(EntityManager& em, Entity entity, const glm::vec2& point, int refW, int refH) {
        glm::vec4 clip;
        return !GetClipRect(em, entity, refW, refH, clip) || RectContains(clip, point);
    }

    // True when the widget lies entirely outside its scroll view's window.
    inline bool IsFullyClipped(EntityManager& em, Entity entity, int refW, int refH) {
        glm::vec4 clip;
        if (!GetClipRect(em, entity, refW, refH, clip)) return false;
        const UIElement* element = em.GetComponent<UIElement>(entity);
        if (!element) return true;
        const glm::vec2 pos = element->GetScreenPosition(refW, refH);
        return pos.y + element->size.y <= clip.y || pos.y >= clip.y + clip.w
            || pos.x + element->size.x <= clip.x || pos.x >= clip.x + clip.z;
    }
}

#endif // UISCROLLVIEW_HPP
