#ifndef UIRENDERSYSTEM_HPP
#define UIRENDERSYSTEM_HPP

#include "ecs/ecs_common.hpp"
#include "UIElement.hpp"
#include "UIText.hpp"
#include "UIImage.hpp"
#include "UIButton.hpp"
#include "UITextField.hpp"
#include "UISlider.hpp"
#include "UIDropdown.hpp"
#include "Utils/FontManager.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <utility>

// Horizontal alignment for the generic text helper
enum class UITextAlign {
    LEFT,
    CENTER,
    RIGHT
};

class UIRenderSystem : public ISystem {
public:
    UIRenderSystem(int refWidth, int refHeight);
    ~UIRenderSystem();

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override;
    void UpdateScreenSize(int width, int height);

    bool LoadFont(const std::string& fontName, const std::string& fontPath, unsigned int fontSize = 48);
    FontManager* GetFontManager() { return fontManager.get(); }

    // Call from your input system.
    void OnMouseMove(float x, float y);
    void OnMouseDown(float x, float y);
    void OnMouseUp(float x, float y);

private:
    int refWidth;
    int refHeight;
    int screenWidth;
    int screenHeight;
    glm::vec2 mousePosition;
    bool mouseDown;

    std::unique_ptr<FontManager> fontManager;

    GLuint shaderProgram;
    GLuint textShaderProgram;  // Separate shader for text
    GLuint quadVAO, quadVBO;
    GLuint textVAO, textVBO;

    // Orthographic.
    glm::mat4 projection;

    void InitializeShaders();
    void InitializeQuad();
    void InitializeTextRendering();
    void UpdateProjection();

    void RenderUIImage(const UIElement* element, const UIImage* image);
    void RenderUIText(const UIElement* element, const UIText* text);
    void RenderUIButton(Entity entity, const UIElement* element, const UIButton* button);
    void RenderUITextField(const UIElement* element, const UITextField* textField);
    void RenderUISlider(const UIElement* element, const UISlider* slider);
    void RenderUIDropdown(const UIElement* element, const UIDropdown* dropdown);
    // Popup list: drawn in a second pass so it always sits on top of the UI
    void RenderUIDropdownList(const UIElement* element, const UIDropdown* dropdown);
    void RenderQuad(const glm::vec2& position, const glm::vec2& size,
        const glm::vec4& color, GLuint textureID = 0,
        const glm::vec4& uvRect = glm::vec4(0, 0, 1, 1));
    void RenderBorder(const glm::vec2& position, const glm::vec2& size,
        const glm::vec4& color, float thickness);
    // Approximates a triangle with horizontal slices (the UI shader only draws quads)
    void RenderTriangle(const glm::vec2& center, float width, float height,
        bool pointDown, const glm::vec4& color);

    // Generic text helpers (shared by slider and dropdown rendering)
    float GetFontScale(const std::string& fontName, float fontSize);
    float MeasureTextWidth(const std::string& text, const std::string& fontName, float fontSize);
    std::string TruncateTextToWidth(const std::string& text, const std::string& fontName,
        float fontSize, float maxWidth);
    void RenderTextInRect(const std::string& text, const glm::vec2& rectPos, const glm::vec2& rectSize,
        const std::string& fontName, float fontSize, const glm::vec4& color,
        UITextAlign align = UITextAlign::LEFT);

    void UpdateButton(Entity entity, UIElement* element, UIButton* button);
    Entity hoveredButton;
    Entity pressedButton;

    GLuint CompileShader(const char* source, GLenum type);
    GLuint CreateShaderProgram();
    GLuint CreateTextShaderProgram();
};

#endif // UIRENDERSYSTEM_HPP