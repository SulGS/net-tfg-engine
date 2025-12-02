#ifndef MESH_HPP
#define MESH_HPP
#include <vector>
#include "OpenGLIncludes.hpp"
#include "ecs/ecs.hpp"

class Mesh {
public:
    // Constructor: provide vertices (x, y, z), indices, and RGB color
    Mesh(const std::vector<float>& verts,
         const std::vector<unsigned int>& inds,
         const float color[3]);

    ~Mesh();

    // Render the mesh
    void render() const;

    // Update vertices dynamically (expects x, y, z for each vertex)
    void updateVertices(const std::vector<float>& newVertices);

    // Access color (RenderSystem will set shader uniform)
    const float* getColor() const { return meshColor; }

private:
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    float meshColor[3];        // RGB

    unsigned int VAO = 0;
    unsigned int VBO = 0;
    unsigned int EBO = 0;

    // Initialize OpenGL buffers for 3D vertices
    void initBuffers();
};

#endif // MESH_HPP