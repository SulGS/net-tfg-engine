#ifndef RENDER_SYSTEM_HPP
#define RENDER_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Mesh.hpp"
#include "Material.hpp"
#include "OpenGLIncludes.hpp"
#include <iostream>

class RenderSystem : public ISystem {
public:
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // --------------- acquire camera ---------------
        Camera* activeCamera = nullptr;
        Transform* cameraTransform = nullptr;

        entityManager.acquireMutex();

        auto cameraQuery = entityManager.CreateQuery<Camera, Transform>();
        for (auto [entity, camera, transform] : cameraQuery) {
            activeCamera = camera;
            cameraTransform = transform;
            break;
        }

        if (!activeCamera || !cameraTransform) {
            Debug::Warning("RenderSystem") << "No camera found\n";
            entityManager.releaseMutex();
            return;
        }

        glm::mat4 view = activeCamera->getViewMatrix();
        glm::mat4 projection = activeCamera->getProjectionMatrix();

        // --------------- draw all meshes ---------------
        auto meshQuery = entityManager.CreateQuery<MeshComponent, Transform>();

        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled) continue;

            Mesh* mesh = meshC->mesh.get();
            if (!mesh)  continue;

            // If you need to update per-frame user uniforms before draw:
            //   mesh->getMaterial()->setFloat("uTime", deltaTime);
            //   mesh->getMaterial()->setVec3("uColor", glm::vec3(1, 0, 0));

            glm::mat4 model = transform->getModelMatrix();

            // Material binds shader + uploads all uniforms; Mesh issues the draw
            mesh->render(model, view, projection);
        }

        entityManager.releaseMutex();
    }
};

#endif // RENDER_SYSTEM_HPP