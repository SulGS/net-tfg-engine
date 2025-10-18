#ifndef UITEXT_HPP
#define UITEXT_HPP

#include "ecs/ecs_common.hpp"
#include <string>
#include "OpenGL/OpenGLIncludes.hpp"

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
    std::string fontName;  // Font to use
    
    void SetColor(float r, float g, float b, float a = 1.0f) {
        color = glm::vec4(r, g, b, a);
    }
    
    void SetFont(const std::string& font) {
        fontName = font;
    }
};

#endif // UITEXT_HPP