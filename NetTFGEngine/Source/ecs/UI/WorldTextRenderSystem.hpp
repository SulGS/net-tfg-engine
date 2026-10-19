#ifndef WORLDTEXTRENDERSYSTEM_HPP
#define WORLDTEXTRENDERSYSTEM_HPP

#include "ecs/ecs_common.hpp"
#include "WorldText.hpp"
#include "Utils/FontManager.hpp"
#include "Utils/Utf8.hpp"
#include "Utils/Debug/Debug.hpp"
#include "OpenGL/OpenGLIncludes.hpp"
#include <algorithm>
#include <string>
#include <vector>

// Draws every WorldText. Runs after RenderSystem, onto the finished frame in the window (post-processed and upscaled),
// so the glyphs are as sharp as the UI's whatever the render resolution, and before UIRenderSystem, so menus and HUD
// cover it. No depth test: the scene's depth is at render resolution in RenderSystem's own targets, not the window's,
// so world text always shows over the geometry (what labels want anyway).
// Glyphs come from UIRenderSystem's FontManager (SetFontManager), so the same fonts serve both.
class WorldTextRenderSystem : public ISystem {
public:
    WorldTextRenderSystem() {
        drawsFrame = true;
        InitGL();
    }

    ~WorldTextRenderSystem() override {
        if (vbo) glDeleteBuffers(1, &vbo);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (program) glDeleteProgram(program);
    }

    void SetFontManager(const FontManager* fontManager) { fonts = fontManager; }

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        if (!fonts || !program) return;

        Camera* camera = nullptr;
        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        for (auto [entity, cam, camTransform] : camQuery) {
            camera = cam;
            break;   // only one camera supported, like RenderSystem
        }
        if (!camera) return;

