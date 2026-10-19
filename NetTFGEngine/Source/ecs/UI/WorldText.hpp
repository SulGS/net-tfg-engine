#ifndef WORLDTEXT_HPP
#define WORLDTEXT_HPP

#include "ecs/ecs_common.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <string>

// Text placed in the world, at its entity's Transform, instead of on the screen like UIText: it moves with the entity
// and is sized in world units, so it grows and shrinks with the camera like any other object. Drawn by
// WorldTextRenderSystem over the finished 3D frame and under the UI. Needs a Transform on the same entity; its scale
// is ignored (height sets the size), and so is its rotation when faceCamera is on.
class WorldText : public IComponent {
public:
    WorldText(const std::string& text = "", float height = 1.0f)
        : text(text)
        , height(height)
    {}

    std::string text;   // UTF-8

    // Height of a capital letter ('H'), in world units.
    float height;

    glm::vec4 color = glm::vec4(1.0f);
    std::string fontName = "default";   // a font loaded in UIRenderSystem (they share its FontManager)

    // Point of the text placed at the anchor, as a fraction of its width (x) and of its cap height (y), from the
    // baseline's left end: (0.5, 0) = centre of the baseline, (0.5, 0.5) = centre of the text.
    glm::vec2 pivot = glm::vec2(0.5f, 0.0f);

    // Anchor = Transform position + offset (world axes) + localOffset (along the text's own right/up axes, which are
    // the camera's when faceCamera is on: (0, 3) is "3 units above the entity on screen" whichever way the camera looks).
    glm::vec3 offset = glm::vec3(0.0f);
    glm::vec2 localOffset = glm::vec2(0.0f);

    // Billboard: always turned towards the camera, upright on screen. Off: lies on the Transform's local XY plane.
    bool faceCamera = true;

    // Drop shadow behind the glyphs, displaced down-right by shadowOffset * height. Alpha 0 turns it off.
    glm::vec4 shadowColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.75f);
    float shadowOffset = 0.08f;

    bool visible = true;

    void SetColor(float r, float g, float b, float a = 1.0f) {
        color = glm::vec4(r, g, b, a);
    }
};

#endif // WORLDTEXT_HPP
