#include "UIRenderSystem.hpp"
#include <iostream>
#include <algorithm>
#include "Utils/Debug/Debug.hpp"
#include "Utils/AssetManager.hpp"

// UI Shaders
const char* uiVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

uniform mat4 uProjection;
uniform mat4 uModel;

out vec2 TexCoord;

void main() {
    gl_Position = uProjection * uModel * vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

const char* uiFragmentShader = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform vec4 uColor;
uniform sampler2D uTexture;
uniform bool uUseTexture;

void main() {
    if (uUseTexture) {
        FragColor = texture(uTexture, TexCoord) * uColor;
    } else {
        FragColor = uColor;
    }
}
)";

// Text rendering shaders
const char* textVertexShader = R"(
#version 330 core
layout (location = 0) in vec4 vertex; // vec2 pos, vec2 tex

out vec2 TexCoords;

uniform mat4 uProjection;

void main() {
    gl_Position = uProjection * vec4(vertex.xy, 0.0, 1.0);
    TexCoords = vertex.zw;
}
)";

const char* textFragmentShader = R"(
#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D text;
uniform vec4 textColor;

void main() {
    vec4 sampled = vec4(1.0, 1.0, 1.0, texture(text, TexCoords).r);
    FragColor = textColor * sampled;
}
)";

UIRenderSystem::UIRenderSystem(int width, int height)
    : screenWidth(width)
    , screenHeight(height)
    , mousePosition(0.0f)
    , mouseDown(false)
    , hoveredButton(0)
    , pressedButton(0)
    , fontManager(std::make_unique<FontManager>())
{
    InitializeShaders();
    InitializeQuad();
    InitializeTextRendering();
    UpdateProjection();
}

UIRenderSystem::~UIRenderSystem() {
    glDeleteVertexArrays(1, &quadVAO);
    glDeleteBuffers(1, &quadVBO);
    glDeleteVertexArrays(1, &textVAO);
    glDeleteBuffers(1, &textVBO);
    glDeleteProgram(shaderProgram);
    glDeleteProgram(textShaderProgram);
}

bool UIRenderSystem::LoadFont(const std::string& fontName, const std::string& fontPath, unsigned int fontSize) {
    return fontManager->LoadFont(fontName, fontPath, fontSize);
}

void UIRenderSystem::Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) {
    // Save previous OpenGL state
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLint blendSrc, blendDst;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDst);
    
    // Setup UI rendering state
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(shaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uProjection"), 
                       1, GL_FALSE, glm::value_ptr(projection));
    
    // Collect all UI elements and sort by layer
    std::vector<std::tuple<Entity, UIElement*, int>> uiElements;
    
    auto query = entityManager.CreateQuery<UIElement>();
    for (auto [entity, element] : query) {
        if (element->isVisible) {
            uiElements.push_back({entity, element, element->layer});
        }
    }
    
    // Sort by layer (lower layers rendered first)
    std::sort(uiElements.begin(), uiElements.end(), 
        [](const auto& a, const auto& b) {
            return std::get<2>(a) < std::get<2>(b);
        });
    
    // Render all UI elements
    for (const auto& [entity, element, layer] : uiElements) {
        // Check for button component
        UIButton* button = entityManager.GetComponent<UIButton>(entity);
        if (button) {
            UpdateButton(entity, element, button);
            RenderUIButton(entity, element, button);
        }
        
        // Check for image component
        UIImage* image = entityManager.GetComponent<UIImage>(entity);
        if (image) {

            if (!image->isLoaded) {
                auto reqBuffer = AssetManager::instance().acquire<GLuint>(image->texturePath);
                if (!reqBuffer)
                {
                    Debug::Error("UIRenderSystem") << "Failed to load texture: " << image->texturePath << "\n";
                }
                else 
                {
                    image->textureID = *reqBuffer;
					image->isLoaded = true;
                    RenderUIImage(element, image);
                }
                
            }

        }
        
        // Check for text component
        UIText* text = entityManager.GetComponent<UIText>(entity);
        if (text) {
            RenderUIText(element, text);
        }

        UITextField* textField = entityManager.GetComponent<UITextField>(entity);
        if (textField) {
            RenderUITextField(element, textField);
        }

    }
    
    glUseProgram(0);
    
    // Restore previous OpenGL state
    if (depthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    
    if (!blend) glDisable(GL_BLEND);
    glBlendFunc(blendSrc, blendDst);
}

