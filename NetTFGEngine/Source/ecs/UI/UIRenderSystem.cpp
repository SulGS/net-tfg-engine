#include "UIRenderSystem.hpp"
#include <iostream>
#include <algorithm>
#include "Utils/Debug/Debug.hpp"
#include "Utils/AssetManager.hpp"
#include "Utils/Utf8.hpp"

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

UIRenderSystem::UIRenderSystem(int refWidth, int refHeight)
    : refWidth(refWidth)
    , refHeight(refHeight)
    , screenWidth(refWidth)
    , screenHeight(refHeight)
    , mousePosition(0.0f)
    , mouseDown(false)
    , hoveredButton(0)
    , pressedButton(0)
    , fontManager(std::make_unique<FontManager>())
{
    drawsFrame = true;
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
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLint blendSrc, blendDst;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDst);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(shaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));

    std::vector<std::tuple<Entity, UIElement*, int>> uiElements;

    auto query = entityManager.CreateQuery<UIElement>();
    for (auto [entity, element] : query) {
        if (element->isVisible) {
            uiElements.push_back({ entity, element, element->layer });
        }
    }

    // Sort by layer (lower layers rendered first)
    std::sort(uiElements.begin(), uiElements.end(),
        [](const auto& a, const auto& b) {
            return std::get<2>(a) < std::get<2>(b);
        });

    // Open dropdown popups are deferred to a second pass so they are never
    // covered by elements on higher layers.
    std::vector<std::pair<const UIElement*, const UIDropdown*>> openDropdowns;

    for (const auto& [entity, element, layer] : uiElements) {
        UIButton* button = entityManager.GetComponent<UIButton>(entity);
        if (button) {
            UpdateButton(entity, element, button);
            RenderUIButton(entity, element, button);
        }

        UIImage* image = entityManager.GetComponent<UIImage>(entity);
        if (image) {
            if (!image->isLoaded) {
                auto reqBuffer = AssetManager::instance().loadAsset<TextureID>(image->texturePath);
                if (!reqBuffer)
                {
                    Debug::Error("UIRenderSystem") << "Failed to load texture: " << image->texturePath << "\n";
                }
                else
                {
                    image->textureID = reqBuffer->value;
                    image->isLoaded = true;
                }
            }

            if (image->isLoaded) {
                RenderUIImage(element, image);
            }
        }

        UIText* text = entityManager.GetComponent<UIText>(entity);
        if (text) {
            RenderUIText(element, text);
        }

        UITextField* textField = entityManager.GetComponent<UITextField>(entity);
        if (textField) {
            RenderUITextField(element, textField);
        }

        UISlider* slider = entityManager.GetComponent<UISlider>(entity);
        if (slider) {
            RenderUISlider(element, slider);
        }

        // Header now; popup list is rendered in the second pass below.
        UIDropdown* dropdown = entityManager.GetComponent<UIDropdown>(entity);
        if (dropdown) {
            RenderUIDropdown(element, dropdown);
            if (dropdown->isOpen && !dropdown->options.empty()) {
                openDropdowns.push_back({ element, dropdown });
            }
        }
    }

    // Second pass: open dropdown lists on top of everything else
    for (const auto& [element, dropdown] : openDropdowns) {
        RenderUIDropdownList(element, dropdown);
    }

    glUseProgram(0);

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
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void UIRenderSystem::InitializeQuad() {
    float vertices[] = {
        // Pos      // Tex
        0.0f, 1.0f, 0.0f, 0.0f,  // Changed from 0.0f, 1.0f
        1.0f, 0.0f, 1.0f, 1.0f,  // Changed from 1.0f, 0.0f
        0.0f, 0.0f, 0.0f, 1.0f,  // Changed from 0.0f, 0.0f

        0.0f, 1.0f, 0.0f, 0.0f,  // Changed from 0.0f, 1.0f
        1.0f, 1.0f, 1.0f, 0.0f,  // Changed from 1.0f, 1.0f
        1.0f, 0.0f, 1.0f, 1.0f   // Changed from 1.0f, 0.0f
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
    // Ortho over reference dimensions. The GPU viewport stretches reference-space
    // geometry to fill the actual screen automatically.
    projection = glm::ortho(0.0f, (float)refWidth, (float)refHeight, 0.0f, -1.0f, 1.0f);
}

void UIRenderSystem::RenderUIImage(const UIElement* element, const UIImage* image) {
    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
    glm::vec4 color = image->color * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);

    RenderQuad(pos, element->size, color, image->textureID, image->uvRect);
}

