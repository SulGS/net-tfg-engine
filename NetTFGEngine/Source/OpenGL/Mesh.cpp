#include "Mesh.hpp"
#include "Utils/Debug/Debug.hpp"
#include <iostream>

Mesh::Mesh(const std::vector<float>& verts,
           const std::vector<unsigned int>& inds,
           const float color[3])
    : vertices(verts), indices(inds)
{
    meshColor[0] = color[0];
    meshColor[1] = color[1];
    meshColor[2] = color[2];
    
    initBuffers();
}

Mesh::~Mesh() {
    if (VAO) glDeleteVertexArrays(1, &VAO);
    if (VBO) glDeleteBuffers(1, &VBO);
    if (EBO) glDeleteBuffers(1, &EBO);
}

void Mesh::initBuffers() {
    // Validate vertex data (must be multiples of 3: x, y, z)
    if (vertices.size() % 3 != 0) {
        std::cerr << "Error: Vertex data must have 3 components (x, y, z) per vertex!" << std::endl;
        std::cerr << "Current size: " << vertices.size() << std::endl;
        return;
    }

    // Generate and bind VAO
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    // Generate and bind VBO
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, 
                 vertices.size() * sizeof(float), 
                 vertices.data(), 
                 GL_DYNAMIC_DRAW);

    // Set vertex attribute pointer for 3D vertices (x, y, z)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 
                          3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Generate and bind EBO if indices provided
    if (!indices.empty()) {
        glGenBuffers(1, &EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, 
                     indices.size() * sizeof(unsigned int),
                     indices.data(), 
                     GL_STATIC_DRAW);
    }

    // Unbind VAO
    glBindVertexArray(0);
}

void Mesh::render() const {
    if (VAO == 0) {
        Debug::Error("RenderSystem") << "ERROR: VAO is 0, mesh not initialized!\n";
        return;
    }


    glBindVertexArray(VAO);
    
    if (!indices.empty()) {
        glDrawElements(GL_TRIANGLES, 
                       static_cast<GLsizei>(indices.size()), 
                       GL_UNSIGNED_INT, 
                       0);
    } else {
        int vertCount = static_cast<GLsizei>(vertices.size() / 3);
        glDrawArrays(GL_TRIANGLES, 0, vertCount);
    }
    
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        Debug::Error("RenderSystem") << "    ERROR after draw: " << err << "\n";
    }
    
    glBindVertexArray(0);
}

void Mesh::updateVertices(const std::vector<float>& newVertices) {
    if (newVertices.size() % 3 != 0) {
        std::cerr << "Error: New vertex data must have 3 components (x, y, z) per vertex!" << std::endl;
        return;
    }

    if (VBO == 0) {
        std::cerr << "Error: VBO not initialized!" << std::endl;
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    
    // Compare BEFORE updating vertices
    size_t newSize = newVertices.size() * sizeof(float);
    size_t oldSize = vertices.size() * sizeof(float);
    
    if (newSize > oldSize) {
        // Need more space - reallocate
        glBufferData(GL_ARRAY_BUFFER, newSize, newVertices.data(), GL_DYNAMIC_DRAW);
    } else {
        // Can fit in existing buffer - just update
        glBufferSubData(GL_ARRAY_BUFFER, 0, newSize, newVertices.data());
    }
    
    vertices = newVertices;  // Update AFTER comparison
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}