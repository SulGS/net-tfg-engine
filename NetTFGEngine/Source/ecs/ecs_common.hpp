#ifndef ECS_COMMON_H
#define ECS_COMMON_H

#include "netcode/netcode_common.hpp"
#include "ecs.hpp"
#include "OpenGL/Mesh.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

class Transform : public IComponent {
public:
    Transform()
        : position(0.0f, 0.0f, 0.0f)
        , rotation(0.0f, 0.0f, 0.0f)
        , scale(1.0f, 1.0f, 1.0f)
        , modelMatrix(1.0f)
        , dirty(true)
    {}

    // Position
    void setPosition(const glm::vec3& pos) {
        position = pos;
        dirty = true;
    }

    void translate(const glm::vec3& delta) {
        position += delta;
        dirty = true;
    }

    // Smoothly move position toward target using exponential smoothing
    void SmoothPositionToward(const glm::vec3& target, float deltaTime, float speed) {
        if (deltaTime <= 0.0f) return;
        float t = 1.0f - std::exp(-speed * deltaTime);
        position = position + (target - position) * t;
        dirty = true;
    }

    const glm::vec3& getPosition() const { return position; }

    // Rotation (Euler angles in degrees)
    void setRotation(const glm::vec3& rot) {
        rotation = rot;
        dirty = true;
    }

    void rotate(const glm::vec3& delta) {
        rotation += delta;
        dirty = true;
    }

    // Smoothly rotate toward target (Euler degrees) using shortest path per-axis
    void SmoothRotationToward(const glm::vec3& target, float deltaTime, float speed) {
        if (deltaTime <= 0.0f) return;
        float t = 1.0f - std::exp(-speed * deltaTime);
        // Per-axis shortest-angle interpolation
        auto interpAngle = [&](float cur, float tgt) {
            float diff = tgt - cur;
            while (diff > 180.0f) diff -= 360.0f;
            while (diff <= -180.0f) diff += 360.0f;
            return cur + diff * t;
        };
        rotation.x = interpAngle(rotation.x, target.x);
        rotation.y = interpAngle(rotation.y, target.y);
        rotation.z = interpAngle(rotation.z, target.z);
        dirty = true;
    }

    const glm::vec3& getRotation() const { return rotation; }

    // Scale
    void setScale(const glm::vec3& scl) {
        scale = scl;
        dirty = true;
    }

    void setScale(float uniformScale) {
        scale = glm::vec3(uniformScale);
        dirty = true;
    }

    // Smoothly scale toward target using exponential smoothing
    void SmoothScaleToward(const glm::vec3& target, float deltaTime, float speed) {
        if (deltaTime <= 0.0f) return;
        float t = 1.0f - std::exp(-speed * deltaTime);
        scale = scale + (target - scale) * t;
        dirty = true;
    }

    const glm::vec3& getScale() const { return scale; }

    // Model matrix
    const glm::mat4& getModelMatrix() {
        if (dirty) {
            updateModelMatrix();
            dirty = false;
        }
        return modelMatrix;
    }

private:
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;
    glm::mat4 modelMatrix;
    bool dirty;

    void updateModelMatrix() {
        modelMatrix = glm::mat4(1.0f);
        modelMatrix = glm::translate(modelMatrix, position);
        modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.x), glm::vec3(1, 0, 0));
        modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        modelMatrix = glm::scale(modelMatrix, scale);
    }
};

class Camera : public IComponent {
public:
    enum class ProjectionType {
        PERSPECTIVE,
        ORTHOGRAPHIC
    };

    Camera()
        : fov(45.0f)
        , aspectRatio(16.0f / 9.0f)
        , nearPlane(0.1f)
        , farPlane(100.0f)
        , projectionType(ProjectionType::PERSPECTIVE)
        , target(0.0f, 0.0f, 0.0f)
        , up(0.0f, 1.0f, 0.0f)
        , projectionMatrix(1.0f)
        , viewMatrix(1.0f)
        , projectionDirty(true)
        , viewDirty(true)
    {
        updateProjectionMatrix();
    }