void UIRenderSystem::RenderUIText(const UIElement* element, const UIText* text) {
    if (text->text.empty()) return;

    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);

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
    const Character* refChar = fontManager->GetCharacter(fontName, U'H');
    if (!refChar) {
        refChar = fontManager->GetCharacter(fontName, U'A');
    }

    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = text->fontSize / loadedFontSize;

    glUseProgram(textShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glm::vec4 textColor = text->color * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);
    glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"),
        1, glm::value_ptr(textColor));

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(textVAO);

    // FreeType baseline: bearing.y is the distance from baseline to glyph top.
    float cursorX = pos.x;

    // Horizontal alignment inside the element's rectangle (LEFT starts at its left edge). The width is measured the same way RenderTextInRect does, so both place text identically.
    if (text->align != UITextAlign::LEFT) {
        const float textWidth = MeasureTextWidth(text->text, fontName, text->fontSize);
        const float spare = element->size.x - textWidth;
        cursorX += (text->align == UITextAlign::CENTER) ? spare * 0.5f : spare;
    }

    // Find the maximum bearing.y to establish a consistent baseline
    float maxBearingY = 0.0f;
    for (char32_t c : Utf8::Decode(text->text)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }

    float baselineY = pos.y + (maxBearingY * scale);

    // If element has a height, center the text vertically
    if (element->size.y > 0.0f) {
        float maxHeight = 0.0f;
        float minY = 0.0f;
        for (char32_t c : Utf8::Decode(text->text)) {
            const Character* ch = fontManager->GetCharacter(fontName, c);
            if (ch) {
                float top = ch->bearing.y * scale;
                float bottom = (ch->bearing.y - ch->size.y) * scale;
                maxHeight = std::max(maxHeight, top);
                minY = std::min(minY, bottom);
            }
        }
        float totalTextHeight = maxHeight - minY;

        float yOffset = (element->size.y - totalTextHeight) * 0.5f;
        baselineY = pos.y + yOffset + maxHeight;
    }

    // Render each character
    for (char32_t c : Utf8::Decode(text->text)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (!ch) continue;

        // Calculate glyph position
        // xpos: cursor + bearing offset
        // ypos: baseline - bearing (FreeType's bearing.y is from baseline UP)
        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;

        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

        float vertices[6][4] = {
            { xpos,     ypos + h,   0.0f, 1.0f },  // Bottom-left
            { xpos + w, ypos,       1.0f, 0.0f },  // Top-right
            { xpos,     ypos,       0.0f, 0.0f },  // Top-left

            { xpos,     ypos + h,   0.0f, 1.0f },  // Bottom-left
            { xpos + w, ypos + h,   1.0f, 1.0f },  // Bottom-right
            { xpos + w, ypos,       1.0f, 0.0f }   // Top-right
        };

        glBindTexture(GL_TEXTURE_2D, ch->textureID);
        glUniform1i(glGetUniformLocation(textShaderProgram, "text"), 0);

        glBindBuffer(GL_ARRAY_BUFFER, textVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Advance cursor (advance is in 1/64th of pixels)
        cursorX += (ch->advance >> 6) * scale;
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void UIRenderSystem::RenderUIButton(Entity entity, const UIElement* element, const UIButton* button) {
    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);

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

    glm::vec2 textPos = pos + glm::vec2(button->padding, button->padding);
    glm::vec2 textAreaSize = element->size - glm::vec2(button->padding * 2.0f);

    std::string displayText = button->text;

    if (displayText.empty()) {
        return;
    }

    const std::string fontName = button->fontName;
    if (!fontManager || !fontManager->HasFont(fontName)) {
        return;
    }

    const Character* refChar = fontManager->GetCharacter(fontName, U'H');
    if (!refChar) refChar = fontManager->GetCharacter(fontName, U'A');
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = button->fontSize / loadedFontSize;

    glUseProgram(textShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));

    glm::vec4 renderColor = button->textColor * glm::vec4(1.0f, 1.0f, 1.0f, element->opacity);

    glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"),
        1, glm::value_ptr(renderColor));

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(textVAO);

    float maxBearingY = 0.0f;
    for (char32_t c : Utf8::Decode(displayText)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }

    float textWidth = 0.0f;
    for (char32_t c : Utf8::Decode(displayText)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch) {
            textWidth += (ch->advance >> 6) * scale;
        }
    }

    // Center text horizontally and vertically in the text area
    float baselineY = textPos.y + (textAreaSize.y + maxBearingY * scale) * 0.5f;
    float cursorX = textPos.x + (textAreaSize.x - textWidth) * 0.5f;

    for (char32_t c : Utf8::Decode(displayText)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (!ch) continue;

        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;
        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

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

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void UIRenderSystem::RenderQuad(const glm::vec2& position, const glm::vec2& size,
    const glm::vec4& color, GLuint textureID,
    const glm::vec4& uvRect) {

    glUseProgram(shaderProgram);
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

        GLint width, height;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    }
    else {
        glUniform1i(glGetUniformLocation(shaderProgram, "uUseTexture"), 0);
    }

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (textureID) {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}


void UIRenderSystem::RenderUITextField(const UIElement* element, const UITextField* textField) {
    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);

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

    glm::vec2 textPos = pos + glm::vec2(textField->padding, textField->padding);
    glm::vec2 textAreaSize = element->size - glm::vec2(textField->padding * 2.0f);

    std::string displayText = textField->GetDisplayText();
    bool showPlaceholder = displayText.empty() && textField->state != TextFieldState::FOCUSED;

    if (showPlaceholder) {
        displayText = textField->placeholderText;
    }

    if (displayText.empty()) {
        return;
    }

    const std::string fontName = textField->fontName;
    if (!fontManager || !fontManager->HasFont(fontName)) {
        return;
    }

    const Character* refChar = fontManager->GetCharacter(fontName, U'H');
    if (!refChar) refChar = fontManager->GetCharacter(fontName, U'A');
    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    float scale = textField->fontSize / loadedFontSize;

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

    float maxBearingY = 0.0f;
    for (char32_t c : Utf8::Decode(displayText)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }

    // Center text vertically in the text area
    float baselineY = textPos.y + (textAreaSize.y + maxBearingY * scale) * 0.5f;
    float cursorX = textPos.x;

    if (!showPlaceholder && textField->HasSelection() && textField->state == TextFieldState::FOCUSED) {
        size_t selStart = textField->GetSelectionMin();
        size_t selEnd = textField->GetSelectionMax();

        float selStartX = textPos.x;
        for (size_t i = 0; i < selStart && i < displayText.length();) {
            const Character* ch = fontManager->GetCharacter(fontName, Utf8::Next(displayText, i));
            if (ch) {
                selStartX += (ch->advance >> 6) * scale;
            }
        }

        float selWidth = 0.0f;
        for (size_t i = selStart; i < selEnd && i < displayText.length();) {
            const Character* ch = fontManager->GetCharacter(fontName, Utf8::Next(displayText, i));
            if (ch) {
                selWidth += (ch->advance >> 6) * scale;
            }
        }

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

    float cursorRenderX = textPos.x;
    // Walked by bytes, like cursorPosition: i ends up just past the glyph being drawn.
    for (size_t i = 0; i < displayText.length();) {
        const Character* ch = fontManager->GetCharacter(fontName, Utf8::Next(displayText, i));
        if (!ch) continue;

        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;
        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

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
        if (!showPlaceholder && i == textField->cursorPosition) {
            cursorRenderX = cursorX + (ch->advance >> 6) * scale;
        }

        cursorX += (ch->advance >> 6) * scale;
    }

    if (!showPlaceholder && textField->cursorPosition == 0) {
        cursorRenderX = textPos.x;
    }
    else if (!showPlaceholder && textField->cursorPosition == displayText.length() && !displayText.empty()) {
        cursorRenderX = cursorX;
    }

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

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void UIRenderSystem::UpdateButton(Entity entity, UIElement* element, UIButton* button) {

}

// Shared drawing helpers

void UIRenderSystem::RenderBorder(const glm::vec2& position, const glm::vec2& size,
    const glm::vec4& color, float thickness) {
    if (thickness <= 0.0f) return;

    // Top
    RenderQuad(glm::vec2(position.x, position.y - thickness),
        glm::vec2(size.x, thickness), color);
    // Bottom
    RenderQuad(glm::vec2(position.x, position.y + size.y),
        glm::vec2(size.x, thickness), color);
    // Left
    RenderQuad(glm::vec2(position.x - thickness, position.y),
        glm::vec2(thickness, size.y), color);
    // Right
    RenderQuad(glm::vec2(position.x + size.x, position.y),
        glm::vec2(thickness, size.y), color);
}

void UIRenderSystem::RenderTriangle(const glm::vec2& center, float width, float height,
    bool pointDown, const glm::vec4& color) {
    // The UI shader only draws quads, so the triangle is approximated with
    // a few horizontal slices that shrink towards the tip.
    const int steps = 6;
    float sliceHeight = height / static_cast<float>(steps);

    for (int i = 0; i < steps; i++) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float sliceWidth = width * (1.0f - t);
        float y = pointDown
            ? (center.y - height * 0.5f + static_cast<float>(i) * sliceHeight)
            : (center.y + height * 0.5f - static_cast<float>(i + 1) * sliceHeight);

        RenderQuad(glm::vec2(center.x - sliceWidth * 0.5f, y),
            glm::vec2(sliceWidth, sliceHeight + 0.5f), color);
    }
}

float UIRenderSystem::GetFontScale(const std::string& fontName, float fontSize) {
    if (!fontManager) return 1.0f;

    const Character* refChar = fontManager->GetCharacter(fontName, U'H');
    if (!refChar) refChar = fontManager->GetCharacter(fontName, U'A');

    float loadedFontSize = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
    if (loadedFontSize <= 0.0f) loadedFontSize = 48.0f;

    return fontSize / loadedFontSize;
}

float UIRenderSystem::MeasureTextWidth(const std::string& text, const std::string& fontName,
    float fontSize) {
    if (!fontManager || !fontManager->HasFont(fontName) || text.empty()) return 0.0f;

    float scale = GetFontScale(fontName, fontSize);
    float width = 0.0f;
    for (char32_t c : Utf8::Decode(text)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch) width += (ch->advance >> 6) * scale;
    }
    return width;
}

