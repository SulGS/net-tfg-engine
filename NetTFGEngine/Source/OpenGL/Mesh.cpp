#include "Mesh.hpp"
#include "Utils/Debug/Debug.hpp"

Mesh::Mesh(const std::string& meshName,
    std::shared_ptr<Material> mat)
    : material(std::move(mat))
{
    buffer = AssetManager::instance().loadAsset<MeshBuffer>(meshName);

    if (!buffer || buffer->VAO == 0) {
        Debug::Error("Mesh") << "Failed to load MeshBuffer: "
            << meshName << "\n";
        buffer.reset();
    }
}

// Mesh.cpp
void Mesh::bindMaterial(const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection) const
{
    if (!material) return;
    material->bind(model, view, projection);
}

void Mesh::draw() const
{
    if (!buffer) return;

    glBindVertexArray(buffer->VAO);
    for (const auto& sm : buffer->subMeshes)
    {
        if (sm.diffuseTex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sm.diffuseTex);
            material->setInt("uAlbedoTex", 0);
        }
        if (sm.normalTex) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, sm.normalTex);
            material->setInt("uNormalTex", 1);
        }
        if (sm.mrTex) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, sm.mrTex);
            material->setInt("uMRTex", 2);
        }
        if (sm.occlusionTex) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, sm.occlusionTex);
            material->setInt("uOcclusionTex", 3);
        }
        if (sm.emissiveTex) {
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, sm.emissiveTex);
            material->setInt("uEmissiveTex", 4);
        }
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