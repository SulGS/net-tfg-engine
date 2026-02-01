#include "Mesh.hpp"
#include "Utils/Debug/Debug.hpp"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
Mesh::Mesh(const std::vector<float>& verts,
    const std::vector<unsigned int>& inds,
    std::shared_ptr<Material> mat)
    : vertices(verts), indices(inds), material(std::move(mat))
{
    initBuffers();
}

Mesh::~Mesh() {
    if (VAO) glDeleteVertexArrays(1, &VAO);
    if (VBO) glDeleteBuffers(1, &VBO);
    if (EBO) glDeleteBuffers(1, &EBO);
}

// ---------------------------------------------------------------------------
// Buffer initialisation (pure geometry, no shader involvement)
// ---------------------------------------------------------------------------
void Mesh::initBuffers() {
    if (vertices.size() % 3 != 0) {
        Debug::Error("Mesh") << "Vertex data must have 3 components (x, y, z) per vertex! "
            << "Current size: " << vertices.size() << "\n";
        return;
    }

    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER,
        vertices.size() * sizeof(float),
        vertices.data(),
        GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    if (!indices.empty()) {
        glGenBuffers(1, &EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            indices.size() * sizeof(unsigned int),
            indices.data(),
            GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Render — material binds the shader + uniforms, then we draw
// ---------------------------------------------------------------------------
void Mesh::render(const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection) const
{
    if (!VAO || !material) {
        Debug::Error("Mesh") << "Cannot render: VAO or material not initialized.\n";
        return;
    }

    // Material handles glUseProgram + all uniform uploads
    material->bind(model, view, projection);

    glBindVertexArray(VAO);

    if (!indices.empty()) {
        glDrawElements(GL_TRIANGLES,
            static_cast<GLsizei>(indices.size()),
            GL_UNSIGNED_INT,
            0);
    }
    else {
        glDrawArrays(GL_TRIANGLES, 0,
            static_cast<GLsizei>(vertices.size() / 3));
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        Debug::Error("Mesh") << "OpenGL error after draw: " << err << "\n";
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

// ---------------------------------------------------------------------------
// Dynamic vertex update
// ---------------------------------------------------------------------------
void Mesh::updateVertices(const std::vector<float>& newVertices) {
    if (newVertices.size() % 3 != 0) {
        Debug::Error("Mesh") << "New vertex data must have 3 components (x, y, z) per vertex!\n";
        return;
    }
    if (!VBO) {
        Debug::Error("Mesh") << "VBO not initialized!\n";
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    size_t newSize = newVertices.size() * sizeof(float);
    size_t oldSize = vertices.size() * sizeof(float);

    if (newSize > oldSize) {
        glBufferData(GL_ARRAY_BUFFER, newSize, newVertices.data(), GL_DYNAMIC_DRAW);
    }
    else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, newSize, newVertices.data());
    }

    vertices = newVertices;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}