void UIRenderSystem::UpdateScreenSize(int width, int height) {
    screenWidth = width;
    screenHeight = height;
    UpdateProjection();
}

void UIRenderSystem::OnMouseMove(float x, float y) {
    mousePosition = glm::vec2(x, y);
}

void UIRenderSystem::OnMouseDown(float x, float y) {
    mousePosition = glm::vec2(x, y);
    mouseDown = true;
}

void UIRenderSystem::OnMouseUp(float x, float y) {
    mousePosition = glm::vec2(x, y);
    mouseDown = false;
}

void UIRenderSystem::InitializeShaders() {
    shaderProgram = CreateShaderProgram();
}

void UIRenderSystem::InitializeTextRendering() {
    textShaderProgram = CreateTextShaderProgram();

    // Create VAO/VBO for dynamic text quads (each vertex contains vec2 pos + vec2 uv => vec4)
    glGenVertexArrays(1, &textVAO);
    glGenBuffers(1, &textVBO);

    glBindVertexArray(textVAO);
    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    // Reserve space for a reasonable number of glyphs (e.g., 1024 quads)
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 6 * 1024, nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    // vertex: vec4 (pos.xy, uv.xy)
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void UIRenderSystem::InitializeQuad() {
    // Quad vertices (position + texcoord)
    float vertices[] = {
        // Pos      // Tex
        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        
        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 0.0f, 1.0f, 0.0f
    };
    
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 
                         (void*)(2 * sizeof(float)));
    
    glBindVertexArray(0);
}

void UIRenderSystem::UpdateProjection() {
    projection = glm::ortho(0.0f, (float)screenWidth, (float)screenHeight, 0.0f, -1.0f, 1.0f);
}

void UIRenderSystem::RenderUIImage(const UIElement* element, const UIImage* image) {
    glm::vec2 pos = element->GetScreenPosition(screenWidth, screenHeight);
    glm::vec4 color = image->color * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    
    RenderQuad(pos, element->size, color, image->textureID, image->uvRect);
}

