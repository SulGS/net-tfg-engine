#ifndef UISLIDER_HPP
#define UISLIDER_HPP

#include "ecs/ecs_common.hpp"
#include <string>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "OpenGL/OpenGLIncludes.hpp"

enum class SliderState {
    NORMAL,
    HOVERED,
    DRAGGING,
    DISABLED
};

enum class SliderOrientation {
    HORIZONTAL,
    VERTICAL
};

// Slider/value bar. Geometry lives here so UIRenderSystem and UIUpdateSystem always agree on track/handle position. Rects are glm::vec4(x, y, width, height).
class UISlider : public IComponent {
public:
    UISlider(float minV = 0.0f, float maxV = 1.0f, float startValue = 0.0f)
        : id("")
        , value(startValue)
        , minValue(minV)
        , maxValue(maxV)
        , step(0.0f)
        , orientation(SliderOrientation::HORIZONTAL)
        , state(SliderState::NORMAL)
        , isInteractable(true)
        , trackThickness(6.0f)
        , handleWidth(14.0f)          // extent ALONG the slider axis
        , handleHeight(22.0f)         // extent ACROSS the slider axis
        , borderWidth(1.0f)
        , trackColor(0.75f, 0.75f, 0.75f, 1.0f)
        , fillColor(0.3f, 0.5f, 0.9f, 1.0f)
        , handleColor(0.95f, 0.95f, 0.95f, 1.0f)
        , handleHoverColor(1.0f, 1.0f, 1.0f, 1.0f)
        , handleDraggingColor(0.85f, 0.9f, 1.0f, 1.0f)
        , handleBorderColor(0.35f, 0.35f, 0.35f, 1.0f)
        , disabledTrackColor(0.6f, 0.6f, 0.6f, 0.5f)
        , disabledFillColor(0.5f, 0.5f, 0.5f, 0.5f)
        , disabledHandleColor(0.7f, 0.7f, 0.7f, 0.5f)
        , showValue(false)
        , decimals(2)
        , valueSuffix("")
        , fontName("default")
        , fontSize(14.0f)
        , textColor(0.0f, 0.0f, 0.0f, 1.0f)
        , valueLabelWidth(52.0f)
        , labelGap(6.0f)
    {
        value = SnapValue(startValue);
    }

    std::string id;

    float value;
    float minValue;
    float maxValue;
    float step;                       // 0 = continuous, >0 = snap to increments

    SliderOrientation orientation;
    SliderState state;
    bool isInteractable;

    float trackThickness;
    float handleWidth;
    float handleHeight;
    float borderWidth;

    glm::vec4 trackColor;
    glm::vec4 fillColor;
    glm::vec4 handleColor;
    glm::vec4 handleHoverColor;
    glm::vec4 handleDraggingColor;
    glm::vec4 handleBorderColor;
    glm::vec4 disabledTrackColor;
    glm::vec4 disabledFillColor;
    glm::vec4 disabledHandleColor;

    bool showValue;
    int decimals;
    std::string valueSuffix;          // e.g. "%", " dB"
    std::string fontName;
    float fontSize;
    glm::vec4 textColor;
    float valueLabelWidth;            // reserved space for the label
    float labelGap;

    std::function<void(float)> onValueChanged = nullptr;
    std::function<void()> onDragStart = nullptr;
    std::function<void(float)> onDragEnd = nullptr;
    // Optional custom formatting for the value label
    std::function<std::string(float)> valueFormatter = nullptr;

    // value

    float GetRange() const { return maxValue - minValue; }

    float GetNormalizedValue() const {
        float range = GetRange();
        if (std::fabs(range) < 1e-6f) return 0.0f;
        float t = (value - minValue) / range;
        return std::min(std::max(t, 0.0f), 1.0f);
    }

    // Clamp to [min,max] and snap to `step` if one is set
    float SnapValue(float v) const {
        if (step > 0.0f) {
            float steps = std::round((v - minValue) / step);
            v = minValue + steps * step;
        }
        float lo = std::min(minValue, maxValue);
        float hi = std::max(minValue, maxValue);
        return std::min(std::max(v, lo), hi);
    }

