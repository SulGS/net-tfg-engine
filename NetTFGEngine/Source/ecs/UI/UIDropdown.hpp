#ifndef UIDROPDOWN_HPP
#define UIDROPDOWN_HPP

#include "ecs/ecs_common.hpp"
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include "OpenGL/OpenGLIncludes.hpp"

enum class DropdownState {
    NORMAL,
    HOVERED,
    OPEN,
    DISABLED
};

// Dropdown / combo box component.
//
// The element rect (UIElement::position/size) is the *header* only. The popup
// list is drawn outside those bounds, so its geometry is computed here and the
// systems use ContainsList()/GetItemIndexAtPoint() for hit testing instead of
// UIElement::Contains().
//
// Rects are returned as glm::vec4(x, y, width, height).
class UIDropdown : public IComponent {
public:
    UIDropdown(const std::string& placeholder = "Select...")
        : id("")
        , options()
        , selectedIndex(-1)
        , placeholderText(placeholder)
        , state(DropdownState::NORMAL)
        , isOpen(false)
        , isInteractable(true)
        , hoveredIndex(-1)
        , scrollOffset(0)
        , maxVisibleItems(6)
        , itemHeight(28.0f)
        , padding(8.0f)
        , borderWidth(2.0f)
        , arrowSize(10.0f)
        , listGap(2.0f)
        , backgroundColor(1.0f, 1.0f, 1.0f, 1.0f)
        , hoverColor(0.95f, 0.95f, 0.95f, 1.0f)
        , openColor(0.9f, 0.95f, 1.0f, 1.0f)
        , disabledColor(0.85f, 0.85f, 0.85f, 1.0f)
        , borderColor(0.7f, 0.7f, 0.7f, 1.0f)
        , openBorderColor(0.3f, 0.5f, 0.9f, 1.0f)
        , disabledBorderColor(0.6f, 0.6f, 0.6f, 1.0f)
        , listBackgroundColor(1.0f, 1.0f, 1.0f, 1.0f)
        , listBorderColor(0.6f, 0.6f, 0.6f, 1.0f)
        , itemHoverColor(0.85f, 0.9f, 1.0f, 1.0f)
        , itemSelectedColor(0.75f, 0.85f, 1.0f, 1.0f)
        , scrollbarColor(0.6f, 0.6f, 0.6f, 1.0f)
        , textColor(0.0f, 0.0f, 0.0f, 1.0f)
        , placeholderColor(0.5f, 0.5f, 0.5f, 1.0f)
        , itemTextColor(0.05f, 0.05f, 0.05f, 1.0f)
        , disabledTextColor(0.45f, 0.45f, 0.45f, 1.0f)
        , arrowColor(0.25f, 0.25f, 0.25f, 1.0f)
        , fontName("default")
        , fontSize(16.0f)
    {
    }

    std::string id;

    // Data
    std::vector<std::string> options;
    int selectedIndex;                // -1 = nothing selected
    std::string placeholderText;

    // State
    DropdownState state;
    bool isOpen;
    bool isInteractable;
    int hoveredIndex;                 // highlighted row while open (-1 = none)
    int scrollOffset;                 // first visible row
    int maxVisibleItems;

    // Geometry
    float itemHeight;
    float padding;
    float borderWidth;
    float arrowSize;
    float listGap;                    // space between header and popup

    // Header colors
    glm::vec4 backgroundColor;
    glm::vec4 hoverColor;
    glm::vec4 openColor;
    glm::vec4 disabledColor;
    glm::vec4 borderColor;
    glm::vec4 openBorderColor;
    glm::vec4 disabledBorderColor;

    // List colors
    glm::vec4 listBackgroundColor;
    glm::vec4 listBorderColor;
    glm::vec4 itemHoverColor;
    glm::vec4 itemSelectedColor;
    glm::vec4 scrollbarColor;

    // Text
    glm::vec4 textColor;
    glm::vec4 placeholderColor;
    glm::vec4 itemTextColor;
    glm::vec4 disabledTextColor;
    glm::vec4 arrowColor;
    std::string fontName;
    float fontSize;

    // Callbacks
    std::function<void(int, const std::string&)> onSelectionChanged = nullptr;
    std::function<void()> onOpen = nullptr;
    std::function<void()> onClose = nullptr;

    // ----------------------------------------------------------- options

    int GetOptionCount() const { return static_cast<int>(options.size()); }

    bool HasSelection() const {
        return selectedIndex >= 0 && selectedIndex < GetOptionCount();
    }

    const std::string& GetSelectedText() const {
        static const std::string empty;
        return HasSelection() ? options[selectedIndex] : empty;
    }

