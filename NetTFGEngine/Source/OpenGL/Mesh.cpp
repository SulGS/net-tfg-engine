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


void Mesh::render(const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection) const
{
    if (!buffer || !material)
        return;

    material->bind(model, view, projection);
    glBindVertexArray(buffer->VAO);

    for (const auto& sm : buffer->subMeshes)
    {
        if (sm.diffuseTex != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sm.diffuseTex);
            material->setInt("uTexture", 0);
        }
        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, (void*)(size_t)(sm.indexOffset * sizeof(uint32_t)));
    }

    glBindVertexArray(0);
}



