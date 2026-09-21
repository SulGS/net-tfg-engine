#ifndef UITEXT_HPP
#define UITEXT_HPP

#include "ecs/ecs_common.hpp"
#include <string>
#include "OpenGL/OpenGLIncludes.hpp"

// Horizontal alignment of text inside a rectangle
enum class UITextAlign {
    LEFT,
    CENTER,
    RIGHT
};

class UIText : public IComponent {
public:
    UIText(const std::string& text = "", float fontSize = 24.0f)
        : text(text)
        , fontSize(fontSize)
        , color(1.0f, 1.0f, 1.0f, 1.0f)
        , fontName("default")
    {}

    std::string text;
    float fontSize;
    glm::vec4 color;  // RGBA
    std::string fontName;

    // Where the text sits horizontally inside its UIElement's rectangle. LEFT (the default) starts at the rectangle's left edge; CENTER and RIGHT use the measured width of the text, so they stay put if the text changes.
    UITextAlign align = UITextAlign::LEFT;

    void SetColor(float r, float g, float b, float a = 1.0f) {
        color = glm::vec4(r, g, b, a);
    }
    
    void SetFont(const std::string& font) {
        fontName = font;
    }
};

#endif // UITEXT_HPP