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

    void render(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

private:
    std::optional<MeshBuffer> buffer;
    std::shared_ptr<Material> material;
};


#endif // MESH_HPP
