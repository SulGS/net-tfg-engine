#ifndef MESH_HPP
#define MESH_HPP

#include <string>
#include <glm/glm.hpp>
#include "Material.hpp"
#include "Utils/AssetManager.hpp"

class Mesh {
public:
    Mesh(const std::string& meshName,
        std::shared_ptr<Material> material);

    // Mesh.hpp
    void bindMaterial(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    void draw() const;

    // Keep render() as a convenience wrapper for other use cases
    void render(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    // Depth pre-pass — only MVP, no material/texture work
    void drawDepthOnly(const glm::mat4& mvp, GLuint depthShader) const;

	Material* getMaterial() const { return material.get(); }

private:
    std::optional<MeshBuffer> buffer;
    std::shared_ptr<Material> material;
};

#endif // MESH_HPP