std::string UIRenderSystem::TruncateTextToWidth(const std::string& text, const std::string& fontName,
    float fontSize, float maxWidth) {
    if (maxWidth <= 0.0f || text.empty()) return text;
    if (MeasureTextWidth(text, fontName, fontSize) <= maxWidth) return text;

    const std::string ellipsis = "...";
    float ellipsisWidth = MeasureTextWidth(ellipsis, fontName, fontSize);
    float scale = GetFontScale(fontName, fontSize);
    float budget = maxWidth - ellipsisWidth;
    if (budget <= 0.0f) return ellipsis;

    std::string result;
    float width = 0.0f;
    for (size_t i = 0; i < text.size();) {
        const size_t start = i;
        const Character* ch = fontManager->GetCharacter(fontName, Utf8::Next(text, i));
        if (!ch) continue;
        float advance = (ch->advance >> 6) * scale;
        if (width + advance > budget) break;
        width += advance;
        result.append(text, start, i - start);
    }
    return result + ellipsis;
}

void UIRenderSystem::RenderTextInRect(const std::string& text, const glm::vec2& rectPos,
    const glm::vec2& rectSize, const std::string& fontName, float fontSize,
    const glm::vec4& color, UITextAlign align) {
    if (text.empty()) return;
    if (!fontManager || !fontManager->HasFont(fontName)) return;

    float scale = GetFontScale(fontName, fontSize);

    glUseProgram(textShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(textShaderProgram, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniform4fv(glGetUniformLocation(textShaderProgram, "textColor"),
        1, glm::value_ptr(color));

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(textVAO);

    // Baseline: use the tallest glyph so every string sits consistently
    float maxBearingY = 0.0f;
    for (char32_t c : Utf8::Decode(text)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (ch && ch->bearing.y > maxBearingY) {
            maxBearingY = static_cast<float>(ch->bearing.y);
        }
    }

    float textWidth = MeasureTextWidth(text, fontName, fontSize);
    float cursorX = rectPos.x;
    if (align == UITextAlign::CENTER) {
        cursorX += (rectSize.x - textWidth) * 0.5f;
    }
    else if (align == UITextAlign::RIGHT) {
        cursorX += (rectSize.x - textWidth);
    }

    float baselineY = rectPos.y + (rectSize.y + maxBearingY * scale) * 0.5f;

    for (char32_t c : Utf8::Decode(text)) {
        const Character* ch = fontManager->GetCharacter(fontName, c);
        if (!ch) continue;

        float xpos = cursorX + ch->bearing.x * scale;
        float ypos = baselineY - ch->bearing.y * scale;
        float w = ch->size.x * scale;
        float h = ch->size.y * scale;

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

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

// Slider

void UIRenderSystem::RenderUISlider(const UIElement* element, const UISlider* slider) {
    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
    glm::vec4 alpha(1.0f, 1.0f, 1.0f, element->opacity);

    // Track
    glm::vec2 trackPos = slider->GetTrackPosition(pos, element->size);
    glm::vec2 trackSize = slider->GetTrackSize(element->size);
    RenderQuad(trackPos, trackSize, slider->GetCurrentTrackColor() * alpha);

    // Filled portion
    glm::vec4 fill = slider->GetFillRect(pos, element->size);
    if (fill.z > 0.0f && fill.w > 0.0f) {
        RenderQuad(glm::vec2(fill.x, fill.y), glm::vec2(fill.z, fill.w),
            slider->GetCurrentFillColor() * alpha);
    }

    // Handle (+ border)
    glm::vec2 handlePos = slider->GetHandlePosition(pos, element->size);
    glm::vec2 handleSize = slider->GetHandleSize();
    RenderQuad(handlePos, handleSize, slider->GetCurrentHandleColor() * alpha);
    RenderBorder(handlePos, handleSize, slider->handleBorderColor * alpha, slider->borderWidth);

    // Value label
    if (slider->showValue) {
        glm::vec4 labelRect = slider->GetValueLabelRect(pos, element->size);
        UITextAlign align = (slider->orientation == SliderOrientation::HORIZONTAL)
            ? UITextAlign::RIGHT : UITextAlign::CENTER;

        RenderTextInRect(slider->GetValueText(),
            glm::vec2(labelRect.x, labelRect.y),
            glm::vec2(labelRect.z, labelRect.w),
            slider->fontName, slider->fontSize,
            slider->textColor * alpha, align);
    }
}

// Dropdown

void UIRenderSystem::RenderUIDropdown(const UIElement* element, const UIDropdown* dropdown) {
    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
    glm::vec4 alpha(1.0f, 1.0f, 1.0f, element->opacity);

    // Header background + border
    RenderQuad(pos, element->size, dropdown->GetCurrentBackgroundColor() * alpha);
    RenderBorder(pos, element->size, dropdown->GetCurrentBorderColor() * alpha,
        dropdown->borderWidth);

    // Header text (selected option or placeholder), clipped with an ellipsis
    glm::vec4 textRect = dropdown->GetTextRect(pos, element->size);
    std::string label = TruncateTextToWidth(dropdown->GetDisplayText(), dropdown->fontName,
        dropdown->fontSize, textRect.z);

    RenderTextInRect(label,
        glm::vec2(textRect.x, textRect.y),
        glm::vec2(textRect.z, textRect.w),
        dropdown->fontName, dropdown->fontSize,
        dropdown->GetCurrentTextColor() * alpha, UITextAlign::LEFT);

    // Arrow: points down when closed, up when open
    glm::vec2 arrowCenter = dropdown->GetArrowCenter(pos, element->size);
    RenderTriangle(arrowCenter, dropdown->arrowSize, dropdown->arrowSize * 0.6f,
        !dropdown->isOpen, dropdown->arrowColor * alpha);
}

void UIRenderSystem::RenderUIDropdownList(const UIElement* element, const UIDropdown* dropdown) {
    if (!dropdown->isOpen || dropdown->options.empty()) return;

    glm::vec2 pos = element->GetScreenPosition(refWidth, refHeight);
    glm::vec4 alpha(1.0f, 1.0f, 1.0f, element->opacity);

    glm::vec4 listRect = dropdown->GetListRect(pos, element->size, refHeight);
    glm::vec2 listPos(listRect.x, listRect.y);
    glm::vec2 listSize(listRect.z, listRect.w);

    // Panel background + border
    RenderQuad(listPos, listSize, dropdown->listBackgroundColor * alpha);
    RenderBorder(listPos, listSize, dropdown->listBorderColor * alpha, dropdown->borderWidth);

    int first = dropdown->scrollOffset;
    int last = std::min(first + dropdown->GetVisibleItemCount(), dropdown->GetOptionCount());

    for (int i = first; i < last; i++) {
        glm::vec4 itemRect = dropdown->GetItemRect(i, pos, element->size, refHeight);
        if (itemRect.w <= 0.0f) continue;

        glm::vec2 itemPos(itemRect.x, itemRect.y);
        glm::vec2 itemSize(itemRect.z, itemRect.w);

        // Highlight: hovered row wins over the selected row
        if (i == dropdown->hoveredIndex) {
            RenderQuad(itemPos, itemSize, dropdown->itemHoverColor * alpha);
        }
        else if (i == dropdown->selectedIndex) {
            RenderQuad(itemPos, itemSize, dropdown->itemSelectedColor * alpha);
        }

        float textWidth = itemSize.x - dropdown->padding * 2.0f;
        std::string label = TruncateTextToWidth(dropdown->options[i], dropdown->fontName,
            dropdown->fontSize, textWidth);

        RenderTextInRect(label,
            glm::vec2(itemPos.x + dropdown->padding, itemPos.y),
            glm::vec2(textWidth, itemSize.y),
            dropdown->fontName, dropdown->fontSize,
            dropdown->itemTextColor * alpha, UITextAlign::LEFT);
    }

    // Scrollbar thumb
    glm::vec4 thumb = dropdown->GetScrollbarRect(pos, element->size, refHeight);
    if (thumb.z > 0.0f && thumb.w > 0.0f) {
        RenderQuad(glm::vec2(thumb.x, thumb.y), glm::vec2(thumb.z, thumb.w),
            dropdown->scrollbarColor * alpha);
    }
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