    // Returns true if the value actually changed
    bool SetValue(float newValue, bool notify = true) {
        float snapped = SnapValue(newValue);
        if (std::fabs(snapped - value) < 1e-6f) return false;
        value = snapped;
        if (notify && onValueChanged) onValueChanged(value);
        return true;
    }

    bool SetNormalizedValue(float t, bool notify = true) {
        t = std::min(std::max(t, 0.0f), 1.0f);
        return SetValue(minValue + t * GetRange(), notify);
    }

    void SetRange(float minV, float maxV, bool notify = false) {
        minValue = minV;
        maxValue = maxV;
        SetValue(value, notify);
    }

    // direction: +1 / -1. Uses `step`, or 1% of the range when continuous.
    bool StepValue(int direction, bool notify = true) {
        float amount = (step > 0.0f) ? step : GetRange() * 0.01f;
        return SetValue(value + amount * static_cast<float>(direction), notify);
    }

    std::string GetValueText() const {
        if (valueFormatter) return valueFormatter(value);
        char buffer[64];
        int d = std::min(std::max(decimals, 0), 6);
        std::snprintf(buffer, sizeof(buffer), "%.*f", d, value);
        return std::string(buffer) + valueSuffix;
    }

    // geometry

    // Element area actually used by the track (the label takes the rest)
    glm::vec2 GetTrackAreaSize(const glm::vec2& elementSize) const {
        if (orientation == SliderOrientation::HORIZONTAL) {
            float w = elementSize.x - (showValue ? (valueLabelWidth + labelGap) : 0.0f);
            return glm::vec2(std::max(w, 1.0f), elementSize.y);
        }
        float h = elementSize.y - (showValue ? (fontSize + labelGap) : 0.0f);
        return glm::vec2(elementSize.x, std::max(h, 1.0f));
    }

    glm::vec2 GetHandleSize() const {
        if (orientation == SliderOrientation::HORIZONTAL) {
            return glm::vec2(handleWidth, handleHeight);
        }
        return glm::vec2(handleHeight, handleWidth);
    }

    glm::vec2 GetTrackPosition(const glm::vec2& elementPos, const glm::vec2& elementSize) const {
        glm::vec2 area = GetTrackAreaSize(elementSize);
        if (orientation == SliderOrientation::HORIZONTAL) {
            return glm::vec2(elementPos.x, elementPos.y + (area.y - trackThickness) * 0.5f);
        }
        return glm::vec2(elementPos.x + (area.x - trackThickness) * 0.5f, elementPos.y);
    }

    glm::vec2 GetTrackSize(const glm::vec2& elementSize) const {
        glm::vec2 area = GetTrackAreaSize(elementSize);
        if (orientation == SliderOrientation::HORIZONTAL) {
            return glm::vec2(area.x, trackThickness);
        }
        return glm::vec2(trackThickness, area.y);
    }

    // Distance the handle can travel from one end to the other
    float GetTravelLength(const glm::vec2& elementSize) const {
        glm::vec2 area = GetTrackAreaSize(elementSize);
        float length = (orientation == SliderOrientation::HORIZONTAL ? area.x : area.y) - handleWidth;
        return std::max(length, 1.0f);
    }

    glm::vec2 GetHandlePosition(const glm::vec2& elementPos, const glm::vec2& elementSize) const {
        glm::vec2 area = GetTrackAreaSize(elementSize);
        glm::vec2 handleSize = GetHandleSize();
        float t = GetNormalizedValue();

        if (orientation == SliderOrientation::HORIZONTAL) {
            return glm::vec2(elementPos.x + t * GetTravelLength(elementSize),
                elementPos.y + (area.y - handleSize.y) * 0.5f);
        }
        // Vertical: min at the bottom
        return glm::vec2(elementPos.x + (area.x - handleSize.x) * 0.5f,
            elementPos.y + (1.0f - t) * GetTravelLength(elementSize));
    }