void UIRenderSystem::RenderUIText(const UIElement* element, const UIText* text) {
    if (text->text.empty()) return;
    
    glm::vec2 pos = element->GetScreenPosition(screenWidth, screenHeight);

    const std::string fontName = text->fontName;
    if (!fontManager || !fontManager->HasFont(fontName)) {
        // Fallback: draw placeholder
        glm::vec4 color = text->color * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
        RenderQuad(pos, element->size, color);
        return;
    }

    // Calculate scale based on desired font size
    // Most fonts are loaded at a specific size (e.g., 48px)
    // We need to find the actual loaded size to scale correctly
    const Character* refChar = fontManager->GetCharacter(fontName, 'H');
    if (!refChar) {
        refChar = fontManager->GetCharacter(fontName, 'A');
    }
    
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = text->fontSize / loadedFontSize;

    // Use text shader
    glUseProgram(textShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"), 
                       1, GL_FALSE, glm::value_ptr(projection));
    glm::vec4 textColor = text->color * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"), 
                 1, glm::value_ptr(textColor));

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(textVAO);

    // Calculate baseline position
    // FreeType uses a baseline system where:
    // - bearing.y is the distance from baseline to top of glyph
    // - Characters sit on the baseline, descenders go below
    float cursorX = pos.x;
    
    // Find the maximum bearing.y to establish a consistent baseline
    float maxBearingY = 0.0f;
    for (char c : text->text) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }
    
    // Baseline is at pos.y + maxBearingY (scaled)
    float baselineY = pos.y + (maxBearingY * scale);
    
    // If element has a height, center the text vertically
    if (element->size.y > 0.0f) {
        // Measure total text height
        float maxHeight = 0.0f;
        float minY = 0.0f;
        for (char c : text->text) {
            const Character* ch = fontManager->GetCharacter(fontName, c);
            if (ch) {
                float top = ch->bearing.y * scale;
                float bottom = (ch->bearing.y - ch->size.y) * scale;
                maxHeight = std::max(maxHeight, top);
                minY = std::min(minY, bottom);
            }
        }
        float totalTextHeight = maxHeight - minY;
        
        // Center vertically in element
        float yOffset = (element->size.y - totalTextHeight) * 0.5f;
        baselineY = pos.y + yOffset + maxHeight;
    }

    // Render each character
    for (char c : text->text) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (!ch) continue;

        // Calculate glyph position
        // xpos: cursor + bearing offset
        // ypos: baseline - bearing (FreeType's bearing.y is from baseline UP)
        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;

        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

        // Build quad vertices (position.xy, texcoord.xy)
        float vertices[6][4] = {
            { xpos,     ypos + h,   0.0f, 1.0f },  // Bottom-left
            { xpos + w, ypos,       1.0f, 0.0f },  // Top-right
            { xpos,     ypos,       0.0f, 0.0f },  // Top-left

            { xpos,     ypos + h,   0.0f, 1.0f },  // Bottom-left
            { xpos + w, ypos + h,   1.0f, 1.0f },  // Bottom-right
            { xpos + w, ypos,       1.0f, 0.0f }   // Top-right
        };

        // Bind glyph texture
        glBindTexture(GL_TEXTURE_2D, ch->textureID);
        glUniform1i(glGetUniformLocation(textShaderProgram, "text"), 0);

        // Update VBO and draw
        glBindBuffer(GL_ARRAY_BUFFER, textVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Advance cursor (advance is in 1/64th of pixels)
        cursorX += (ch->advance >> 6) * scale;
    }

    // Cleanup
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void UIRenderSystem::RenderUIButton(Entity entity, const UIElement* element, const UIButton* button) {
    glm::vec2 pos = element->GetScreenPosition(screenWidth, screenHeight);

    // Render background
    glm::vec4 bgColor = button->GetCurrentColor() * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    RenderQuad(pos, element->size, bgColor);

    // Render border (as a slightly larger quad behind)
    glm::vec4 borderColor = button->GetCurrentBorderColor() * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    float borderWidth = 2.0f;

    // Top border
    RenderQuad(glm::vec2(pos.x, pos.y - borderWidth),
        glm::vec2(element->size.x, borderWidth), borderColor);
    // Bottom border
    RenderQuad(glm::vec2(pos.x, pos.y + element->size.y),
        glm::vec2(element->size.x, borderWidth), borderColor);
    // Left border
    RenderQuad(glm::vec2(pos.x - borderWidth, pos.y),
        glm::vec2(borderWidth, element->size.y), borderColor);
    // Right border
    RenderQuad(glm::vec2(pos.x + element->size.x, pos.y),
        glm::vec2(borderWidth, element->size.y), borderColor);

    // Calculate text rendering area (with padding)
    glm::vec2 textPos = pos + glm::vec2(button->padding, button->padding);
    glm::vec2 textAreaSize = element->size - glm::vec2(button->padding * 2.0f);

    // Get text to render
    std::string displayText = button->text;

    if (displayText.empty()) {
        return;
    }

    // Get font info
    const std::string fontName = button->fontName;
    if (!fontManager || !fontManager->HasFont(fontName)) {
        return;
    }

    // Calculate scale
    const Character* refChar = fontManager->GetCharacter(fontName, 'H');
    if (!refChar) refChar = fontManager->GetCharacter(fontName, 'A');
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = button->fontSize / loadedFontSize;

    // Setup text rendering
    glUseProgram(textShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));

    glm::vec4 renderColor = button->textColor * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);

    glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"),
        1, glm::value_ptr(renderColor));

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(textVAO);

    // Calculate baseline
    float maxBearingY = 0.0f;
    for (char c : displayText) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }

    // Calculate total text width for centering
    float textWidth = 0.0f;
    for (char c : displayText) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch) {
            textWidth += (ch->advance >> 6) * scale;
        }
    }

    // Center text horizontally and vertically in the text area
    float baselineY = textPos.y + (textAreaSize.y + maxBearingY * scale) * 0.5f;
    float cursorX = textPos.x + (textAreaSize.x - textWidth) * 0.5f;

    // Render text
    for (size_t i = 0; i < displayText.length(); i++) {
        char c = displayText[i];
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (!ch) continue;

        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;
        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

        // Build vertices
        float vertices[6][4] = {
            { xpos,     ypos + h,   0.0f, 1.0f },
            { xpos + w, ypos,       1.0f, 0.0f },
            { xpos,     ypos,       0.0f, 0.0f },
            { xpos,     ypos + h,   0.0f, 1.0f },
            { xpos + w, ypos + h,   1.0f, 1.0f },
            { xpos + w, ypos,       1.0f, 0.0f }
        };

        glBindTexture(GL_TEXTURE_2D, ch->textureID);
        glUniform1i(glGetUniformLocation(textShaderProgram, "text"), 0);

        glBindBuffer(GL_ARRAY_BUFFER, textVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        cursorX += (ch->advance >> 6) * scale;
    }

    // Cleanup
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void UIRenderSystem::RenderQuad(const glm::vec2& position, const glm::vec2& size,
    const glm::vec4& color, GLuint textureID,
    const glm::vec4& uvRect) {
    glUseProgram(shaderProgram);  // Ensure shader is active
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uModel"),
        1, GL_FALSE, glm::value_ptr(
            glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f))
        ));
    glUniform4fv(glGetUniformLocation(shaderProgram, "uColor"), 1, glm::value_ptr(color));

    if (textureID) {
        glUniform1i(glGetUniformLocation(shaderProgram, "uUseTexture"), 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glUniform1i(glGetUniformLocation(shaderProgram, "uTexture"), 0);
    }
    else {
        glUniform1i(glGetUniformLocation(shaderProgram, "uUseTexture"), 0);
    }

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}