    // What the header shows (selected option, or the placeholder)
    const std::string& GetDisplayText() const {
        return HasSelection() ? options[selectedIndex] : placeholderText;
    }

    bool IsShowingPlaceholder() const { return !HasSelection(); }

    UIDropdown* AddOption(const std::string& option) {
        options.push_back(option);
        return this;
    }

    UIDropdown* SetOptions(const std::vector<std::string>& newOptions) {
        options = newOptions;
        if (selectedIndex >= GetOptionCount()) selectedIndex = -1;
        scrollOffset = 0;
        hoveredIndex = -1;
        return this;
    }

    void ClearOptions() {
        options.clear();
        selectedIndex = -1;
        scrollOffset = 0;
        hoveredIndex = -1;
    }

    // Returns true if the selection actually changed
    bool SelectIndex(int index, bool notify = true) {
        if (index < -1 || index >= GetOptionCount()) return false;
        if (index == selectedIndex) return false;
        selectedIndex = index;
        if (notify && onSelectionChanged) {
            onSelectionChanged(selectedIndex, GetSelectedText());
        }
        return true;
    }

    bool SelectOption(const std::string& option, bool notify = true) {
        auto it = std::find(options.begin(), options.end(), option);
        if (it == options.end()) return false;
        return SelectIndex(static_cast<int>(std::distance(options.begin(), it)), notify);
    }

    // ------------------------------------------------------- open / close

    void Open() {
        if (isOpen || !isInteractable || options.empty()) return;
        isOpen = true;
        state = DropdownState::OPEN;
        hoveredIndex = HasSelection() ? selectedIndex : 0;
        EnsureVisible(hoveredIndex);
        if (onOpen) onOpen();
    }

    void Close() {
        if (!isOpen) return;
        isOpen = false;
        state = DropdownState::NORMAL;
        hoveredIndex = -1;
        if (onClose) onClose();
    }

    void Toggle() {
        if (isOpen) Close();
        else Open();
    }

    // -------------------------------------------------------- scrolling

    int GetVisibleItemCount() const {
        return std::min(std::max(maxVisibleItems, 1), GetOptionCount());
    }

    int GetMaxScrollOffset() const {
        return std::max(0, GetOptionCount() - GetVisibleItemCount());
    }

    void ClampScroll() {
        scrollOffset = std::min(std::max(scrollOffset, 0), GetMaxScrollOffset());
    }

    // delta in rows (negative = up)
    void Scroll(int delta) {
        scrollOffset += delta;
        ClampScroll();
    }

    void EnsureVisible(int index) {
        if (index < 0) return;
        int visible = GetVisibleItemCount();
        if (index < scrollOffset) scrollOffset = index;
        else if (index >= scrollOffset + visible) scrollOffset = index - visible + 1;
        ClampScroll();
    }

    // direction: +1 down / -1 up. Moves the highlighted row while open.
    void MoveHighlight(int direction) {
        int count = GetOptionCount();
        if (count == 0) return;
        int start = (hoveredIndex >= 0) ? hoveredIndex : (HasSelection() ? selectedIndex : -1);
        int next = start + direction;
        if (next < 0) next = 0;
        if (next >= count) next = count - 1;
        hoveredIndex = next;
        EnsureVisible(hoveredIndex);
    }

    bool NeedsScrollbar() const { return GetOptionCount() > GetVisibleItemCount(); }

    // -------------------------------------------------------- geometry

    float GetListHeight() const {
        return GetVisibleItemCount() * itemHeight + borderWidth * 2.0f;
    }

    // True when there is no room below and the popup must be drawn above
    bool OpensUpward(const glm::vec2& elementPos, const glm::vec2& elementSize, int screenHeight) const {
        float below = elementPos.y + elementSize.y + listGap + GetListHeight();
        float above = elementPos.y - listGap - GetListHeight();
        return below > static_cast<float>(screenHeight) && above >= 0.0f;
    }

    glm::vec4 GetListRect(const glm::vec2& elementPos, const glm::vec2& elementSize, int screenHeight) const {
        float height = GetListHeight();
        float y = OpensUpward(elementPos, elementSize, screenHeight)
            ? (elementPos.y - listGap - height)
            : (elementPos.y + elementSize.y + listGap);
        return glm::vec4(elementPos.x, y, elementSize.x, height);
    }

