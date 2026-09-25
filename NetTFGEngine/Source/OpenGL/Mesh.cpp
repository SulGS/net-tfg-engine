#include "Mesh.hpp"
#include "Render pipeline/DefaultShader.hpp"
#include "Utils/Debug/Debug.hpp"

GLuint CreateFallback1x1(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    uint8_t px[4] = { r, g, b, a };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

// The default material lives here rather than in a Mesh member so that all default meshes share one, and it is released explicitly (ReleaseDefaultMaterial) while the GL context is still alive: destroying it at static-destruction time would call glDeleteProgram with no context.
static std::shared_ptr<Material>& defaultMaterialSlot()
{
    static std::shared_ptr<Material> slot;
    return slot;
}

void Mesh::InitDefaultMaterial()
{
    auto& slot = defaultMaterialSlot();
    if (slot) return;

    DefaultShader::Register();
    slot = std::make_shared<Material>(DefaultShader::VertexKey, DefaultShader::FragmentKey);
    Debug::Info("Mesh") << "Default shader ready\n";
}

void Mesh::ReleaseDefaultMaterial()
{
    defaultMaterialSlot().reset();
}

std::shared_ptr<Material> Mesh::DefaultMaterial()
{
    // Normally built at engine startup; this covers a mesh created before that.
    if (!defaultMaterialSlot()) InitDefaultMaterial();
    return defaultMaterialSlot();
}

Mesh::Mesh(const std::string& meshName,
    std::shared_ptr<Material> mat)
    : material(mat ? std::move(mat) : DefaultMaterial())
{
	this->meshName = meshName;

    buffer = AssetManager::instance().loadAsset<MeshBuffer>(meshName);

    if (!buffer || buffer->VAO == 0) {
        Debug::Error("Mesh") << "Failed to load MeshBuffer: "
            << meshName << "\n";
        buffer.reset();
    }

    m_fallbackWhite = CreateFallback1x1(255, 255, 255, 255); // albedo, occlusion
    m_fallbackBlack = CreateFallback1x1(0, 0, 0, 255); // emissive
    m_fallbackNormal = CreateFallback1x1(128, 128, 255, 255); // flat normal
    m_fallbackMR = CreateFallback1x1(0, 255, 0, 255); // metallic=0, roughness=1
}

Mesh::~Mesh()
{
	AssetManager::instance().unloadAsset<MeshBuffer>(this->meshName);

	if (m_fallbackWhite) glDeleteTextures(1, &m_fallbackWhite);
	if (m_fallbackBlack) glDeleteTextures(1, &m_fallbackBlack);
	if (m_fallbackNormal) glDeleteTextures(1, &m_fallbackNormal);
	if (m_fallbackMR) glDeleteTextures(1, &m_fallbackMR);
}

void Mesh::bindMaterial(const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection) const
{
    if (!material) return;
    material->bind(model, view, projection);
}

void Mesh::draw() const
{
    if (!buffer || !material) return;

    // Which PBR texture slots this shader actually uses. Unread samplers are stripped by the compiler (e.g. lava/water
    // have none), so binding them would be wasted work and, without this check, a "uniform not found" warning per frame.
    const bool useAlbedo    = material->hasUniform("uAlbedoTex");
    const bool useNormal    = material->hasUniform("uNormalTex");
    const bool useMR        = material->hasUniform("uMRTex");
    const bool useOcclusion = material->hasUniform("uOcclusionTex");
    const bool useEmissive  = material->hasUniform("uEmissiveTex");

    glBindVertexArray(buffer->VAO);
    for (const auto& sm : buffer->subMeshes)
    {
        // Unit 0 - albedo (fallback: white)
        if (useAlbedo) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sm.diffuseTex ? sm.diffuseTex : m_fallbackWhite);
            material->setIntIfPresent("uAlbedoTex", 0);
        }

        // Unit 1 - normal map (fallback: flat normal 0.5, 0.5, 1.0)
        if (useNormal) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, sm.normalTex ? sm.normalTex : m_fallbackNormal);
            material->setIntIfPresent("uNormalTex", 1);
        }

        // Unit 2 - metallic/roughness (fallback: non-metal, full rough)
        if (useMR) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, sm.mrTex ? sm.mrTex : m_fallbackMR);
            material->setIntIfPresent("uMRTex", 2);
        }

        // Unit 3 - occlusion (fallback: full white = no occlusion)
        if (useOcclusion) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, sm.occlusionTex ? sm.occlusionTex : m_fallbackWhite);
            material->setIntIfPresent("uOcclusionTex", 3);
        }

        // Unit 4 - emissive (fallback: black = no emission)
        if (useEmissive) {
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, sm.emissiveTex ? sm.emissiveTex : m_fallbackBlack);
            material->setIntIfPresent("uEmissiveTex", 4);
        }

        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT,
            (void*)(size_t)(sm.indexOffset * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}

void Mesh::drawGeometryOnly() const
{
    if (!buffer) return;
    glBindVertexArray(buffer->VAO);
    for (const auto& sm : buffer->subMeshes)
        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT,
            (void*)(size_t)(sm.indexOffset * sizeof(uint32_t)));
    glBindVertexArray(0);
}

void Mesh::drawGBuffer(GLuint gbufferShader) const
{
    if (!buffer) return;
    glBindVertexArray(buffer->VAO);
    for (const auto& sm : buffer->subMeshes)
    {
        // Unit 1 — normal map (fallback: flat normal 0.5, 0.5, 1.0)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sm.normalTex ? sm.normalTex : m_fallbackNormal);
        glUniform1i(glGetUniformLocation(gbufferShader, "uNormalTex"), 1);

        // Unit 2 — metallic/roughness (fallback: non-metal, full rough)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sm.mrTex ? sm.mrTex : m_fallbackMR);
        glUniform1i(glGetUniformLocation(gbufferShader, "uMRTex"), 2);

        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT,
            (void*)(size_t)(sm.indexOffset * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}

void Mesh::render(const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection) const
{
    if (!buffer || !material) return;
    bindMaterial(model, view, projection);
    draw();
}

void Mesh::drawDepthOnly(const glm::mat4& mvp, GLuint depthShader) const
{
    if (!buffer)
        return;

    // Caller already bound depthShader via glUseProgram
    GLint loc = glGetUniformLocation(depthShader, "uMVP");
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(mvp));

    glBindVertexArray(buffer->VAO);
    for (const auto& sm : buffer->subMeshes)
        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT,
            (void*)(size_t)(sm.indexOffset * sizeof(uint32_t)));

    glBindVertexArray(0);
}