#ifndef MESH_HPP
#define MESH_HPP

#include <vector>
#include <memory>
#include "OpenGLIncludes.hpp"
#include "Material.hpp"
#include "ecs/ecs.hpp"

class Mesh {
public:
    // Constructor: geometry + a shared Material
    Mesh(const std::vector<float>& verts,
        const std::vector<unsigned int>& inds,
        std::shared_ptr<Material> material);

    ~Mesh();

    // Render: binds the material, then issues the draw call
    void render(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    // Update vertex data dynamically (expects x, y, z per vertex)
    void updateVertices(const std::vector<float>& newVertices);

    // Access the material (e.g. to set uniforms before drawing)
    Material* getMaterial() const { return material.get(); }

private:
    // Geometry
    std::vector<float>        vertices;
    std::vector<unsigned int> indices;

    // OpenGL buffer handles
    GLuint VAO = 0;
    GLuint VBO = 0;
    GLuint EBO = 0;

    // Shared material (shader + uniforms)
    std::shared_ptr<Material> material;

    void initBuffers();
};

#endif // MESH_HPP