        const glm::mat4& view = camera->getViewMatrix();
        const glm::mat4 viewProjection = camera->getProjectionMatrix() * view;
        // Rows of the view rotation: the camera's right and up in world space.
        const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);

        auto textQuery = entityManager.CreateQuery<WorldText, Transform>();
        if (textQuery.Count() == 0) return;

        const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean blend = glIsEnabled(GL_BLEND);
        const GLboolean cull = glIsEnabled(GL_CULL_FACE);
        GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);   // viewport left by RenderSystem's last pass: the whole window
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);                // a non-billboard text may be seen from behind
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(program);
        glUniformMatrix4fv(locViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
        glUniform1i(locTexture, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        for (auto [entity, text, transform] : textQuery) {
            if (!text->visible || text->text.empty() || text->height <= 0.0f) continue;
            if (!fonts->HasFont(text->fontName)) continue;

            glm::vec3 right = cameraRight;
            glm::vec3 up = cameraUp;
            if (!text->faceCamera) {
                const glm::mat4& model = transform->getModelMatrix();
                right = glm::normalize(glm::vec3(model[0]));
                up = glm::normalize(glm::vec3(model[1]));
            }

            const glm::vec3 anchor = transform->getPosition() + text->offset
                + right * text->localOffset.x + up * text->localOffset.y;

            const int glyphCount = BuildGlyphs(*text, anchor, right, up);
            if (glyphCount == 0) continue;

            if (text->shadowColor.a > 0.0f) {
                const float d = text->shadowOffset * text->height;
                DrawGlyphs(glyphCount, text->shadowColor, (right - up) * d);
            }
            DrawGlyphs(glyphCount, text->color, glm::vec3(0.0f));
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        if (depthTest) glEnable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE);
        if (!blend) glDisable(GL_BLEND);
        glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    }

private:
    static constexpr int FLOATS_PER_VERTEX = 5;   // vec3 position + vec2 uv
    static constexpr int FLOATS_PER_GLYPH = 6 * FLOATS_PER_VERTEX;

    const FontManager* fonts = nullptr;

    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    size_t vboCapacityGlyphs = 0;
    GLint locViewProjection = -1;
    GLint locColor = -1;
    GLint locOffset = -1;
    GLint locTexture = -1;

    // Built by BuildGlyphs for the current text: its vertices (also uploaded to vbo) and each glyph's texture.
    std::vector<float> vertices;
    std::vector<GLuint> glyphTextures;

    // Lays the text out in world space and uploads it; returns how many glyph quads it produced.
    int BuildGlyphs(const WorldText& text, const glm::vec3& anchor, const glm::vec3& right, const glm::vec3& up) {
        vertices.clear();
        glyphTextures.clear();

        // Same reference as UIText's fontSize: the loaded size of a capital letter.
        const Character* refChar = fonts->GetCharacter(text.fontName, U'H');
        if (!refChar) refChar = fonts->GetCharacter(text.fontName, U'A');
        float capHeight = refChar ? static_cast<float>(refChar->size.y) : 48.0f;
        if (capHeight <= 0.0f) capHeight = 48.0f;
        const float scale = text.height / capHeight;   // font pixels -> world units

        const std::u32string codepoints = Utf8::Decode(text.text);

        float width = 0.0f;
        for (char32_t c : codepoints) {
            if (const Character* ch = fonts->GetCharacter(text.fontName, c))
                width += (ch->advance >> 6) * scale;
        }

        float cursorX = -text.pivot.x * width;
        const float baselineY = -text.pivot.y * text.height;

        for (char32_t c : codepoints) {
            const Character* ch = fonts->GetCharacter(text.fontName, c);
            if (!ch) continue;

            const float x0 = cursorX + ch->bearing.x * scale;
            const float x1 = x0 + ch->size.x * scale;
            const float y1 = baselineY + ch->bearing.y * scale;          // glyph top (bearing.y is up from the baseline)
            const float y0 = y1 - ch->size.y * scale;                    // glyph bottom
            cursorX += (ch->advance >> 6) * scale;

            if (ch->size.x == 0 || ch->size.y == 0) continue;            // spaces: advance only

            // FreeType bitmaps are uploaded top row first: v = 0 is the glyph's top.
            const glm::vec3 bl = anchor + right * x0 + up * y0;
            const glm::vec3 br = anchor + right * x1 + up * y0;
            const glm::vec3 tl = anchor + right * x0 + up * y1;
            const glm::vec3 tr = anchor + right * x1 + up * y1;
            PushVertex(bl, 0.0f, 1.0f);
            PushVertex(tr, 1.0f, 0.0f);
            PushVertex(tl, 0.0f, 0.0f);
            PushVertex(bl, 0.0f, 1.0f);
            PushVertex(br, 1.0f, 1.0f);
            PushVertex(tr, 1.0f, 0.0f);
            glyphTextures.push_back(ch->textureID);
        }

        const size_t glyphCount = glyphTextures.size();
        if (glyphCount == 0) return 0;

        if (glyphCount > vboCapacityGlyphs) {
            vboCapacityGlyphs = std::max(glyphCount, vboCapacityGlyphs * 2);
            glBufferData(GL_ARRAY_BUFFER, vboCapacityGlyphs * FLOATS_PER_GLYPH * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(float), vertices.data());
        return static_cast<int>(glyphCount);
    }

    void PushVertex(const glm::vec3& p, float u, float v) {
        vertices.insert(vertices.end(), { p.x, p.y, p.z, u, v });
    }

    // One draw per glyph: every glyph is its own texture in FontManager.
    void DrawGlyphs(int glyphCount, const glm::vec4& color, const glm::vec3& worldOffset) {
        glUniform4fv(locColor, 1, glm::value_ptr(color));
        glUniform3fv(locOffset, 1, glm::value_ptr(worldOffset));
        for (int i = 0; i < glyphCount; i++) {
            glBindTexture(GL_TEXTURE_2D, glyphTextures[i]);
            glDrawArrays(GL_TRIANGLES, i * 6, 6);
        }
    }

    void InitGL() {
        const char* vs = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;

uniform mat4 uViewProjection;
uniform vec3 uOffset;   // world-space shift of the whole text (drop shadow)

out vec2 TexCoord;

void main() {
    gl_Position = uViewProjection * vec4(aPos + uOffset, 1.0);
    TexCoord = aTexCoord;
}
)";
        const char* fs = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uGlyph;   // single channel coverage (FontManager)
uniform vec4 uColor;

void main() {
    FragColor = vec4(uColor.rgb, uColor.a * texture(uGlyph, TexCoord).r);
}
)";
        const GLuint v = Compile(vs, GL_VERTEX_SHADER);
        const GLuint f = Compile(fs, GL_FRAGMENT_SHADER);
        if (v && f) {
            program = glCreateProgram();
            glAttachShader(program, v);
            glAttachShader(program, f);
            glLinkProgram(program);
            GLint ok = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                Debug::Error("WorldTextRenderSystem") << "Link failed: " << log << "\n";
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);

        if (program) {
            locViewProjection = glGetUniformLocation(program, "uViewProjection");
            locColor = glGetUniformLocation(program, "uColor");
            locOffset = glGetUniformLocation(program, "uOffset");
            locTexture = glGetUniformLocation(program, "uGlyph");
        }

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        vboCapacityGlyphs = 64;
        glBufferData(GL_ARRAY_BUFFER, vboCapacityGlyphs * FLOATS_PER_GLYPH * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    static GLuint Compile(const char* source, GLenum type) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            Debug::Error("WorldTextRenderSystem") << "Shader compile failed: " << log << "\n";
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }
};

#endif // WORLDTEXTRENDERSYSTEM_HPP