    // Projection settings
    void setPerspective(float fovDegrees, float aspect, float n, float f) {
        fov = fovDegrees;
        aspectRatio = aspect;
        nearPlane = n;
        farPlane = f;
        projectionType = ProjectionType::PERSPECTIVE;
        projectionDirty = true;
    }

    void setOrthographic(float left, float right, float bottom, float top, float n, float f) {
        orthoLeft = left;
        orthoRight = right;
        orthoBottom = bottom;
        orthoTop = top;
        nearPlane = n;
        farPlane = f;
        projectionType = ProjectionType::ORTHOGRAPHIC;
        projectionDirty = true;
    }

    void updateAspectRatio(float aspect) {
        aspectRatio = aspect;
        projectionDirty = true;
    }

    // View settings (target and up vector)
    void setTarget(const glm::vec3& tgt) {
        target = tgt;
        viewDirty = true;
    }

    void setUp(const glm::vec3& upVec) {
        up = upVec;
        viewDirty = true;
    }

    const glm::vec3& getTarget() const { return target; }
    const glm::vec3& getUp() const { return up; }

    // Matrix getters
    const glm::mat4& getProjectionMatrix() {
        if (projectionDirty) {
            updateProjectionMatrix();
            projectionDirty = false;
        }
        return projectionMatrix;
    }

    const glm::mat4& getViewMatrix() {
        if (viewDirty) {
            viewDirty = false;
            // View matrix will be updated by CameraSystem using Transform
        }
        return viewMatrix;
    }

    // Called by CameraSystem
    void setViewMatrix(const glm::mat4& view) {
        viewMatrix = view;
    }

    void markViewDirty() {
        viewDirty = true;
    }

    bool isViewDirty() const {
        return viewDirty;
    }

    // Getters
    float getFov() const { return fov; }
    float getAspectRatio() const { return aspectRatio; }
    float getNearPlane() const { return nearPlane; }
    float getFarPlane() const { return farPlane; }
    ProjectionType getProjectionType() const { return projectionType; }

private:
    // Projection parameters
    float fov;
    float aspectRatio;
    float nearPlane;
    float farPlane;
    ProjectionType projectionType;

    // Orthographic parameters
    float orthoLeft, orthoRight, orthoBottom, orthoTop;

    // View parameters
    glm::vec3 target;
    glm::vec3 up;

    // Matrices
    glm::mat4 projectionMatrix;
    glm::mat4 viewMatrix;

    // Dirty flags
    bool projectionDirty;
    bool viewDirty;

    void updateProjectionMatrix() {
        if (projectionType == ProjectionType::PERSPECTIVE) {
            projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
        } else {
            projectionMatrix = glm::ortho(orthoLeft, orthoRight, orthoBottom, orthoTop, nearPlane, farPlane);
        }
    }
};

class Playable : public IComponent {
public:
    int playerId;
    InputBlob input;
    bool isLocal = false;

    Playable() : playerId(-1), input(MakeZeroInputBlob()), isLocal(false) {}
    Playable(int pid, InputBlob input, bool isLocal) : playerId(pid), input(input), isLocal(isLocal) {}
};

class MeshComponent : public IComponent {
public:
    std::unique_ptr<Mesh> mesh;
    bool enabled;
    MeshComponent() : mesh(nullptr), enabled(true) {}
    MeshComponent(Mesh* m) : mesh(m), enabled(true) {}
};

class CameraSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, float deltaTime) override {

        auto query = entityManager.CreateQuery<Camera, Transform>();

        // Find all entities with Camera and Transform components
        for (auto [entity, camera, transform] : query) {

            if (camera && transform) {
                // Update view matrix based on camera position and target
                glm::vec3 position = transform->getPosition();
                glm::vec3 target = camera->getTarget();
                glm::vec3 up = camera->getUp();

                glm::mat4 viewMatrix = glm::lookAt(position, target, up);
                camera->setViewMatrix(viewMatrix);
            }
        }
    }
};



#endif // ECS_COMMON_H