    // Filled part of the track (from min up to the handle center)
    glm::vec4 GetFillRect(const glm::vec2& elementPos, const glm::vec2& elementSize) const {
        glm::vec2 trackPos = GetTrackPosition(elementPos, elementSize);
        glm::vec2 trackSize = GetTrackSize(elementSize);
        glm::vec2 handlePos = GetHandlePosition(elementPos, elementSize);
        glm::vec2 handleSize = GetHandleSize();

        if (orientation == SliderOrientation::HORIZONTAL) {
            float centerX = handlePos.x + handleSize.x * 0.5f;
            return glm::vec4(trackPos.x, trackPos.y, std::max(centerX - trackPos.x, 0.0f), trackSize.y);
        }
        float centerY = handlePos.y + handleSize.y * 0.5f;
        float bottom = trackPos.y + trackSize.y;
        return glm::vec4(trackPos.x, centerY, trackSize.x, std::max(bottom - centerY, 0.0f));
    }

    glm::vec4 GetValueLabelRect(const glm::vec2& elementPos, const glm::vec2& elementSize) const {
        glm::vec2 area = GetTrackAreaSize(elementSize);
        if (orientation == SliderOrientation::HORIZONTAL) {
            return glm::vec4(elementPos.x + area.x + labelGap, elementPos.y,
                valueLabelWidth, elementSize.y);
        }
        return glm::vec4(elementPos.x, elementPos.y + area.y + labelGap, elementSize.x, fontSize);
    }

    // interaction

    // Hit test over the interactive area (track area, ignoring the label)
    bool ContainsPoint(const glm::vec2& point, const glm::vec2& elementPos,
        const glm::vec2& elementSize) const {
        glm::vec2 area = GetTrackAreaSize(elementSize);
        // Grow a little so a thin track is still easy to grab
        float grow = std::max(0.0f, (handleHeight - (orientation == SliderOrientation::HORIZONTAL ? area.y : area.x)) * 0.5f);
        return point.x >= elementPos.x - grow && point.x <= elementPos.x + area.x + grow &&
            point.y >= elementPos.y - grow && point.y <= elementPos.y + area.y + grow;
    }

    bool ContainsHandle(const glm::vec2& point, const glm::vec2& elementPos,
        const glm::vec2& elementSize) const {
        glm::vec2 handlePos = GetHandlePosition(elementPos, elementSize);
        glm::vec2 handleSize = GetHandleSize();
        return point.x >= handlePos.x && point.x <= handlePos.x + handleSize.x &&
            point.y >= handlePos.y && point.y <= handlePos.y + handleSize.y;
    }

    // Value under a point in reference space (used while dragging)
    float GetValueFromPoint(const glm::vec2& point, const glm::vec2& elementPos,
        const glm::vec2& elementSize) const {
        float travel = GetTravelLength(elementSize);
        float t;
        if (orientation == SliderOrientation::HORIZONTAL) {
            t = (point.x - elementPos.x - handleWidth * 0.5f) / travel;
        }
        else {
            t = 1.0f - (point.y - elementPos.y - handleWidth * 0.5f) / travel;
        }
        t = std::min(std::max(t, 0.0f), 1.0f);
        return minValue + t * GetRange();
    }

    // colors

    bool IsDisabled() const { return !isInteractable || state == SliderState::DISABLED; }

    glm::vec4 GetCurrentTrackColor() const {
        return IsDisabled() ? disabledTrackColor : trackColor;
    }

    glm::vec4 GetCurrentFillColor() const {
        return IsDisabled() ? disabledFillColor : fillColor;
    }

    glm::vec4 GetCurrentHandleColor() const {
        if (IsDisabled()) return disabledHandleColor;
        switch (state) {
        case SliderState::DRAGGING: return handleDraggingColor;
        case SliderState::HOVERED:  return handleHoverColor;
        default:                    return handleColor;
        }
    }

    // chainable setters

    UISlider* SetOnValueChanged(std::function<void(float)> callback) {
        onValueChanged = callback;
        return this;
    }

    UISlider* SetOnDragStart(std::function<void()> callback) {
        onDragStart = callback;
        return this;
    }

    UISlider* SetOnDragEnd(std::function<void(float)> callback) {
        onDragEnd = callback;
        return this;
    }
};

#endif // UISLIDER_HPP