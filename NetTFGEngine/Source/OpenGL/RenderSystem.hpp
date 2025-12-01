#ifndef RENDER_SYSTEM_HPP
#define RENDER_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Mesh.hpp"
#include "OpenGLIncludes.hpp"
#include <iostream>

class RenderSystem : public ISystem {
public:
    RenderSystem() : shaderProgram(0) {
        initializeShaders();
    }

    ~RenderSystem() {
        if (shaderProgram) {
            glDeleteProgram(shaderProgram);
        }
    }

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Camera* activeCamera = nullptr;
    Transform* cameraTransform = nullptr;

    auto query = entityManager.CreateQuery<Camera, Transform>();
    
    for (auto [entity, camera, transform] : query) {
        activeCamera = camera;
        cameraTransform = transform;
        break;
    }

    if (!activeCamera || !cameraTransform) {
        Debug::Warning("RenderSystem") << "No camera found" << "\n";
        return;
    }

    glUseProgram(shaderProgram);

    // Get matrices
    glm::mat4 view = activeCamera->getViewMatrix();
    glm::mat4 projection = activeCamera->getProjectionMatrix();
    
    
    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, glm::value_ptr(projection));

    auto queryMeshes = entityManager.CreateQuery<MeshComponent, Transform>();
    int meshCount = 0;

    for (auto [entity, meshC, transform] : queryMeshes) {
        if(!meshC->enabled) continue;
        meshCount++;
        Mesh* mesh = meshC->mesh.get();

        if (mesh) {

            glm::mat4 model = transform->getModelMatrix();
            
            glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
            glUniform3fv(colorLoc, 1, mesh->getColor());

            mesh->render();
        }
    }

    glUseProgram(0);
}


private:
    unsigned int shaderProgram;
    int modelLoc, viewLoc, projectionLoc, colorLoc;

    void initializeShaders() {
        const char* vertexShaderSource = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        
        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;
        
        void main() {
            gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
        }
        )";

        const char* fragmentShaderSource = R"(
        #version 330 core
        out vec4 FragColor;
        uniform vec3 uColor;
        
        void main() {
            FragColor = vec4(uColor, 1.0);
        }
        )";

        unsigned int vertexShader = compileShader(vertexShaderSource, GL_VERTEX_SHADER);
        unsigned int fragmentShader = compileShader(fragmentShaderSource, GL_FRAGMENT_SHADER);

        shaderProgram = glCreateProgram();
        glAttachShader(shaderProgram, vertexShader);
        glAttachShader(shaderProgram, fragmentShader);
        glLinkProgram(shaderProgram);

        int success;
        char infoLog[512];
        glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shaderProgram, 512, nullptr, infoLog);
            Debug::Error("RenderSystem") << "Shader linking failed: " << infoLog << "\n";
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        modelLoc = glGetUniformLocation(shaderProgram, "uModel");
        viewLoc = glGetUniformLocation(shaderProgram, "uView");
        projectionLoc = glGetUniformLocation(shaderProgram, "uProjection");
        colorLoc = glGetUniformLocation(shaderProgram, "uColor");
    }

    unsigned int compileShader(const char* source, unsigned int type) {
        unsigned int shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        int success;
        char infoLog[512];
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 512, nullptr, infoLog);
            Debug::Error("RenderSystem") << "Shader compilation failed: " << infoLog << "\n";
        }

        return shader;
    }
};

#endif // RENDER_SYSTEM_HPP