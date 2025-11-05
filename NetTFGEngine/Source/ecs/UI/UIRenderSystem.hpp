#ifndef UIRENDERSYSTEM_HPP
#define UIRENDERSYSTEM_HPP

#include "ecs/ecs_common.hpp"
#include "UIElement.hpp"
#include "UIText.hpp"
#include "UIImage.hpp"
#include "UIButton.hpp"
#include "Utils/FontManager.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <map>
#include <string>
#include <memory>

class UIRenderSystem : public ISystem {
public:
    UIRenderSystem(int screenWidth, int screenHeight);
    ~UIRenderSystem();

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, float deltaTime) override;
    void UpdateScreenSize(int width, int height);
    
    // Font management
    bool LoadFont(const std::string& fontName, const std::string& fontPath, unsigned int fontSize = 48);
    FontManager* GetFontManager() { return fontManager.get(); }
    
    // Input handling (call from your input system)
    void OnMouseMove(float x, float y);
    void OnMouseDown(float x, float y);
    void OnMouseUp(float x, float y);

private:
    int screenWidth;
    int screenHeight;
    glm::vec2 mousePosition;
    bool mouseDown;
    
    // Font manager
    std::unique_ptr<FontManager> fontManager;
    
    // OpenGL resources
    GLuint shaderProgram;
    GLuint textShaderProgram;  // Separate shader for text
    GLuint quadVAO, quadVBO;
    GLuint textVAO, textVBO;
    
    // Projection matrix for UI (orthographic)
    glm::mat4 projection;
    
    // Initialize shaders and buffers
    void InitializeShaders();
    void InitializeQuad();
    void InitializeTextRendering();
    void UpdateProjection();
    
    // Rendering methods
    void RenderUIImage(const UIElement* element, const UIImage* image);
    void RenderUIText(const UIElement* element, const UIText* text);
    void RenderUIButton(Entity entity, const UIElement* element, const UIButton* button);
    void RenderQuad(const glm::vec2& position, const glm::vec2& size, 
                   const glm::vec4& color, GLuint textureID = 0,
                   const glm::vec4& uvRect = glm::vec4(0, 0, 1, 1));
    
    // Button interaction
    void UpdateButton(Entity entity, UIElement* element, UIButton* button);
    Entity hoveredButton;
    Entity pressedButton;
    
    // Shader compilation
    GLuint CompileShader(const char* source, GLenum type);
    GLuint CreateShaderProgram();
    GLuint CreateTextShaderProgram();
};

#endif // UIRENDERSYSTEM_HPP