    // Rect of one option. Width/height are 0 when the row is scrolled out.
    glm::vec4 GetItemRect(int index, const glm::vec2& elementPos, const glm::vec2& elementSize,
        int screenHeight) const {
        int visible = GetVisibleItemCount();
        int row = index - scrollOffset;
        if (index < 0 || index >= GetOptionCount() || row < 0 || row >= visible) {
            return glm::vec4(0.0f);
        }
        glm::vec4 list = GetListRect(elementPos, elementSize, screenHeight);
        return glm::vec4(list.x + borderWidth,
            list.y + borderWidth + row * itemHeight,
            list.z - borderWidth * 2.0f,
            itemHeight);
    }

    bool ContainsList(const glm::vec2& point, const glm::vec2& elementPos,
        const glm::vec2& elementSize, int screenHeight) const {
        if (!isOpen) return false;
        glm::vec4 list = GetListRect(elementPos, elementSize, screenHeight);
        return point.x >= list.x && point.x <= list.x + list.z &&
            point.y >= list.y && point.y <= list.y + list.w;
    }

    // Absolute option index under a point, or -1
    int GetItemIndexAtPoint(const glm::vec2& point, const glm::vec2& elementPos,
        const glm::vec2& elementSize, int screenHeight) const {
        if (!ContainsList(point, elementPos, elementSize, screenHeight)) return -1;
        glm::vec4 list = GetListRect(elementPos, elementSize, screenHeight);
        float localY = point.y - (list.y + borderWidth);
        if (localY < 0.0f) return -1;
        int row = static_cast<int>(localY / itemHeight);
        if (row < 0 || row >= GetVisibleItemCount()) return -1;
        int index = scrollOffset + row;
        return (index >= 0 && index < GetOptionCount()) ? index : -1;
    }

    // Arrow triangle on the right side of the header
    glm::vec2 GetArrowCenter(const glm::vec2& elementPos, const glm::vec2& elementSize) const {
        return glm::vec2(elementPos.x + elementSize.x - padding - arrowSize * 0.5f,
            elementPos.y + elementSize.y * 0.5f);
    }

    // Space left for the header text once the arrow is accounted for
    glm::vec4 GetTextRect(const glm::vec2& elementPos, const glm::vec2& elementSize) const {
        float width = elementSize.x - padding * 2.0f - arrowSize - padding;
        return glm::vec4(elementPos.x + padding, elementPos.y,
            std::max(width, 1.0f), elementSize.y);
    }

    // Vertical scrollbar inside the popup (empty rect when not needed)
    glm::vec4 GetScrollbarRect(const glm::vec2& elementPos, const glm::vec2& elementSize,
        int screenHeight) const {
        if (!NeedsScrollbar()) return glm::vec4(0.0f);
        glm::vec4 list = GetListRect(elementPos, elementSize, screenHeight);
        float trackHeight = list.w - borderWidth * 2.0f;
        float ratio = static_cast<float>(GetVisibleItemCount()) / static_cast<float>(GetOptionCount());
        float thumbHeight = std::max(trackHeight * ratio, 12.0f);
        float maxOffset = static_cast<float>(GetMaxScrollOffset());
        float t = (maxOffset > 0.0f) ? (static_cast<float>(scrollOffset) / maxOffset) : 0.0f;
        float width = 4.0f;
        return glm::vec4(list.x + list.z - borderWidth - width,
            list.y + borderWidth + t * (trackHeight - thumbHeight),
            width,
            thumbHeight);
    }

    // ---------------------------------------------------------- colors

    bool IsDisabled() const { return !isInteractable || state == DropdownState::DISABLED; }

    glm::vec4 GetCurrentBackgroundColor() const {
        if (IsDisabled()) return disabledColor;
        switch (state) {
        case DropdownState::OPEN:    return openColor;
        case DropdownState::HOVERED: return hoverColor;
        default:                     return backgroundColor;
        }
    }

    glm::vec4 GetCurrentBorderColor() const {
        if (IsDisabled()) return disabledBorderColor;
        return (state == DropdownState::OPEN) ? openBorderColor : borderColor;
    }

    glm::vec4 GetCurrentTextColor() const {
        if (IsDisabled()) return disabledTextColor;
        return IsShowingPlaceholder() ? placeholderColor : textColor;
    }

    // ----------------------------------------------- chainable setters

    UIDropdown* SetOnSelectionChanged(std::function<void(int, const std::string&)> callback) {
        onSelectionChanged = callback;
        return this;
    }

    UIDropdown* SetOnOpen(std::function<void()> callback) {
        onOpen = callback;
        return this;
    }

    UIDropdown* SetOnClose(std::function<void()> callback) {
        onClose = callback;
        return this;
    }
};

#endif // UIDROPDOWN_HPP