void UIRenderSystem::RenderUITextField(const UIElement* element, const UITextField* textField) {
    glm::vec2 pos = element->GetScreenPosition(screenWidth, screenHeight);

    // Render background
    glm::vec4 bgColor = textField->GetCurrentBackgroundColor() * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    RenderQuad(pos, element->size, bgColor);

    // Render border (as a slightly larger quad behind)
    glm::vec4 borderColor = textField->GetCurrentBorderColor() * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    float borderWidth = 2.0f;

    // Top border
    RenderQuad(glm::vec2(pos.x, pos.y - borderWidth),
        glm::vec2(element->size.x, borderWidth), borderColor);
    // Bottom border
    RenderQuad(glm::vec2(pos.x, pos.y + element->size.y),
        glm::vec2(element->size.x, borderWidth), borderColor);
    // Left border
    RenderQuad(glm::vec2(pos.x - borderWidth, pos.y),
        glm::vec2(borderWidth, element->size.y), borderColor);
    // Right border
    RenderQuad(glm::vec2(pos.x + element->size.x, pos.y),
        glm::vec2(borderWidth, element->size.y), borderColor);

    // Calculate text rendering area (with padding)
    glm::vec2 textPos = pos + glm::vec2(textField->padding, textField->padding);
    glm::vec2 textAreaSize = element->size - glm::vec2(textField->padding * 2.0f);

    // Determine what text to render
    std::string displayText = textField->GetDisplayText();
    bool showPlaceholder = displayText.empty() && textField->state != TextFieldState::FOCUSED;

    if (showPlaceholder) {
        displayText = textField->placeholderText;
    }

    if (displayText.empty()) {
        return;
    }

    // Get font info
    const std::string fontName = textField->fontName;
    if (!fontManager || !fontManager->HasFont(fontName)) {
        return;
    }

    // Calculate scale
    const Character* refChar = fontManager->GetCharacter(fontName, 'H');
    if (!refChar) refChar = fontManager->GetCharacter(fontName, 'A');
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = textField->fontSize / loadedFontSize;

    // Setup text rendering
    glUseProgram(textShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));

    glm::vec4 renderColor = showPlaceholder ?
        textField->placeholderColor : textField->textColor;
    renderColor *= glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);

    glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"),
        1, glm::value_ptr(renderColor));

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(textVAO);

    // Calculate baseline
    float maxBearingY = 0.0f;
    for (char c : displayText) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }

    // Center text vertically in the text area
    float baselineY = textPos.y + (textAreaSize.y + maxBearingY * scale) * 0.5f;
    float cursorX = textPos.x;

    // Render selection background if there's a selection
    if (!showPlaceholder && textField->HasSelection() && textField->state == TextFieldState::FOCUSED) {
        size_t selStart = textField->GetSelectionMin();
        size_t selEnd = textField->GetSelectionMax();

        // Calculate X position of selection start
        float selStartX = textPos.x;
        for (size_t i = 0; i < selStart && i < displayText.length(); i++) {
            const Character* ch = fontManager->GetCharacter(fontName, displayText[i]);
            if (ch) {
                selStartX += (ch->advance >> 6) * scale;
            }
        }

        // Calculate width of selection
        float selWidth = 0.0f;
        for (size_t i = selStart; i < selEnd && i < displayText.length(); i++) {
            const Character* ch = fontManager->GetCharacter(fontName, displayText[i]);
            if (ch) {
                selWidth += (ch->advance >> 6) * scale;
            }
        }

        // Render selection background
        glUseProgram(shaderProgram);
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));
        RenderQuad(glm::vec2(selStartX, textPos.y),
            glm::vec2(selWidth, textAreaSize.y),
            textField->selectionColor);

        // Switch back to text shader
        glUseProgram(textShaderProgram);
        glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));
        glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"),
            1, glm::value_ptr(renderColor));
    }

    // Render text
    float cursorRenderX = textPos.x;
    for (size_t i = 0; i < displayText.length(); i++) {
        char c = displayText[i];
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (!ch) continue;

        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;
        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

        // Build vertices
        float vertices[6][4] = {
            { xpos,     ypos + h,   0.0f, 1.0f },
            { xpos + w, ypos,       1.0f, 0.0f },
            { xpos,     ypos,       0.0f, 0.0f },
            { xpos,     ypos + h,   0.0f, 1.0f },
            { xpos + w, ypos + h,   1.0f, 1.0f },
            { xpos + w, ypos,       1.0f, 0.0f }
        };

        glBindTexture(GL_TEXTURE_2D, ch->textureID);
        glUniform1i(glGetUniformLocation(textShaderProgram, "text"), 0);

        glBindBuffer(GL_ARRAY_BUFFER, textVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Track cursor position for rendering cursor
        if (!showPlaceholder && i == textField->cursorPosition - 1) {
            cursorRenderX = cursorX + (ch->advance >> 6) * scale;
        }

        cursorX += (ch->advance >> 6) * scale;
    }

    // If cursor is at position 0, set render position
    if (!showPlaceholder && textField->cursorPosition == 0) {
        cursorRenderX = textPos.x;
    }
    // If cursor is at end and text is not empty
    else if (!showPlaceholder && textField->cursorPosition == displayText.length() && !displayText.empty()) {
        cursorRenderX = cursorX;
    }

    // Render cursor if focused and visible
    if (!showPlaceholder && textField->state == TextFieldState::FOCUSED && textField->cursorVisible) {
        glUseProgram(shaderProgram);
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));

        float cursorWidth = 2.0f;
        float cursorHeight = textField->fontSize;
        glm::vec4 cursorColor = textField->textColor;

        RenderQuad(glm::vec2(cursorRenderX, textPos.y + (textAreaSize.y - cursorHeight) * 0.5f),
            glm::vec2(cursorWidth, cursorHeight),
            cursorColor);
    }

    // Cleanup
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void UIRenderSystem::UpdateButton(Entity entity, UIElement* element, UIButton* button) {
    
}

GLuint UIRenderSystem::CompileShader(const char* source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        Debug::Info("UISystem") << "UI Shader compilation failed: " << infoLog << "\n";
    }
    
    return shader;
}

GLuint UIRenderSystem::CreateTextShaderProgram() {
    GLuint vertexShader = CompileShader(textVertexShader, GL_VERTEX_SHADER);
    GLuint fragmentShader = CompileShader(textFragmentShader, GL_FRAGMENT_SHADER);
    
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        Debug::Info("UISystem") << "Text shader linking failed: " << infoLog << "\n";
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    return program;
}

GLuint UIRenderSystem::CreateShaderProgram() {
    GLuint vertexShader = CompileShader(uiVertexShader, GL_VERTEX_SHADER);
    GLuint fragmentShader = CompileShader(uiFragmentShader, GL_FRAGMENT_SHADER);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        Debug::Info("UISystem") << "UI shader linking failed: " << infoLog << "\n";
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return program;
}