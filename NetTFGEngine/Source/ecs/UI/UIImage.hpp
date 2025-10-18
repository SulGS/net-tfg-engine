#ifndef UIIMAGE_HPP
#define UIIMAGE_HPP

#include "ecs/ecs_common.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <string>

class UIImage : public IComponent {
public:
    UIImage()
        : textureID(0)
        , color(1.0f, 1.0f, 1.0f, 1.0f)
        , uvRect(0.0f, 0.0f, 1.0f, 1.0f)
    {}

    GLuint textureID;           // OpenGL texture ID
    glm::vec4 color;            // Tint color (RGBA)
    glm::vec4 uvRect;           // UV coordinates (x, y, width, height)
    
    void SetColor(float r, float g, float b, float a = 1.0f) {
        color = glm::vec4(r, g, b, a);
    }
    
    void SetUVRect(float x, float y, float w, float h) {
        uvRect = glm::vec4(x, y, w, h);
    }
};

#endif // UIIMAGE_HPP